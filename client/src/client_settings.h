#pragma once

#include <string>

namespace melonds_remote::client {

struct ClientSettings {
    // Preserve the release launcher's existing behavior for users who do
    // not have a settings file yet.
    bool autoUpdateOnLaunch = true;

    // Empty means "use the system default recording device" (SDL3's
    // SDL_AUDIO_DEVICE_DEFAULT_RECORDING) -- the common case, and the
    // safe fallback if a previously-selected named device is
    // unplugged/renamed at next launch (GitHub issue #2: "the client
    // device's built-in microphone as the default when available").
    // Otherwise holds the exact string SDL_GetAudioDeviceName() returned
    // for the device the user picked in Settings.
    std::string micDeviceName;

    // Persisted mute state (issue #2's "mute ... controls"), separate
    // from whether a capture device is even open -- muting doesn't close
    // the device, it just stops sending captured audio to the host, so
    // the level meter still works while muted.
    bool micMuted = false;
};

// $HOME/.config/melonds-remote-client/settings.conf, or empty if HOME is
// unavailable. An empty path makes loading return defaults and saving fail
// without touching the filesystem.
std::string defaultClientSettingsPath();

ClientSettings loadClientSettings(const std::string& settingsPath);

// Replaces the settings file atomically where the platform permits it.
// Returns false when the directory or file could not be written.
bool saveClientSettings(const std::string& settingsPath, const ClientSettings& settings);

} // namespace melonds_remote::client
