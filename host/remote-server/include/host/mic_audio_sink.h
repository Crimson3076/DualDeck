#pragma once

// Seam between the network layer and wherever received microphone audio
// actually goes (GitHub issue #2). Mirrors IEmulatorInputSink's shape and
// threading contract exactly: LoggingMicAudioSink is the Phase-1
// implementation used by the standalone synthetic host; once wired into
// melonDS (see host/melonds-patches/README.md), a MelonDSMicAudioSink
// implements this same interface and feeds EmuInstance::remoteMicFeed()
// on the emulation thread, never directly from the network thread.

#include "melonds_remote/protocol.h"

namespace melonds_remote::host {

class IMicAudioSink {
public:
    virtual ~IMicAudioSink() = default;

    // Called whenever the network layer has a new accepted MicAudioFrame.
    // Implementations must not block (no I/O, no locking that could stall
    // the emulation thread) -- store/forward the samples and let whatever
    // consumes them (e.g. melonDS's own Mic::Advance-driven pull) do so on
    // its own schedule, matching how applyControllerState() is handled.
    virtual void applyMicAudio(const MicAudioFramePayload& frame) = 0;

    // Called on client disconnect, capability loss, or mic-input timeout.
    // Unlike releaseAll() for controller state, there is no "audio held
    // down" to force-release -- absence of further applyMicAudio() calls
    // already reads as silence to any consumer pulling from a ring buffer
    // that simply runs dry. This exists for implementations that want an
    // explicit signal anyway (e.g. to clear a level-meter display).
    virtual void releaseAudio() = 0;
};

} // namespace melonds_remote::host
