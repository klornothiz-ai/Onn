// ============================================================================
// ProsperoLayer RDNA2 Core - headless AudioOut backend (round 8, item #2)
// ----------------------------------------------------------------------------
// Without SDL2 the shipped AudioOut implementation (libs/audio.cpp) and its
// registration lib (libs/libAudio.cpp) are excluded from the build, so the
// guest had NO sceAudioOut surface at all on a headless host: InitAudio_1
// was a no-op and dynlib_dlsym could not resolve a single audio symbol.
//
// This translation unit is compiled ONLY when SDL2 is absent (see the
// Makefile's HAVE_SDL branch, mirroring libs/optional_stubs_sdl.cpp) and
// closes that gap end-to-end:
//
//   guest sceAudioOut* (HLE, via dynlib_dlsym)
//     -> Libs::Audio::AudioOut::* entry points  (this file)
//     -> PS5::Audio::HeadlessAudioSink          (real decode/accounting)
//     -> observable stats + captured float stream (tests / host device feed)
//
// The entry-point semantics follow libs/audio.cpp where the host has no
// device: AudioOutOpen validates port type / format / free-slot exactly like
// the SDL backend (1-based handles, OUT_PORTS_MAX ports), and AudioOutOutput
// / AudioOutOutputs submit full grains of interleaved guest PCM to the sink.
// Divergences are documented inline and are fail-closed (a typed Sony error
// instead of an EXIT) so tests can exercise every rejection path.
// ============================================================================

#include "common/abi.h"
#include "libs/audio.h"
#include "libs/audio_headless.h"
#include "libs/libs.h"
#include "libs/ps_errno.h"
#include "loader/symbolDatabase.h"

#include <cstdint>

namespace Libs {

namespace {
// Port-type constants and the format encoding mirror libs/audio.cpp (the
// SDL backend) so both backends accept the same guest arguments.
constexpr int AUDIO_OUT_PORT_TYPE_MAIN      = 0;
constexpr int AUDIO_OUT_PORT_TYPE_BGM       = 1;
constexpr int AUDIO_OUT_PORT_TYPE_VOICE     = 2;
constexpr int AUDIO_OUT_PORT_TYPE_PERSONAL  = 3;
constexpr int AUDIO_OUT_PORT_TYPE_PADSPK    = 4;
constexpr int AUDIO_OUT_PORT_TYPE_VIBRATION = 10;
constexpr int AUDIO_OUT_PORT_TYPE_AUDIO3D   = 126;
constexpr int AUDIO_OUT_PORT_TYPE_AUX       = 127;

constexpr uint32_t AUDIO_OUT_PARAM_FORMAT_MASK = 0x000000ffu;

// Same port budget as the SDL backend (Audio::OUT_PORTS_MAX).
constexpr int OUT_PORTS_MAX = 32;

bool audio_out_port_type_is_valid(int type) {
        return (type >= AUDIO_OUT_PORT_TYPE_MAIN && type <= AUDIO_OUT_PORT_TYPE_PADSPK) ||
               type == AUDIO_OUT_PORT_TYPE_VIBRATION || type == AUDIO_OUT_PORT_TYPE_AUDIO3D ||
               type == AUDIO_OUT_PORT_TYPE_AUX;
}

// The low byte of AudioOutOpen's param encodes the sample format; the values
// are exactly PS5::Audio::SampleFormat's enumerators (0..7).
PS5::Audio::SampleFormat audio_out_format_from_param(uint32_t param) {
        switch (param & AUDIO_OUT_PARAM_FORMAT_MASK) {
                case 0: return PS5::Audio::SampleFormat::S16Mono;
                case 1: return PS5::Audio::SampleFormat::S16Stereo;
                case 2: return PS5::Audio::SampleFormat::S16_8Ch;
                case 3: return PS5::Audio::SampleFormat::FloatMono;
                case 4: return PS5::Audio::SampleFormat::FloatStereo;
                case 5: return PS5::Audio::SampleFormat::Float8Ch;
                case 6: return PS5::Audio::SampleFormat::S16_8ChStd;
                case 7: return PS5::Audio::SampleFormat::Float8ChStd;
                default: return PS5::Audio::SampleFormat::Unknown;
        }
}

struct HeadlessAudioPort {
        bool in_use{false};
        int  type{AUDIO_OUT_PORT_TYPE_MAIN};
        PS5::Audio::HeadlessAudioSink sink;
};

// Single-threaded guest-submit model, like the headless GPU bridge. Ports are
// indexed by guest handle - 1 (handles are 1-based, as in the SDL backend).
HeadlessAudioPort g_headless_out_ports[OUT_PORTS_MAX];

bool headless_port_valid(int handle) {
        return handle >= 1 && handle <= OUT_PORTS_MAX && g_headless_out_ports[handle - 1].in_use;
}
} // namespace

namespace LibAudioOutHeadless {

LIB_VERSION("AudioOut", 1, "AudioOut", 1, 1);

namespace AudioOut = Libs::Audio::AudioOut;

// The same NIDs libs/libAudio.cpp registers for the AudioOut module under
// SDL2 -- the guest resolves the identical symbol set on a headless host.
LIB_DEFINE(InitAudio_1_AudioOut_Headless) {
        LIB_FUNC("JfEPXVxhFqA", AudioOut::AudioOutInit);
        LIB_FUNC("ekNvsT22rsY", AudioOut::AudioOutOpen);
        LIB_FUNC("b+uAV89IlxE", AudioOut::AudioOutSetVolume);
        LIB_FUNC("w3PdaSTSwGE", AudioOut::AudioOutOutputs);
        LIB_FUNC("QOQtbeDqsT4", AudioOut::AudioOutOutput);
        LIB_FUNC("s1--uE9mBFw", AudioOut::AudioOutClose);
        LIB_FUNC("GrQ9s4IrNaQ", AudioOut::AudioOutGetPortState);
}

} // namespace LibAudioOutHeadless

namespace Audio::AudioOut {

// PRINT_NAME support for the guest-facing entry points below (same placement
// as libs/audio.cpp: LIB_NAME inside the AudioOut namespace).
LIB_NAME("AudioOut", "AudioOut")

// Guest ABI structs. libs/audio.h forward-declares them exactly here; the
// SDL TU (libs/audio.cpp) defines its own copies and is never compiled
// together with this TU, so there is exactly one definition per link
// configuration.
struct AudioOutOutputParam {
        int         handle;
        const void* ptr;
};

struct AudioOutPortState {
        uint16_t output;
        uint8_t  channel;
        uint8_t  reserved1[1];
        int16_t  volume;
        uint16_t reroute_counter;
        uint64_t flag;
        uint64_t reserved2[2];
};

int KYTY_SYSV_ABI AudioOutInit() {
        PRINT_NAME();

        return OK;
}

int KYTY_SYSV_ABI AudioOutOpen(int user_id, int type, int index, uint32_t len, uint32_t freq,
                               uint32_t param) {
        PRINT_NAME();

        (void)user_id;

        if (!audio_out_port_type_is_valid(type)) {
                return AUDIO_OUT_ERROR_INVALID_PORT_TYPE;
        }
        // The SDL backend EXIT_NOT_IMPLEMENTEDs a non-zero index; a headless
        // backend must stay callable from tests, so it fails closed with a typed
        // error instead of terminating the process.
        if (index != 0) {
                return AUDIO_OUT_ERROR_INVALID_PORT_TYPE;
        }

        const auto format = audio_out_format_from_param(param);
        if (format == PS5::Audio::SampleFormat::Unknown) {
                return AUDIO_OUT_ERROR_INVALID_FORMAT;
        }

        for (int id = 1; id <= OUT_PORTS_MAX; ++id) {
                HeadlessAudioPort& port = g_headless_out_ports[id - 1];
                if (port.in_use) {
                        continue;
                }

                PS5::Audio::AudioPortConfig config{};
                config.format     = format;
                config.sample_rate = freq;
                config.grain      = len;
                if (!port.sink.Open(config)) {
                        return AUDIO_OUT_ERROR_INVALID_FORMAT;
                }
                port.in_use = true;
                port.type   = type;
                return id; // 1-based handle, matching the SDL backend's Id::ToInt()
        }

        return AUDIO_OUT_ERROR_PORT_FULL;
}

int KYTY_SYSV_ABI AudioOutClose(int handle) {
        PRINT_NAME();

        if (!headless_port_valid(handle)) {
                return AUDIO_OUT_ERROR_INVALID_PORT;
        }

        HeadlessAudioPort& port = g_headless_out_ports[handle - 1];
        port.sink.Close();
        port.in_use = false;
        return OK;
}

int KYTY_SYSV_ABI AudioOutOutput(int handle, const void* ptr) {
        PRINT_NAME();

        if (!headless_port_valid(handle)) {
                return AUDIO_OUT_ERROR_INVALID_PORT;
        }
        if (ptr == nullptr) {
                return AUDIO_OUT_ERROR_INVALID_POINTER;
        }

        HeadlessAudioPort& port   = g_headless_out_ports[handle - 1];
        const uint32_t     grain  = port.sink.Config().grain;
        const int          frames = port.sink.Output(ptr, grain);
        if (frames < 0) {
                return AUDIO_OUT_ERROR_INVALID_SIZE;
        }
        // Sony ABI: the number of samples (frames) mixed on success.
        return frames;
}

int KYTY_SYSV_ABI AudioOutOutputs(AudioOutOutputParam* param, uint32_t num) {
        PRINT_NAME();

        if (param == nullptr || num == 0) {
                return AUDIO_OUT_ERROR_INVALID_POINTER;
        }

        // Whole-batch validation first: one invalid port rejects the entire call
        // before any sink side effect (all-or-nothing, like the PM4 path).
        for (uint32_t i = 0; i < num; ++i) {
                if (!headless_port_valid(param[i].handle)) {
                        return AUDIO_OUT_ERROR_INVALID_PORT;
                }
                if (param[i].ptr == nullptr) {
                        return AUDIO_OUT_ERROR_INVALID_POINTER;
                }
        }

        for (uint32_t i = 0; i < num; ++i) {
                HeadlessAudioPort& port  = g_headless_out_ports[param[i].handle - 1];
                const uint32_t     grain = port.sink.Config().grain;
                if (port.sink.Output(param[i].ptr, grain) < 0) {
                        return AUDIO_OUT_ERROR_INVALID_SIZE;
                }
        }

        // Sony ABI: the number of output operations performed.
        return static_cast<int>(num);
}

int KYTY_SYSV_ABI AudioOutGetPortState(int handle, AudioOutPortState* state) {
        PRINT_NAME();

        if (!headless_port_valid(handle)) {
                return AUDIO_OUT_ERROR_INVALID_PORT;
        }
        if (state == nullptr) {
                return AUDIO_OUT_ERROR_INVALID_POINTER;
        }

        const HeadlessAudioPort& port     = g_headless_out_ports[handle - 1];
        const uint32_t           channels = PS5::Audio::HeadlessAudioSink::ChannelsForFormat(
        port.sink.Config().format);

        state->reroute_counter = 0;
        state->volume          = 127;
        state->flag            = 0;
        state->reserved2[0]    = 0;
        state->reserved2[1]    = 0;

        // Same type -> output/channel mapping as the SDL backend.
        switch (port.type) {
                case AUDIO_OUT_PORT_TYPE_MAIN:
                case AUDIO_OUT_PORT_TYPE_BGM:
                case AUDIO_OUT_PORT_TYPE_AUDIO3D:
                        state->output  = 1;
                        state->channel = static_cast<uint8_t>(channels > 2 ? 2 : channels);
                        break;
                case AUDIO_OUT_PORT_TYPE_VOICE:
                case AUDIO_OUT_PORT_TYPE_PERSONAL:
                        state->output  = 0x40;
                        state->channel = 1;
                        break;
                case AUDIO_OUT_PORT_TYPE_PADSPK:
                case AUDIO_OUT_PORT_TYPE_VIBRATION:
                        state->output  = 4;
                        state->channel = 1;
                        break;
                case AUDIO_OUT_PORT_TYPE_AUX:
                        state->output  = 0x80;
                        state->channel = 0;
                        break;
                default: return AUDIO_OUT_ERROR_INVALID_PORT;
        }

        return OK;
}

int KYTY_SYSV_ABI AudioOutSetVolume(int handle, uint32_t flag, int* vol) {
        PRINT_NAME();

        (void)flag;

        if (!headless_port_valid(handle)) {
                return AUDIO_OUT_ERROR_INVALID_PORT;
        }
        if (vol == nullptr) {
                return AUDIO_OUT_ERROR_INVALID_POINTER;
        }

        // Volume is applied by the host device path; the sink path is full-scale
        // by construction. Accept and report success like the SDL backend.
        return OK;
}

const PS5::Audio::AudioSinkStats* HeadlessAudioOutStats(int handle) {
        if (!headless_port_valid(handle)) {
                return nullptr;
        }
        return &g_headless_out_ports[handle - 1].sink.Stats();
}

const std::vector<float>* HeadlessAudioOutCaptured(int handle) {
        if (!headless_port_valid(handle)) {
                return nullptr;
        }
        return &g_headless_out_ports[handle - 1].sink.CapturedFloat();
}

} // namespace Audio::AudioOut
} // namespace Libs
