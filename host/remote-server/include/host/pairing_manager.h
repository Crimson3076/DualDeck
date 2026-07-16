#pragma once

// Implements the "pairing code" flow from SPEC.md section 13's "later
// pairing options" (six-digit pairing code, pre-shared token -- both now
// supported side by side, see NetServerConfig::authToken vs this class):
//
//   1. An unrecognized client attempt causes a fresh, short-lived,
//      single-use 6-digit code to be generated and surfaced to whoever is
//      at the host (console log, and optionally a GUI hook).
//   2. The user reads the code and enters it on the client, which retries
//      the handshake with that code as the Hello payload's authToken.
//   3. On a correct code, the host issues a long-lived opaque pairing
//      token in the HelloAck, which the client persists and sends on all
//      future connections instead of prompting for a code again.
//
// Independent of NetServerConfig::authToken, which remains a static
// pre-shared secret for scripting/CI use (checked first by NetServer;
// this class is only consulted when no static token is configured).

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <unordered_set>

namespace melonds_remote::host {

class PairingManager {
public:
    // `stateFilePath` is where previously-issued pairing tokens are
    // persisted (one per line) so a paired client survives a host
    // restart. An empty path disables persistence -- pairing still works
    // within the current process lifetime, just isn't remembered across
    // restarts (used by tests that shouldn't touch the filesystem).
    explicit PairingManager(std::string stateFilePath,
                            std::chrono::seconds codeTtl = std::chrono::seconds(300));

    PairingManager(const PairingManager&) = delete;
    PairingManager& operator=(const PairingManager&) = delete;

    enum class CheckResult {
        ValidExistingToken, // matched a previously-issued token: accept, nothing new to hand back
        ValidCode,          // matched the active code: accept, a new token was just issued
        NeedsCode,          // no match and no code was active; one was just generated as a side effect
        WrongCode,          // no match, but a code IS currently active (mistyped or stale)
    };

    // Checks `presented` (the Hello payload's authToken field) against
    // known pairing tokens and the currently-active pairing code. As a
    // side effect, generates a fresh code if nothing matches and no code
    // is currently active (see the onCodeChanged callback).
    CheckResult check(const std::string& presented);

    // Only meaningful immediately after check() returns ValidCode.
    std::string lastIssuedToken() const;

    // The currently-active pairing code, if any.
    std::optional<std::string> currentCode() const;

    // Fired synchronously (on whatever thread calls check()) whenever the
    // active code changes: a fresh code when one is generated, or
    // std::nullopt when it's consumed or expires. Callers needing
    // UI-thread delivery (e.g. a Qt window) must marshal it themselves.
    void setOnCodeChanged(std::function<void(std::optional<std::string>)> callback);

private:
    std::string generateCode();
    std::string generateToken();
    void loadTokensLocked();
    void persistTokenLocked(const std::string& token);
    void setCodeLocked(std::optional<std::string> code);
    void expireIfNeededLocked();

    mutable std::mutex mutex_;
    std::string stateFilePath_;
    std::chrono::seconds codeTtl_;

    std::optional<std::string> currentCode_;
    std::chrono::steady_clock::time_point codeExpiry_;

    std::string lastIssuedToken_;
    std::unordered_set<std::string> knownTokens_;

    std::function<void(std::optional<std::string>)> onCodeChanged_;

    std::mt19937 rng_;
};

} // namespace melonds_remote::host
