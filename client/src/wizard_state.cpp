#include "wizard_state.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace melonds_remote::client {

std::string defaultWizardStatePath() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    return std::string(home) + "/.config/melonds-remote-client/setup_complete.txt";
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
