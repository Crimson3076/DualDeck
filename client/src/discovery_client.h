#pragma once

// LAN host discovery (spec section 8.1's "future versions" item, now
// implemented): broadcasts a DiscoveryRequest and collects whatever
// DiscoveryResponse replies come back within a short window, so the
// client doesn't need to be given a host address if there's exactly one
// (or the user picks from a short list) melonds-remote host on the LAN.
//
// A single call is a one-shot blocking operation -- no long-lived socket
// of its own. main.cpp's discoverAndSelectHost() calls this repeatedly
// on a dedicated background thread (not the render/input thread) for as
// long as the host-picker screen is shown, so the picker keeps
// refreshing without ever blocking event/frame handling; see
// `cancel` below for how that thread gets torn down promptly.

#include <atomic>
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
// for up to `timeoutMs` milliseconds. Deduplicates by source address
// (keeps the most recent reply if a host somehow answers more than
// once). Returns an empty vector if nothing replied in time -- not an
// error, just "no host found yet" (the caller is expected to retry).
//
// `cancel`, if non-null, is polled roughly every kCancelPollMs and ends
// the scan early (returning whatever was collected so far) once it
// reads true -- lets a caller running this in a loop on a background
// thread stop within a bounded, short delay instead of waiting out the
// rest of whatever `timeoutMs` remained.
std::vector<DiscoveredHost> discoverHosts(uint16_t discoveryPort, int timeoutMs,
                                           const std::atomic<bool>* cancel = nullptr);

} // namespace melonds_remote::client
