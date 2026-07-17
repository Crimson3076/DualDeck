#pragma once

// Where the Host-Service<->adapter Unix domain socket lives (GitHub
// issue #28 Phase 2 / the ADR's IPC decision). Ownership: created and
// removed by the Host Service (the listening side, see
// adapter_ipc_server.h); an adapter process only ever connects to an
// existing path, never creates one.

#include <string>

namespace melonds_remote::adapter::ipc {

// Returns "$XDG_RUNTIME_DIR/dualdeck/adapter.sock", or
// "$HOME/.cache/dualdeck/adapter.sock" if XDG_RUNTIME_DIR is unset --
// matching this codebase's existing fallback convention for
// "$HOME/.config/melonds-remote*" paths elsewhere (plain env-var
// lookups, no XDG library dependency). Returns an empty string if
// neither XDG_RUNTIME_DIR nor HOME is set (nothing meaningful to return
// -- callers should treat that as "IPC unavailable," the same way
// device_identity.h's defaultDeviceIdentityStorePath()-style helpers
// already treat a missing HOME).
std::string defaultAdapterSocketPath();

// Ensures the socket's parent directory exists with mode 0700 (only
// the current user may read/traverse it -- GitHub issue #28: "How only
// the current user may register an adapter"). Returns false on failure
// (caller should not attempt to bind the socket in that case).
bool ensureSocketDirectory(const std::string& socketPath);

} // namespace melonds_remote::adapter::ipc
