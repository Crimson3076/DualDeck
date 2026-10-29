#include "device_identity.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdint>

namespace dualdeck::client {

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

// The device ID is a bearer credential: 16 bytes straight from the kernel
// CSPRNG, never a seeded userspace PRNG.
std::string generateIdentity() {
    uint8_t bytes[16];
    size_t got = 0;
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        while (got < sizeof(bytes)) {
            ssize_t n = ::read(fd, bytes + got, sizeof(bytes) - got);
            if (n <= 0) break;
            got += static_cast<size_t>(n);
        }
        ::close(fd);
    }
    if (got != sizeof(bytes)) {
        // No usable entropy source (practically impossible on Linux): an
        // unguessable ID can't be made and a weak one must not be, so stop.
        std::fprintf(stderr, "device identity: cannot read /dev/urandom\n");
        std::abort();
    }
    static const char kHexDigits[] = "0123456789abcdef";
    std::string id;
    id.reserve(sizeof(bytes) * 2);
    for (uint8_t b : bytes) {
        id.push_back(kHexDigits[b >> 4]);
        id.push_back(kHexDigits[b & 0x0f]);
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
            // Tighten files written by older versions (default umask).
            ::chmod(storePath.c_str(), 0600);
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
        // 0600 from creation (no window where it's world-readable).
        int fd = ::open(storePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd >= 0) {
            ::fchmod(fd, 0600);
            std::string line = identity + '\n';
            ssize_t ignored = ::write(fd, line.data(), line.size());
            (void)ignored;
            ::close(fd);
        }
    }

    return identity;
}

} // namespace dualdeck::client
