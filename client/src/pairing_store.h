#pragma once

// Persists the pairing token issued by a host (see
// melonds_remote::HelloAckPayload::pairingToken) across client runs, keyed
// by host address, so the user only has to enter the 6-digit pairing code
// once per host rather than on every launch (spec section 13's "six-digit
// pairing code" option). Plain text, one "host=token" line per host --
// this is a bearer-token cache for a purely local, single-user pairing
// scheme, not a secrets vault, so no encryption is attempted; the same
// trust model already applies to melonDS's own on-disk config.

#include <optional>
#include <string>

namespace melonds_remote::client {

// Where the token file lives: $HOME/.config/melonds-remote-client/pairing_tokens.txt,
// or empty if $HOME isn't set (persistence is then silently unavailable --
// pairing still works for the life of the process).
std::string defaultPairingStorePath();

// Returns the stored token for `hostAddress`, if any.
std::optional<std::string> loadPairingToken(const std::string& storePath, const std::string& hostAddress);

// Stores (overwriting any previous entry for the same host) or removes
// (if `token` is empty) the token for `hostAddress`.
void savePairingToken(const std::string& storePath, const std::string& hostAddress, const std::string& token);

} // namespace melonds_remote::client
