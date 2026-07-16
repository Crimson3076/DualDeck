#include "device_identity.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

namespace melonds_remote::client {

namespace {

std::string generateIdentity() {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<int> nibble(0, 15);
    static const char kHexDigits[] = "0123456789abcdef";
    std::string id(32, '0');
    for (char& c : id) {
        c = kHexDigits[nibble(rng)];
    }
    return id;
}

} // namespace

std::string defaultDeviceIdentityStorePath() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    return std::string(home) + "/.config/melonds-remote-client/device_id.txt";
}

std::string loadOrCreateDeviceIdentity(const std::string& storePath) {
    if (!storePath.empty()) {
        std::ifstream in(storePath);
        std::string line;
        if (in.is_open() && std::getline(in, line) && !line.empty()) {
            return line;
        }
    }

    std::string identity = generateIdentity();

    if (!storePath.empty()) {
        std::error_code ec;
        std::filesystem::path path(storePath);
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path(), ec);
        }
        std::ofstream out(storePath, std::ios::trunc);
        if (out.is_open()) {
            out << identity << '\n';
        }
    }

    return identity;
}

} // namespace melonds_remote::client
