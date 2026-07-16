#pragma once

// Remembers which discovered host the user picked last time, so the
// host-selection screen (always shown on launch, see main.cpp's
// discoverAndSelectHost()) pre-highlights it as the default choice --
// picking the same host again next time is then just one button press,
// without silently skipping the screen entirely (the user asked to
// always be able to pick a different host). Plain text, single line:
// just the last-selected host's address.

#include <optional>
#include <string>

namespace melonds_remote::client {

// $HOME/.config/melonds-remote-client/last_host.txt, or empty if $HOME
// isn't set (persistence then silently unavailable).
std::string defaultLastHostStorePath();

std::optional<std::string> loadLastHost(const std::string& storePath);
void saveLastHost(const std::string& storePath, const std::string& hostAddress);

} // namespace melonds_remote::client
