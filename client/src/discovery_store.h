#pragma once

// Remembers which discovered host the user picked last time, so a
// second launch on the same LAN with the same single host (or the same
// previously-chosen one among several) reconnects without asking again --
// matches the "ask once, remember after" pattern already used for
// pairing tokens (see pairing_store.h). Plain text, single line: just
// the last-selected host's address.

#include <optional>
#include <string>

namespace melonds_remote::client {

// $HOME/.config/melonds-remote-client/last_host.txt, or empty if $HOME
// isn't set (persistence then silently unavailable).
std::string defaultLastHostStorePath();

std::optional<std::string> loadLastHost(const std::string& storePath);
void saveLastHost(const std::string& storePath, const std::string& hostAddress);

} // namespace melonds_remote::client
