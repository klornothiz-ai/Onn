// ============================================================================
// ProsperoLayer RDNA2 Core - headless audio sink test (item #2, audio half)
// ----------------------------------------------------------------------------
// Verifies the SDL-free audio-out core: format/channel/byte math, PCM decode
// for both int16 and float formats, frame/byte accounting, per-call RMS and
// stream peak, duration, and an exact known-signal round-trip. This is the
// audio equivalent of the headless GPU path -- guest audio output becomes
// observable and correct with no host audio device present.
// ============================================================================
#include "audio/headless_audio_sink.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {
using namespace PS5::Audio;

int g_failures = 0, g_checks = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) { ++g_failures; std::cerr << "  [FAIL] line " << line << ": " << e << '\n'; }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

constexpr double kPi = 3.14159265358979323846;
} // namespace

int main() {
    std::cout << "=== Headless Audio Sink Test (item #2, audio) ===\n";

    // --- format math --------------------------------------------------------
    CHECK(HeadlessAudioSink::ChannelsForFormat(SampleFormat::S16Mono) == 1);
    CHECK(HeadlessAudioSink::ChannelsForFormat(SampleFormat::S16Stereo) == 2);
    CHECK(HeadlessAudioSink::ChannelsForFormat(SampleFormat::Float8Ch) == 8);
    CHECK(HeadlessAudioSink::BytesPerSample(SampleFormat::S16Stereo) == 2);
    CHECK(HeadlessAudioSink::BytesPerSample(SampleFormat::FloatStereo) == 4);
    CHECK(HeadlessAudioSink::BytesPerFrame(SampleFormat::S16Stereo) == 4);
    CHECK(HeadlessAudioSink::BytesPerFrame(SampleFormat::Float8Ch) == 32);
    CHECK(!HeadlessAudioSink::FormatIsFloat(SampleFormat::S16Stereo));
    CHECK(HeadlessAudioSink::FormatIsFloat(SampleFormat::FloatStereo));

    // --- open validation ----------------------------------------------------
    {
        HeadlessAudioSink sink;
        CHECK(!sink.Open({SampleFormat::Unknown, 48000, 256})); // bad format
        CHECK(!sink.Open({SampleFormat::S16Stereo, 0, 256}));   // bad rate
        CHECK(!sink.Open({SampleFormat::S16Stereo, 48000, 0})); // bad grain
        CHECK(sink.Open({SampleFormat::S16Stereo, 48000, 256}));
        CHECK(sink.IsOpen());
        // output before configured size still works with explicit frame count
        int16_t buf[2 * 256] = {};
        CHECK(sink.OutputGrain(buf) == 256);
        CHECK(sink.Stats().frames_written == 256);
        CHECK(sink.Stats().samples_written == 512);          // 256 * 2ch
        CHECK(sink.Stats().bytes_consumed == 256 * 4);       // 4 bytes/frame
        CHECK(sink.Stats().peak == 0.0);                     // silence
        CHECK(sink.Output(nullptr, 256) == -1);
    }

    // --- int16 decode: full-scale -> ~1.0 peak ------------------------------
    {
        HeadlessAudioSink sink;
        sink.Open({SampleFormat::S16Mono, 48000, 4});
        int16_t buf[4] = {32767, -32768, 16384, 0};
        CHECK(sink.Output(buf, 4) == 4);
        const auto& s = sink.CapturedFloat();
        CHECK(s.size() == 4);
        CHECK(std::fabs(s[0] - (32767.0f / 32768.0f)) < 1e-4f);
        CHECK(std::fabs(s[1] - (-1.0f)) < 1e-4f);
        CHECK(std::fabs(s[2] - 0.5f) < 1e-4f);
        CHECK(std::fabs(sink.Stats().peak - 1.0) < 1e-3);
        std::cout << "  [ok] int16 decode: peak=" << sink.Stats().peak << "\n";
    }

    // --- float decode passes through unchanged ------------------------------
    {
        HeadlessAudioSink sink;
        sink.Open({SampleFormat::FloatStereo, 48000, 3});
        float buf[6] = {0.25f, -0.5f, 1.0f, -1.0f, 0.0f, 0.75f};
        CHECK(sink.Output(buf, 3) == 3);
        const auto& s = sink.CapturedFloat();
        CHECK(s.size() == 6);
        for (int i = 0; i < 6; ++i) CHECK(std::fabs(s[i] - buf[i]) < 1e-6f);
        CHECK(std::fabs(sink.Stats().peak - 1.0) < 1e-6);
        std::cout << "  [ok] float passthrough exact\n";
    }

    // --- known sine wave: RMS ~= amplitude/sqrt(2), duration accounting -----
    {
        const uint32_t rate = 48000, freq = 480, frames = 4800; // 0.1 s, 10 cycles
        HeadlessAudioSink sink;
        sink.Open({SampleFormat::FloatMono, rate, frames});
        std::vector<float> wave(frames);
        const float amp = 0.8f;
        for (uint32_t i = 0; i < frames; ++i)
            wave[i] = amp * static_cast<float>(std::sin(2.0 * kPi * freq * i / rate));
        CHECK(sink.Output(wave.data(), frames) == static_cast<int>(frames));
        const double expected_rms = amp / std::sqrt(2.0);
        CHECK(std::fabs(sink.Stats().last_rms - expected_rms) < 5e-3);
        CHECK(std::fabs(sink.Stats().peak - amp) < 1e-2);
        CHECK(std::fabs(sink.Stats().duration_seconds - 0.1) < 1e-6);
        std::cout << "  [ok] sine: rms=" << sink.Stats().last_rms
                  << " (expected " << expected_rms << "), dur="
                  << sink.Stats().duration_seconds << "s\n";
    }

    // --- multi-grain streaming accumulates frames + duration ----------------
    {
        const uint32_t rate = 48000, grain = 256;
        HeadlessAudioSink sink;
        sink.Open({SampleFormat::S16Stereo, rate, grain});
        std::vector<int16_t> g(grain * 2, 1000);
        for (int k = 0; k < 10; ++k) CHECK(sink.OutputGrain(g.data()) == static_cast<int>(grain));
        CHECK(sink.Stats().output_calls == 10);
        CHECK(sink.Stats().frames_written == grain * 10);
        CHECK(std::fabs(sink.Stats().duration_seconds -
                        (static_cast<double>(grain * 10) / rate)) < 1e-9);
        CHECK(sink.CapturedFloat().size() == grain * 10 * 2);
        std::cout << "  [ok] streamed " << sink.Stats().output_calls
                  << " grains -> " << sink.Stats().frames_written << " frames ("
                  << sink.Stats().duration_seconds << "s)\n";

        sink.Close();
        CHECK(!sink.IsOpen());
        CHECK(sink.Output(g.data(), grain) == -1); // closed
    }

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures == 0) {
        std::cout << ">> [PASS] Headless audio sink verified "
                     "(SDL-free PCM decode + accounting + signal round-trip).\n";
    }
    return g_failures == 0 ? 0 : 1;
}
