#include "wizard_state.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

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

std::string defaultWizardStatePath() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    std::string newPath = std::string(home) + "/.config/dualdeck-client/setup_complete.txt";
    migrateFromLegacyPathIfNeeded(newPath, std::string(home) + "/.config/melonds-remote-client/setup_complete.txt");
    return newPath;
}

bool isSetupComplete(const std::string& statePath) {
    if (statePath.empty()) return false;
    return std::filesystem::exists(statePath);
}

void markSetupComplete(const std::string& statePath) {
    if (statePath.empty()) return;

    std::error_code ec;
    std::filesystem::path path(statePath);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    std::ofstream out(statePath, std::ios::trunc);
    if (out.is_open()) {
        out << "done\n";
    }
}

} // namespace melonds_remote::client
