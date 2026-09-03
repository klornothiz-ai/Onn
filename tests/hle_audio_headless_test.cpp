// ============================================================================
// ProsperoLayer RDNA2 Core - HLE headless AudioOut test (round 8, item #2)
// ----------------------------------------------------------------------------
// Drives the guest-facing AudioOut entry points (libs/audio_headless.cpp,
// the SDL-free backend) end-to-end over the real HeadlessAudioSink:
//
//   AudioOutInit / AudioOutOpen / AudioOutOutput / AudioOutOutputs /
//   AudioOutClose / AudioOutGetPortState / AudioOutSetVolume
//
// Proves: NID registration (the guest dlsym resolution path works headless),
// fail-closed open/output validation, real S16 + float PCM decode through a
// full-scale sine whose measured RMS matches amplitude/sqrt(2), byte/frame
// accounting, all-or-nothing multi-port submission, port-state reporting,
// close/reopen semantics, and port-budget exhaustion. No SDL / no audio
// device required.
// ============================================================================

#include "libs/audio.h"
#include "libs/audio_headless.h"
#include "libs/ps_errno.h"
#include "loader/symbolDatabase.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {
int g_failures = 0, g_checks = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) { ++g_failures; std::cerr << "  [FAIL] line " << line << ": " << e << '\n'; }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

// Guest-ABI mirrors. libs/audio.h only forward-declares these; the layouts
// are identical to the backend's definitions (16 / 32 bytes).
struct TestOutputParam {
    int         handle;
    const void* ptr;
};
struct TestPortState {
    uint16_t output;
    uint8_t  channel;
    uint8_t  reserved1[1];
    int16_t  volume;
    uint16_t reroute_counter;
    uint64_t flag;
    uint64_t reserved2[2];
};

constexpr double kSqrtHalf = 0.70710678118654752; // 1/sqrt(2)
constexpr double kTwoPi    = 6.28318530717958648;  // 2*pi (M_PI is not standard C++)

bool Near(double got, double want, double tol) {
    return std::fabs(got - want) <= tol;
}
} // namespace

int main() {
    using namespace Libs::Audio;            // AUDIO_OUT_ERROR_* constants (ps_errno.h)
    using namespace Libs::Audio::AudioOut;
    using PS5::Audio::HeadlessAudioSink;
    std::cout << "=== HLE headless AudioOut Test (round 8, item #2) ===\n";

    // --- NID registration: the guest dlsym path resolves headless ------------
    // (dynlib_dlsym resolves through SymbolDatabase::FindSymbol, keyed by the
    // NID -- exactly what LIB_FUNC registration fills.)
    {
        Loader::SymbolDatabase& db = Loader::SymbolDatabase::Instance();
        Libs::LibAudioOutHeadless::InitAudio_1_AudioOut_Headless(&db);
        CHECK(db.FindSymbol("JfEPXVxhFqA") != nullptr); // AudioOutInit
        CHECK(db.FindSymbol("ekNvsT22rsY") != nullptr); // AudioOutOpen
        CHECK(db.FindSymbol("b+uAV89IlxE") != nullptr); // AudioOutSetVolume
        CHECK(db.FindSymbol("w3PdaSTSwGE") != nullptr); // AudioOutOutputs
        CHECK(db.FindSymbol("QOQtbeDqsT4") != nullptr); // AudioOutOutput
        CHECK(db.FindSymbol("s1--uE9mBFw") != nullptr); // AudioOutClose
        CHECK(db.FindSymbol("GrQ9s4IrNaQ") != nullptr); // AudioOutGetPortState
        // The NID must resolve to THIS backend's entry point, not just to any
        // non-null address.
        CHECK(db.FindSymbol("QOQtbeDqsT4") ==
              reinterpret_cast<const void*>(&AudioOutOutput));
        std::cout << "  [ok] all 7 AudioOut NIDs resolve via SymbolDatabase (dlsym path)\n";
    }

    // --- init + open validation ----------------------------------------------
    CHECK(AudioOutInit() == OK);
    CHECK(AudioOutOpen(1000, -1, 0, 256, 48000, 1) == AUDIO_OUT_ERROR_INVALID_PORT_TYPE);
    CHECK(AudioOutOpen(1000, 5, 0, 256, 48000, 1) == AUDIO_OUT_ERROR_INVALID_PORT_TYPE);
    CHECK(AudioOutOpen(1000, 0, 1, 256, 48000, 1) == AUDIO_OUT_ERROR_INVALID_PORT_TYPE);
    CHECK(AudioOutOpen(1000, 0, 0, 256, 48000, 99) == AUDIO_OUT_ERROR_INVALID_FORMAT);

    // --- port lifecycle: MAIN S16-stereo, MAIN float-stereo, VOICE S16-mono --
    const int h1 = AudioOutOpen(1000, 0 /*MAIN*/, 0, 256, 48000, 1 /*S16Stereo*/);
    CHECK(h1 == 1); // 1-based handles, first-free slot
    const int h2 = AudioOutOpen(1000, 0 /*MAIN*/, 0, 256, 48000, 4 /*FloatStereo*/);
    CHECK(h2 == 2);
    const int h3 = AudioOutOpen(1000, 2 /*VOICE*/, 0, 256, 48000, 0 /*S16Mono*/);
    CHECK(h3 == 3);

    // --- output validation -----------------------------------------------------
    {
        std::vector<int16_t> pcm(512);
        CHECK(AudioOutOutput(42, pcm.data()) == AUDIO_OUT_ERROR_INVALID_PORT);
        CHECK(AudioOutOutput(h1, nullptr) == AUDIO_OUT_ERROR_INVALID_POINTER);
        CHECK(AudioOutOutputs(nullptr, 2) == AUDIO_OUT_ERROR_INVALID_POINTER);
    }

    // --- S16 stereo sine through the sink (the core wiring proof) -------------
    {
        // Full-scale 750 Hz sine on both channels, 256 frames.
        std::vector<int16_t> pcm(512);
        for (uint32_t i = 0; i < 256; ++i) {
            const double s = std::sin(kTwoPi * 750.0 * i / 48000.0);
            const int16_t v = static_cast<int16_t>(32767.0 * s);
            pcm[2 * i]     = v; // L
            pcm[2 * i + 1] = v; // R
        }
        CHECK(AudioOutOutput(h1, pcm.data()) == 256);

        const PS5::Audio::AudioSinkStats* st = HeadlessAudioOutStats(h1);
        CHECK(st != nullptr);
        CHECK(st->output_calls == 1);
        CHECK(st->frames_written == 256);
        CHECK(st->samples_written == 512);
        CHECK(st->bytes_consumed == 256 * 4); // 2ch * int16
        CHECK(Near(st->last_rms, kSqrtHalf * (32767.0 / 32768.0), 0.002));
        CHECK(Near(st->peak, 32767.0 / 32768.0, 0.002));
        CHECK(Near(st->duration_seconds, 256.0 / 48000.0, 1e-9));
        const std::vector<float>* cap = HeadlessAudioOutCaptured(h1);
        CHECK(cap != nullptr && cap->size() == 512);
        std::cout << "  [ok] S16 stereo: rms=" << st->last_rms << " (expected "
                  << kSqrtHalf * (32767.0 / 32768.0) << "), dur=" << st->duration_seconds << "s\n";
    }

    // --- float stereo passthrough ----------------------------------------------
    {
        // 750 Hz over 256 frames = exactly 4 cycles, so the measured RMS
        // matches amplitude/sqrt(2) exactly (integer number of periods).
        std::vector<float> pcm(512);
        for (uint32_t i = 0; i < 256; ++i) {
            const float v = static_cast<float>(std::sin(kTwoPi * 750.0 * i / 48000.0));
            pcm[2 * i]     = v;
            pcm[2 * i + 1] = v;
        }
        CHECK(AudioOutOutput(h2, pcm.data()) == 256);

        const PS5::Audio::AudioSinkStats* st = HeadlessAudioOutStats(h2);
        CHECK(st != nullptr);
        CHECK(st->bytes_consumed == 256 * 8); // 2ch * float
        CHECK(Near(st->last_rms, kSqrtHalf, 0.002));
        CHECK(Near(st->peak, 1.0, 0.002));
        std::cout << "  [ok] float stereo: rms=" << st->last_rms << " (expected " << kSqrtHalf
                  << ")\n";
    }

    // --- multi-port AudioOutOutputs + all-or-nothing rejection ------------------
    {
        std::vector<int16_t> pcm1(512, 8000);
        std::vector<float>   pcm2(512, 0.25f);
        TestOutputParam params[2];
        params[0].handle = h1;
        params[0].ptr    = pcm1.data();
        params[1].handle = h2;
        params[1].ptr    = pcm2.data();
        CHECK(AudioOutOutputs(reinterpret_cast<AudioOutOutputParam*>(params), 2) == 2);
        CHECK(HeadlessAudioOutStats(h1)->output_calls == 2);
        CHECK(HeadlessAudioOutStats(h2)->output_calls == 2);

        // A closed/invalid handle in the batch rejects the WHOLE call with no
        // side effects on any port (all-or-nothing).
        TestOutputParam bad[2];
        bad[0].handle = h1;
        bad[0].ptr    = pcm1.data();
        bad[1].handle = 999;
        bad[1].ptr    = pcm1.data();
        CHECK(AudioOutOutputs(reinterpret_cast<AudioOutOutputParam*>(bad), 2) ==
              AUDIO_OUT_ERROR_INVALID_PORT);
        CHECK(HeadlessAudioOutStats(h1)->output_calls == 2); // unchanged
        std::cout << "  [ok] multi-port submit + all-or-nothing rejection\n";
    }

    // --- port state --------------------------------------------------------------
    {
        TestPortState s{};
        CHECK(AudioOutGetPortState(h1, reinterpret_cast<AudioOutPortState*>(&s)) == OK);
        CHECK(s.output == 1);         // MAIN
        CHECK(s.channel == 2);        // stereo clamped to 2
        CHECK(s.volume == 127);
        CHECK(AudioOutGetPortState(h3, reinterpret_cast<AudioOutPortState*>(&s)) == OK);
        CHECK(s.output == 0x40);      // VOICE
        CHECK(s.channel == 1);
        CHECK(AudioOutGetPortState(99, reinterpret_cast<AudioOutPortState*>(&s)) ==
              AUDIO_OUT_ERROR_INVALID_PORT);
        CHECK(AudioOutGetPortState(h1, nullptr) == AUDIO_OUT_ERROR_INVALID_POINTER);
    }

    // --- volume --------------------------------------------------------------------
    {
        int vol[2] = {100, 100};
        CHECK(AudioOutSetVolume(h1, 0, vol) == OK);
        CHECK(AudioOutSetVolume(99, 0, vol) == AUDIO_OUT_ERROR_INVALID_PORT);
    }

    // --- close / reopen semantics ----------------------------------------------------
    {
        CHECK(AudioOutClose(h2) == OK);
        std::vector<int16_t> pcm(512, 0);
        CHECK(AudioOutOutput(h2, pcm.data()) == AUDIO_OUT_ERROR_INVALID_PORT);
        CHECK(AudioOutClose(h2) == AUDIO_OUT_ERROR_INVALID_PORT); // double close
        CHECK(HeadlessAudioOutStats(h2) == nullptr);              // closed port
        const int h2b = AudioOutOpen(1000, 0, 0, 256, 48000, 1);
        CHECK(h2b == 2); // freed slot reused, fresh sink
        CHECK(HeadlessAudioOutStats(h2b)->output_calls == 0);
    }

    // --- port-budget exhaustion -------------------------------------------------------
    {
        int last = 0;
        for (int i = 0; i < 29; ++i) { // slots 4..32 (1,2,3 in use)
            last = AudioOutOpen(1000, 0, 0, 256, 48000, 1);
            CHECK(last == 4 + i);
        }
        CHECK(last == 32);
        CHECK(AudioOutOpen(1000, 0, 0, 256, 48000, 1) == AUDIO_OUT_ERROR_PORT_FULL);
        std::cout << "  [ok] port budget exhausted at 32 (AUDIO_OUT_ERROR_PORT_FULL)\n";
    }

    if (g_failures == 0) {
        std::cout << g_checks << "/" << g_checks << " checks passed\n";
        std::cout << ">> [PASS] Headless AudioOut wiring verified (guest AudioOut* -> real sink).\n";
        return 0;
    }
    std::cout << g_checks - g_failures << "/" << g_checks
              << " checks passed\n>> [FAIL] headless AudioOut wiring.\n";
    return 1;
}
