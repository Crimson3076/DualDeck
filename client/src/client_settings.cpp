#include "client_settings.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace melonds_remote::client {

std::string defaultClientSettingsPath() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    return std::string(home) + "/.config/melonds-remote-client/settings.conf";
}

ClientSettings loadClientSettings(const std::string& settingsPath) {
    ClientSettings settings;
    if (settingsPath.empty()) return settings;

    std::ifstream in(settingsPath);
    std::string line;
    while (std::getline(in, line)) {
        constexpr const char* kPrefix = "auto_update_on_launch=";
        if (line.rfind(kPrefix, 0) != 0) continue;

        std::string value = line.substr(std::char_traits<char>::length(kPrefix));
        if (value == "1" || value == "true" || value == "on") {
            settings.autoUpdateOnLaunch = true;
        } else if (value == "0" || value == "false" || value == "off") {
            settings.autoUpdateOnLaunch = false;
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
