#include "device_identity.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

namespace melonds_remote::client {

namespace {

// One-time melonDS-Remote -> DualDeck rebrand migration: if this file
// doesn't exist yet under the new dualdeck-client config dir but does
// under the old melonds-remote-client one, copy it forward before
// anything else touches the new path. Device identity in particular
// must survive byte-for-byte -- losing it would make the host treat an
// already-approved device as brand new. Duplicated per config-path
// function (device identity, last host, settings, wizard state) rather
// than factored into a shared helper, matching this file's existing
// convention of a small duplicated literal per call site.
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
    std::string newPath = std::string(home) + "/.config/dualdeck-client/device_id.txt";
    migrateFromLegacyPathIfNeeded(newPath, std::string(home) + "/.config/melonds-remote-client/device_id.txt");
    return newPath;
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
