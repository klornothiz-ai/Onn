#pragma once
// ============================================================================
// ProsperoLayer RDNA2 Core - headless audio sink (item #2, audio half)
// ----------------------------------------------------------------------------
// The shipped AudioOut backend (libs/audio.cpp) is entirely SDL-gated: with no
// SDL present the whole library is replaced by no-op stubs (optional_stubs_sdl),
// so guest audio output silently disappears and nothing about it is testable.
//
// This sink provides the real, SDL-free core an audio backend needs: it models
// a PS5 audio-out port (format / channels / sample rate / grain size), accepts
// guest PCM output buffers, decodes every supported sample format to normalized
// float, and accounts for frames/bytes plus per-channel peak & RMS so the
// output is observable and correct without any host audio device. The real
// emulator can feed an SDL device from the same decoded stream on a machine
// that has one; on a headless host the sink still runs and verifies.
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace PS5::Audio {

// PS5 sceAudioOut sample formats (value = AUDIO_OUT_PARAM_FORMAT_* low byte).
enum class SampleFormat : uint32_t {
    S16Mono      = 0,
    S16Stereo    = 1,
    S16_8Ch      = 2,
    FloatMono    = 3,
    FloatStereo  = 4,
    Float8Ch     = 5,
    S16_8ChStd   = 6,
    Float8ChStd  = 7,
    Unknown      = 0xffffffffu,
};

struct AudioPortConfig {
    SampleFormat format{SampleFormat::S16Stereo};
    uint32_t sample_rate{48000};
    uint32_t grain{256};      // frames per AudioOutOutput call (samples_num)
};

struct AudioSinkStats {
    uint64_t output_calls{0};
    uint64_t frames_written{0};    // frames (all channels together = 1 frame)
    uint64_t samples_written{0};   // individual per-channel samples
    uint64_t bytes_consumed{0};
    double   peak{0.0};            // max |amplitude| across the whole stream
    double   last_rms{0.0};        // RMS of the most recent Output() call
    double   duration_seconds{0.0};
};

class HeadlessAudioSink {
public:
    static uint32_t ChannelsForFormat(SampleFormat f);
    static bool     FormatIsFloat(SampleFormat f);
    static uint32_t BytesPerSample(SampleFormat f); // per single channel sample
    static uint32_t BytesPerFrame(SampleFormat f);  // per frame (all channels)

    // Open the port. Returns false on an invalid configuration.
    bool Open(const AudioPortConfig& config);
    bool IsOpen() const { return m_open; }
    const AudioPortConfig& Config() const { return m_config; }

    // Submit interleaved guest PCM (frames*channels samples). The pointer must
    // hold BytesPerFrame(format)*frames bytes. Decoded float frames are appended
    // to the captured stream and stats updated. Returns frames consumed, or -1
    // on error (closed / null / bad size).
    int Output(const void* pcm, uint32_t frames);

    // Convenience: submit exactly one configured grain.
    int OutputGrain(const void* pcm) { return Output(pcm, m_config.grain); }

    void Close();

    const AudioSinkStats& Stats() const { return m_stats; }

    // The captured, normalized (-1..1) interleaved float stream. Lets tests /
    // tools verify the exact decoded signal a host device would have played.
    const std::vector<float>& CapturedFloat() const { return m_captured; }

private:
    bool m_open{false};
    AudioPortConfig m_config{};
    uint32_t m_channels{2};
    AudioSinkStats m_stats{};
    std::vector<float> m_captured;
};

} // namespace PS5::Audio
