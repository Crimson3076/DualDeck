#pragma once

// `dualdeck-client --discover` output: the hosts one LAN scan found, as a
// JSON array, for front ends (the Decky plugin) that want the client's own
// discovery instead of a second implementation of it.
//
//   [{"address":"192.168.1.50","name":"htpc","controlPort":8760,
//     "inputPort":8761,"videoPort":8762,"audioPort":8765,
//     "system":{"id":"nds","name":"Nintendo DS"},
//     "adapter":{"id":"melonds","name":"melonDS","version":"..."}}]

#include <string>
#include <vector>

#include "discovery_client.h"

namespace dualdeck::client {

std::string discoveredHostsToJson(const std::vector<DiscoveredHost>& hosts);

} // namespace dualdeck::client
