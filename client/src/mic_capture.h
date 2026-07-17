#pragma once

// Microphone capture for the client side of GitHub issue #2 ("Allow use
// for microphone"): enumerates recording devices, opens a chosen one at
// the fixed protocol format (mono 16-bit PCM @ melonds_remote::
// kMicAudioSampleRate -- see protocol.h), and exposes a poll-once-per-
// frame interface so main.cpp's existing frame loop can pull captured
// samples the same way it already pulls gamepad/touch state, without
// needing its own audio thread.

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

namespace melonds_remote::client {

struct MicDeviceInfo {
    SDL_AudioDeviceID id = 0;
    std::string name;
};

// Enumerates available recording devices. Always includes a synthetic
// "SYSTEM DEFAULT" entry first (id == SDL_AUDIO_DEVICE_DEFAULT_RECORDING)
// so the settings picker always has at least one selectable option, even
// on a machine SDL reports zero named capture devices for -- opening the
// default ID still works via the platform's own default-device tracking
// in that case, matching issue #2's "built-in microphone as the default
// when available."
std::vector<MicDeviceInfo> listMicDevices();

// Wraps one open SDL3 audio-capture stream. Not thread-safe; intended to
// be owned and polled from the single render/input loop in main.cpp,
// same as every other piece of per-frame client state.
class MicCapture {
public:
    MicCapture();
    ~MicCapture();
    MicCapture(const MicCapture&) = delete;
    MicCapture& operator=(const MicCapture&) = delete;

    // Opens the device whose SDL_GetAudioDeviceName() matches
    // `deviceName` exactly, or the system default if `deviceName` is
    // empty or no longer matches any currently-enumerated device (e.g. a
    // USB mic that was unplugged since the name was saved -- issue #2:
    // "A client without a microphone can still connect and use all
    // existing features," which this falls back toward rather than
    // failing outright). Closes any previously-open stream first.
    // Returns false (capture left closed) if no device could be opened
    // at all, e.g. no recording hardware present or permission denied.
    bool open(const std::string& deviceName);
    void close();
    bool isOpen() const;

    // Pulls all samples SDL has buffered since the last call, appending
    // them (mono int16_t @ kMicAudioSampleRate) to `outSamples`, and
    // returns the RMS level (0.0-1.0) of just the samples pulled this
    // call for a live level-meter display -- 0.0 if capture is closed or
    // nothing was available. Non-blocking; call once per frame.
    float pollSamples(std::vector<int16_t>& outSamples);

private:
    SDL_AudioStream* stream_ = nullptr;
};

} // namespace melonds_remote::client
