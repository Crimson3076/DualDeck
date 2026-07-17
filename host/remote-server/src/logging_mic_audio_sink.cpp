#include "host/logging_mic_audio_sink.h"

#include <cmath>
#include <cstdio>

namespace melonds_remote::host {

namespace {
// Log roughly once a second at the client's ~100 packets/sec (10ms
// chunks, see kMicAudioSamplesPerPacket) send rate, rather than every
// single packet.
constexpr uint64_t kLogEveryNFrames = 100;

float computeRmsLevel(const MicAudioFramePayload& frame) {
    if (frame.numSamples == 0) {
        return 0.0f;
    }
    double sumSquares = 0.0;
    for (uint16_t i = 0; i < frame.numSamples && i < frame.samples.size(); ++i) {
        double s = static_cast<double>(frame.samples[i]) / 32768.0;
        sumSquares += s * s;
    }
    double rms = std::sqrt(sumSquares / static_cast<double>(frame.numSamples));
    if (rms > 1.0) rms = 1.0;
    return static_cast<float>(rms);
}
} // namespace

void LoggingMicAudioSink::applyMicAudio(const MicAudioFramePayload& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    lastFrame_ = frame;
    lastLevel_ = computeRmsLevel(frame);
    ++framesReceived_;
    if (framesReceived_ % kLogEveryNFrames == 1) {
        std::fprintf(stderr, "[mic] level=%.2f (frame #%llu, %u samples)\n", static_cast<double>(lastLevel_),
                      static_cast<unsigned long long>(framesReceived_), frame.numSamples);
    }
}

void LoggingMicAudioSink::releaseAudio() {
    std::lock_guard<std::mutex> lock(mutex_);
    lastFrame_ = MicAudioFramePayload{};
    lastLevel_ = 0.0f;
    std::fprintf(stderr, "[mic] released (disconnect/mute/timeout)\n");
}

MicAudioFramePayload LoggingMicAudioSink::lastFrame() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastFrame_;
}

float LoggingMicAudioSink::lastLevel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastLevel_;
}

} // namespace melonds_remote::host
