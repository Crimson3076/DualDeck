#include "melonds_remote/rate_limiter.h"

#include <algorithm>

namespace melonds_remote {

ConnectionRateLimiter::ConnectionRateLimiter(int maxAttempts, uint64_t windowUs)
    : maxAttempts_(maxAttempts), windowUs_(windowUs) {}

namespace {
void pruneEntry(std::vector<uint64_t>& times, uint64_t nowUs, uint64_t windowUs) {
    times.erase(std::remove_if(times.begin(), times.end(),
                                [&](uint64_t t) { return nowUs - t > windowUs; }),
                times.end());
}
} // namespace

bool ConnectionRateLimiter::allowAttempt(const std::string& key, uint64_t nowUs) {
    Entry& entry = entries_[key];
    pruneEntry(entry.attemptTimesUs, nowUs, windowUs_);

    bool allowed = static_cast<int>(entry.attemptTimesUs.size()) < maxAttempts_;
    entry.attemptTimesUs.push_back(nowUs);
    return allowed;
}

void ConnectionRateLimiter::pruneStaleEntries(uint64_t nowUs) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        pruneEntry(it->second.attemptTimesUs, nowUs, windowUs_);
        if (it->second.attemptTimesUs.empty()) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace melonds_remote
