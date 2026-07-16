#pragma once

// LAN host discovery (spec section 8.1's "future versions" item, now
// implemented): broadcasts a DiscoveryRequest and collects whatever
// DiscoveryResponse replies come back within a short window, so the
// client doesn't need to be given a host address if there's exactly one
// (or the user picks from a short list) melonds-remote host on the LAN.
//
// Deliberately a one-shot blocking call rather than a background
// service -- discovery only matters at startup/reconnect-selection time,
// not continuously, so there's no long-lived socket/thread to manage.

#include <cstdint>
#include <string>
#include <vector>

namespace melonds_remote::client {

struct DiscoveredHost {
    std::string address;  // dotted-quad IPv4, e.g. "192.168.1.50"
    std::string hostName;
    uint16_t controlPort = 8760;
    uint16_t inputPort = 8761;
    uint16_t videoPort = 8762;
};

// Broadcasts a DiscoveryRequest on `discoveryPort` and collects replies
// for `timeoutMs` milliseconds. Deduplicates by source address (keeps
// the most recent reply if a host somehow answers more than once).
// Returns an empty vector if nothing replied in time -- not an error,
// just "no host found yet" (the caller is expected to retry).
std::vector<DiscoveredHost> discoverHosts(uint16_t discoveryPort, int timeoutMs);

} // namespace melonds_remote::client
