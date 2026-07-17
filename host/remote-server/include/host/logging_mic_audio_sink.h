#pragma once

#include <cstdint>
#include <mutex>

#include "host/mic_audio_sink.h"

namespace melonds_remote::host {

// Default IMicAudioSink used until a real melonDS integration exists.
// Tracks the latest frame and periodically logs an RMS level so audio
// arrival and rough loudness can be observed end-to-end without melonDS
// built, mirroring LoggingInputSink's role for controller state.
class LoggingMicAudioSink : public IMicAudioSink {
public:
    void applyMicAudio(const MicAudioFramePayload& frame) override;
    void releaseAudio() override;

    MicAudioFramePayload lastFrame() const;

    // 0.0-1.0 RMS level of the most recently received frame, 0 if none
    // has arrived yet or audio was released. Exposed separately from
    // lastFrame() so a caller doesn't need to recompute it.
    float lastLevel() const;

private:
    mutable std::mutex mutex_;
    MicAudioFramePayload lastFrame_;
    float lastLevel_ = 0.0f;
    uint64_t framesReceived_ = 0;
};

} // namespace melonds_remote::host
