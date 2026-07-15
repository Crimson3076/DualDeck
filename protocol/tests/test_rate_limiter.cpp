#include "melonds_remote/rate_limiter.h"
#include "test_framework.h"

using namespace melonds_remote;

MDR_TEST(rate_limiter_allows_up_to_max_within_window) {
    ConnectionRateLimiter limiter(3, 1'000'000); // 3 per second

    MDR_CHECK(limiter.allowAttempt("1.2.3.4", 0));
    MDR_CHECK(limiter.allowAttempt("1.2.3.4", 100));
    MDR_CHECK(limiter.allowAttempt("1.2.3.4", 200));
    MDR_CHECK(!limiter.allowAttempt("1.2.3.4", 300)); // 4th within the window
}

MDR_TEST(rate_limiter_tracks_keys_independently) {
    ConnectionRateLimiter limiter(1, 1'000'000);

    MDR_CHECK(limiter.allowAttempt("client-a", 0));
    MDR_CHECK(!limiter.allowAttempt("client-a", 100));
    // a different key has its own independent budget
    MDR_CHECK(limiter.allowAttempt("client-b", 100));
}

MDR_TEST(rate_limiter_recovers_after_window_elapses) {
    ConnectionRateLimiter limiter(2, 1'000'000);

    MDR_CHECK(limiter.allowAttempt("1.2.3.4", 0));
    MDR_CHECK(limiter.allowAttempt("1.2.3.4", 100));
    MDR_CHECK(!limiter.allowAttempt("1.2.3.4", 200));

    // well past the 1-second window since the first two attempts
    MDR_CHECK(limiter.allowAttempt("1.2.3.4", 2'000'000));
}

MDR_TEST(rate_limiter_rejected_attempts_still_count_against_budget) {
    // A client hammering the endpoint faster than the window shouldn't be
    // able to "reset" its budget by retrying immediately.
    ConnectionRateLimiter limiter(2, 1'000'000);

    MDR_CHECK(limiter.allowAttempt("1.2.3.4", 0));
    MDR_CHECK(limiter.allowAttempt("1.2.3.4", 10));
    MDR_CHECK(!limiter.allowAttempt("1.2.3.4", 20));
    MDR_CHECK(!limiter.allowAttempt("1.2.3.4", 30));
    MDR_CHECK(!limiter.allowAttempt("1.2.3.4", 500'000));
}

MDR_TEST(rate_limiter_prune_stale_entries_removes_idle_keys) {
    ConnectionRateLimiter limiter(1, 1'000'000);
    MDR_CHECK(limiter.allowAttempt("stale-client", 0));

    limiter.pruneStaleEntries(5'000'000); // long past the window

    // after pruning, the key's history is gone, so a new attempt is allowed
    // immediately rather than being counted against old (expired) history
    MDR_CHECK(limiter.allowAttempt("stale-client", 5'000'000));
}
