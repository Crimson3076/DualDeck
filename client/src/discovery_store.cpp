#include "discovery_store.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace melonds_remote::client {

std::string defaultLastHostStorePath() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    return std::string(home) + "/.config/melonds-remote-client/last_host.txt";
}

std::optional<std::string> loadLastHost(const std::string& storePath) {
    if (storePath.empty()) return std::nullopt;
    std::ifstream in(storePath);
    if (!in.is_open()) return std::nullopt;
    std::string line;
    if (!std::getline(in, line) || line.empty()) return std::nullopt;
    return line;
}

void saveLastHost(const std::string& storePath, const std::string& hostAddress) {
    if (storePath.empty()) return;

    std::error_code ec;
    std::filesystem::path path(storePath);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    std::ofstream out(storePath, std::ios::trunc);
    if (out.is_open()) {
        out << hostAddress << '\n';
    }
}

} // namespace melonds_remote::client
