#pragma once

#include <string>

namespace melonds_remote::client {

struct ClientSettings {
    // Preserve the release launcher's existing behavior for users who do
    // not have a settings file yet.
    bool autoUpdateOnLaunch = true;
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
