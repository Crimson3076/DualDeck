#include "melonds_remote/adapter/ipc/socket_path.h"

#include <sys/stat.h>

#include <cerrno>
#include <cstdlib>

namespace melonds_remote::adapter::ipc {

std::string defaultAdapterSocketPath() {
    if (const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR"); runtimeDir && *runtimeDir) {
        return std::string(runtimeDir) + "/dualdeck/adapter.sock";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::string(home) + "/.cache/dualdeck/adapter.sock";
    }
    return {};
}

bool ensureSocketDirectory(const std::string& socketPath) {
    if (socketPath.empty()) return false;

    auto lastSlash = socketPath.find_last_of('/');
    if (lastSlash == std::string::npos) return false;
    std::string dir = socketPath.substr(0, lastSlash);
    if (dir.empty()) return false;

    // mkdir 0700: only the current user may read/traverse this
    // directory (GitHub issue #28's "only the current user may
    // register an adapter" requirement) -- the umask can only narrow
    // this further, never widen it, since 0700 has no group/other bits
    // to begin with.
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        return false;
    }

    // If the directory already existed (errno == EEXIST above, or it
    // simply always did), re-assert the permission bits rather than
    // trusting whatever created it -- a stale directory from a previous
    // run, or one this process didn't create, should not silently be
    // trusted at a wider mode.
    struct stat st{};
    if (::stat(dir.c_str(), &st) != 0) {
        return false;
    }
    if ((st.st_mode & 0777) != 0700) {
        if (::chmod(dir.c_str(), 0700) != 0) {
            return false;
        }
    }

    return true;
}

} // namespace melonds_remote::adapter::ipc
