#pragma once

// Sliding-window connection-attempt rate limiter (spec section 13:
// "Rate-limit connection attempts"). Transport-independent -- keyed by
// whatever string identifies a client (typically its source IP), so it is
// unit-testable without any socket code.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace melonds_remote {

class ConnectionRateLimiter {
public:
    // Allows at most `maxAttempts` per `windowUs` microseconds, per key.
    ConnectionRateLimiter(int maxAttempts, uint64_t windowUs);

    // Records an attempt from `key` at time `nowUs` and returns true if it
    // is allowed (i.e. fewer than maxAttempts attempts from this key fall
    // within the trailing window), or false if it should be rejected.
    // A rejected attempt is still recorded, so a client hammering the
    // listener does not get to "reset" its own window by retrying faster.
    bool allowAttempt(const std::string& key, uint64_t nowUs);

    // Removes bookkeeping for keys with no attempts in the trailing
    // window, so long-running processes don't accumulate unbounded state
    // from many distinct source addresses (spec section 15: bounded
    // state).
    void pruneStaleEntries(uint64_t nowUs);

private:
    struct Entry {
        std::vector<uint64_t> attemptTimesUs;
    };

    int maxAttempts_;
    uint64_t windowUs_;
    std::unordered_map<std::string, Entry> entries_;
};

} // namespace melonds_remote
