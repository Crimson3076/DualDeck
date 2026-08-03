#include "client_settings.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace melonds_remote::client {

namespace {

// See device_identity.cpp's migrateFromLegacyPathIfNeeded() for the
// full rationale -- same one-time melonDS-Remote -> DualDeck config-dir
// migration, duplicated per file rather than shared.
void migrateFromLegacyPathIfNeeded(const std::string& newPath, const std::string& legacyPath) {
    std::error_code ec;
    if (std::filesystem::exists(newPath, ec)) return;
    if (!std::filesystem::exists(legacyPath, ec)) return;
    std::filesystem::path path(newPath);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::filesystem::copy_file(legacyPath, newPath, std::filesystem::copy_options::none, ec);
}

} // namespace

std::string defaultClientSettingsPath() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    std::string newPath = std::string(home) + "/.config/dualdeck-client/settings.conf";
    migrateFromLegacyPathIfNeeded(newPath, std::string(home) + "/.config/melonds-remote-client/settings.conf");
    return newPath;
}

ClientSettings loadClientSettings(const std::string& settingsPath) {
    ClientSettings settings;
    if (settingsPath.empty()) return settings;

    std::ifstream in(settingsPath);
    std::string line;
    while (std::getline(in, line)) {
        constexpr const char* kAutoUpdatePrefix = "auto_update_on_launch=";
        constexpr const char* kMicDevicePrefix = "mic_device_name=";
        constexpr const char* kMicMutedPrefix = "mic_muted=";
        constexpr const char* kVideoQualityPrefix = "video_quality=";
        constexpr const char* kMirrorHostScreenPrefix = "mirror_host_screen=";

        if (line.rfind(kAutoUpdatePrefix, 0) == 0) {
            std::string value = line.substr(std::char_traits<char>::length(kAutoUpdatePrefix));
            if (value == "1" || value == "true" || value == "on") {
                settings.autoUpdateOnLaunch = true;
            } else if (value == "0" || value == "false" || value == "off") {
                settings.autoUpdateOnLaunch = false;
            }
        } else if (line.rfind(kMicDevicePrefix, 0) == 0) {
            settings.micDeviceName = line.substr(std::char_traits<char>::length(kMicDevicePrefix));
        } else if (line.rfind(kMicMutedPrefix, 0) == 0) {
            std::string value = line.substr(std::char_traits<char>::length(kMicMutedPrefix));
            if (value == "1" || value == "true" || value == "on") {
                settings.micMuted = true;
            } else if (value == "0" || value == "false" || value == "off") {
                settings.micMuted = false;
            }
        } else if (line.rfind(kVideoQualityPrefix, 0) == 0) {
            std::string value = line.substr(std::char_traits<char>::length(kVideoQualityPrefix));
            try {
                int parsed = std::stoi(value);
                // 0 (auto) or a valid quality; anything else (corrupt
                // file, hand-edited garbage) falls back to the default
                // rather than sending an out-of-range byte to the host.
                if (parsed == 0 || (parsed >= 1 && parsed <= 100)) {
                    settings.videoQuality = parsed;
                }
            } catch (const std::exception&) {
                // Leave the default in place.
            }
        } else if (line.rfind(kMirrorHostScreenPrefix, 0) == 0) {
            std::string value = line.substr(std::char_traits<char>::length(kMirrorHostScreenPrefix));
            if (value == "1" || value == "true" || value == "on") {
                settings.mirrorHostScreen = true;
            } else if (value == "0" || value == "false" || value == "off") {
                settings.mirrorHostScreen = false;
            }
        }
    }
    return settings;
}

bool saveClientSettings(const std::string& settingsPath, const ClientSettings& settings) {
    if (settingsPath.empty()) return false;

    std::error_code ec;
    std::filesystem::path path(settingsPath);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) return false;
    }

    std::filesystem::path temporaryPath = path;
    temporaryPath += ".tmp";
    {
        std::ofstream out(temporaryPath, std::ios::trunc);
        if (!out.is_open()) return false;
        out << "auto_update_on_launch=" << (settings.autoUpdateOnLaunch ? "1" : "0") << '\n';
        out << "mic_device_name=" << settings.micDeviceName << '\n';
        out << "mic_muted=" << (settings.micMuted ? "1" : "0") << '\n';
        out << "video_quality=" << settings.videoQuality << '\n';
        out << "mirror_host_screen=" << (settings.mirrorHostScreen ? "1" : "0") << '\n';
        if (!out.good()) {
            out.close();
            std::error_code cleanupError;
            std::filesystem::remove(temporaryPath, cleanupError);
            return false;
        }
    }

    std::filesystem::rename(temporaryPath, path, ec);
    if (ec) {
        std::error_code cleanupError;
        std::filesystem::remove(temporaryPath, cleanupError);
        return false;
    }
    return true;
}

} // namespace melonds_remote::client
