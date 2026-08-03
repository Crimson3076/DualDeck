#pragma once

// Implements device-approval authentication (replaces the earlier
// 6-digit-pairing-code flow, which required the client to type a code --
// unworkable on Steam Deck when Steam Input's virtual keyboard doesn't
// come up, see docs/known-limitations.md):
//
//   1. The client generates a random, persistent device identity once
//      (see client/src/device_identity.h) and sends the same value on
//      every Hello, to every host, forever -- there is no code to type on
//      either side.
//   2. An unrecognized device identity is queued as a pending request
//      (surfaced to whoever is at the host -- console log, and
//      optionally a GUI hook) and the handshake is rejected with
//      `HelloRejectReason::ApprovalRequired`. The client's existing
//      auto-reconnect loop keeps retrying with the same identity; no
//      client-side action is needed or possible.
//   3. A human at the host approves (or denies) the pending request.
//      Approval persists the device identity to disk; every future Hello
//      from that same identity is accepted immediately, no re-approval
//      needed unless the host's approved-device state is deleted.
//
// Independent of NetServerConfig::authToken, which remains a static
// pre-shared secret for scripting/CI use (checked first by NetServer;
// this class is only consulted when no static token is configured).

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace melonds_remote::host {

class DeviceApprovalManager {
public:
    // `stateFilePath` is where approved device identities are persisted
    // (one per line) so an approved client survives a host restart. An
    // empty path disables persistence -- approval still works within the
    // current process lifetime, just isn't remembered across restarts
    // (used by tests that shouldn't touch the filesystem).
    //
    // `pendingRequestTtl` bounds how long a pending request is kept
    // without a fresh retry before it's silently evicted -- a client that
    // gave up (powered off, pointed at a different host) shouldn't leave
    // a permanent stale entry in the approval queue. A live client keeps
    // its entry fresh on its own via the auto-reconnect loop's repeated
    // retries, each of which refreshes lastSeen.
    explicit DeviceApprovalManager(std::string stateFilePath,
                                   std::chrono::seconds pendingRequestTtl = std::chrono::seconds(60));

    DeviceApprovalManager(const DeviceApprovalManager&) = delete;
    DeviceApprovalManager& operator=(const DeviceApprovalManager&) = delete;

    enum class CheckResult {
        Approved, // deviceId is in the persisted approved set: accept immediately
        Pending,  // not yet approved: request recorded/refreshed, reject with ApprovalRequired
    };

    struct PendingRequest {
        std::string deviceId;
        std::string clientName;
        std::string address;
    };

    // Checks `deviceId` (the Hello payload's authToken field, repurposed
    // as a persistent device identity) against the approved-device set.
    // As a side effect, records or refreshes a pending-approval entry
    // when it doesn't match -- an empty deviceId is never recorded as
    // pending (nothing to approve/deny later) and always returns Pending.
    CheckResult check(const std::string& deviceId, const std::string& clientName,
                       const std::string& address);

    // Real user request, 2026-08-03: "auto-trigger [a host update] for
    // already-approved devices" on an AppVersionMismatch rejection --
    // that check happens *before* check() above is ever reached (see
    // net_server.cpp's Hello handling, version compatibility is checked
    // before authentication/approval on purpose), so it needs its own,
    // read-only lookup rather than reusing check(): unlike check(), this
    // never records or refreshes a pending-approval entry for an unknown
    // deviceId -- a client whose version doesn't even match yet has no
    // business cluttering the pending-approval queue with an entry a
    // human at the host can't meaningfully act on until versions match
    // anyway.
    bool isApproved(const std::string& deviceId) const;

    // Approves a pending request by exact deviceId or unambiguous prefix
    // (operator convenience -- typing the first several characters of the
    // id shown in the log is enough). Persists the full deviceId to disk
    // and removes it from the pending set. Returns false if no pending
    // request matches (exactly, or as a unique prefix).
    bool approve(const std::string& deviceIdOrPrefix);

    // Denies (simply discards) a pending request by exact deviceId or
    // unambiguous prefix. Returns false if no pending request matches. A
    // denied client's automatic retries will just reappear as pending
    // again -- this isn't a permanent block, only "not right now".
    bool deny(const std::string& deviceIdOrPrefix);

    // Current pending requests, oldest first, after evicting any that
    // haven't been refreshed within pendingRequestTtl.
    std::vector<PendingRequest> pendingRequests();

    // Fired (on whatever thread calls check()/approve()/deny(), or the
    // watchdog thread that evicts stale entries) whenever the *set* of
    // pending device ids actually changes -- a new arrival, an eviction,
    // or an approve/deny -- not on every refresh of an already-pending
    // entry, so a UI hook isn't re-prompted on every retry. Callers
    // needing UI-thread delivery (e.g. a Qt dialog) must marshal it
    // themselves.
    void setOnPendingRequestsChanged(std::function<void(std::vector<PendingRequest>)> callback);

    // Evicts any pending requests that have gone stale (no retry within
    // pendingRequestTtl). check() already does this internally as a side
    // effect too, but a host that nobody is actively retrying against
    // (e.g. its one pending client gave up) wouldn't otherwise ever prune
    // its queue -- call this periodically from a watchdog loop.
    void evictStale();

private:
    struct PendingEntry {
        std::string clientName;
        std::string address;
        std::chrono::steady_clock::time_point lastSeen;
    };

    void loadApprovedLocked();
    void persistApprovedLocked(const std::string& deviceId);
    void evictStaleLocked();
    void notifyChangedLocked();
    // Returns the exact deviceId matching `idOrPrefix` in pending_, either
    // an exact key match or the single pending entry whose key starts
    // with it. Returns empty string if there's no match or the prefix is
    // ambiguous (matches more than one).
    std::string resolvePendingLocked(const std::string& idOrPrefix) const;

    mutable std::mutex mutex_;
    std::string stateFilePath_;
    std::chrono::seconds pendingRequestTtl_;

    std::unordered_set<std::string> approvedDevices_;
    std::unordered_map<std::string, PendingEntry> pending_; // keyed by deviceId
    std::vector<std::string> pendingOrder_; // insertion order, for stable listing

    std::function<void(std::vector<PendingRequest>)> onPendingRequestsChanged_;
};

} // namespace melonds_remote::host
