#include "audio/headless_audio_sink.hpp"

#include <cmath>
#include <cstring>

namespace PS5::Audio {

uint32_t HeadlessAudioSink::ChannelsForFormat(SampleFormat f) {
    switch (f) {
        case SampleFormat::S16Mono:
        case SampleFormat::FloatMono:    return 1;
        case SampleFormat::S16Stereo:
        case SampleFormat::FloatStereo:  return 2;
        case SampleFormat::S16_8Ch:
        case SampleFormat::Float8Ch:
        case SampleFormat::S16_8ChStd:
        case SampleFormat::Float8ChStd:  return 8;
        default:                         return 0;
    }
}

bool HeadlessAudioSink::FormatIsFloat(SampleFormat f) {
    return f == SampleFormat::FloatMono || f == SampleFormat::FloatStereo ||
           f == SampleFormat::Float8Ch  || f == SampleFormat::Float8ChStd;
}

uint32_t HeadlessAudioSink::BytesPerSample(SampleFormat f) {
    return FormatIsFloat(f) ? sizeof(float) : sizeof(int16_t);
}

uint32_t HeadlessAudioSink::BytesPerFrame(SampleFormat f) {
    return BytesPerSample(f) * ChannelsForFormat(f);
}

bool HeadlessAudioSink::Open(const AudioPortConfig& config) {
    const uint32_t channels = ChannelsForFormat(config.format);
    if (channels == 0) return false;                 // unknown format
    if (config.sample_rate == 0) return false;
    if (config.grain == 0) return false;

    m_config = config;
    m_channels = channels;
    m_open = true;
    m_stats = AudioSinkStats{};
    m_captured.clear();
    return true;
}

int HeadlessAudioSink::Output(const void* pcm, uint32_t frames) {
    if (!m_open || pcm == nullptr || frames == 0) return -1;

    const uint32_t channels = m_channels;
    const size_t samples = static_cast<size_t>(frames) * channels;
    const bool is_float = FormatIsFloat(m_config.format);
    const size_t bytes = static_cast<size_t>(frames) * BytesPerFrame(m_config.format);

    const size_t base = m_captured.size();
    m_captured.resize(base + samples);

    double sum_sq = 0.0;
    if (is_float) {
        const float* src = static_cast<const float*>(pcm);
        for (size_t i = 0; i < samples; ++i) {
            const float v = src[i];
            m_captured[base + i] = v;
            const double a = std::fabs(static_cast<double>(v));
            if (a > m_stats.peak) m_stats.peak = a;
            sum_sq += static_cast<double>(v) * v;
        }
    } else {
        const int16_t* src = static_cast<const int16_t*>(pcm);
        for (size_t i = 0; i < samples; ++i) {
            const float v = static_cast<float>(src[i]) / 32768.0f;
            m_captured[base + i] = v;
            const double a = std::fabs(static_cast<double>(v));
            if (a > m_stats.peak) m_stats.peak = a;
            sum_sq += static_cast<double>(v) * v;
        }
    }

    m_stats.output_calls  += 1;
    m_stats.frames_written  += frames;
    m_stats.samples_written += samples;
    m_stats.bytes_consumed  += bytes;
    m_stats.last_rms = samples ? std::sqrt(sum_sq / static_cast<double>(samples)) : 0.0;
    m_stats.duration_seconds =
        static_cast<double>(m_stats.frames_written) / static_cast<double>(m_config.sample_rate);

    return static_cast<int>(frames);
}

void HeadlessAudioSink::Close() {
    m_open = false;
}

} // namespace PS5::Audio
