#include "mic_capture.h"

#include <algorithm>
#include <cmath>

#include "melonds_remote/protocol.h"

namespace melonds_remote::client {

std::vector<MicDeviceInfo> listMicDevices() {
    std::vector<MicDeviceInfo> devices;
    devices.push_back(MicDeviceInfo{SDL_AUDIO_DEVICE_DEFAULT_RECORDING, "SYSTEM DEFAULT"});

    int count = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count);
    if (ids) {
        for (int i = 0; i < count; ++i) {
            const char* name = SDL_GetAudioDeviceName(ids[i]);
            devices.push_back(MicDeviceInfo{ids[i], name ? name : "UNKNOWN DEVICE"});
        }
        SDL_free(ids);
    }
    return devices;
}

MicCapture::MicCapture() = default;

MicCapture::~MicCapture() {
    close();
}

bool MicCapture::open(const std::string& deviceName) {
    close();

    SDL_AudioDeviceID deviceId = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    if (!deviceName.empty()) {
        for (const auto& d : listMicDevices()) {
            if (d.name == deviceName) {
                deviceId = d.id;
                break;
            }
        }
        // else: fall through to the system default, matching a mic that
        // was unplugged/renamed since this name was saved.
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = 1;
    spec.freq = static_cast<int>(melonds_remote::kMicAudioSampleRate);

    stream_ = SDL_OpenAudioDeviceStream(deviceId, &spec, nullptr, nullptr);
    if (!stream_) {
        return false;
    }
    if (!SDL_ResumeAudioStreamDevice(stream_)) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
        return false;
    }
    return true;
}

void MicCapture::close() {
    if (stream_) {
        SDL_DestroyAudioStream(stream_); // also closes the associated device
        stream_ = nullptr;
    }
}

bool MicCapture::isOpen() const {
    return stream_ != nullptr;
}

float MicCapture::pollSamples(std::vector<int16_t>& outSamples) {
    if (!stream_) {
        return 0.0f;
    }

    int availBytes = SDL_GetAudioStreamAvailable(stream_);
    if (availBytes <= 0) {
        return 0.0f;
    }

    size_t startIndex = outSamples.size();
    size_t availSamples = static_cast<size_t>(availBytes) / sizeof(int16_t);
    outSamples.resize(startIndex + availSamples);

    int gotBytes = SDL_GetAudioStreamData(stream_, outSamples.data() + startIndex, availBytes);
    if (gotBytes <= 0) {
        outSamples.resize(startIndex);
        return 0.0f;
    }
    size_t gotSamples = static_cast<size_t>(gotBytes) / sizeof(int16_t);
    outSamples.resize(startIndex + gotSamples);

    if (gotSamples == 0) {
        return 0.0f;
    }
    double sumSquares = 0.0;
    for (size_t i = startIndex; i < outSamples.size(); ++i) {
        double s = static_cast<double>(outSamples[i]) / 32768.0;
        sumSquares += s * s;
    }
    double rms = std::sqrt(sumSquares / static_cast<double>(gotSamples));
    return static_cast<float>(std::min(rms, 1.0));
}

} // namespace melonds_remote::client
