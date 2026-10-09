#include <string>
#include <vector>

#include "discovery_json.h"
#include "test_framework.h"

using namespace dualdeck::client;

MDR_TEST(discovered_hosts_to_json_empty_scan_is_an_empty_array) {
    MDR_CHECK_EQ(discoveredHostsToJson({}), std::string("[]"));
}

MDR_TEST(discovered_hosts_to_json_lists_every_field) {
    DiscoveredHost htpc;
    htpc.address = "192.168.1.50";
    htpc.hostName = "living \"room\"";
    htpc.system = {"nds", "Nintendo DS"};
    htpc.adapter = {"melonds", "melonDS", "1.0"};
    DiscoveredHost other;
    other.address = "192.168.1.51";
    other.controlPort = 9000;

    MDR_CHECK_EQ(discoveredHostsToJson({htpc, other}),
                 std::string("[{\"address\":\"192.168.1.50\",\"name\":\"living \\\"room\\\"\","
                             "\"controlPort\":8760,\"inputPort\":8761,\"videoPort\":8762,\"audioPort\":8765,"
                             "\"system\":{\"id\":\"nds\",\"name\":\"Nintendo DS\"},"
                             "\"adapter\":{\"id\":\"melonds\",\"name\":\"melonDS\",\"version\":\"1.0\"}},"
                             "{\"address\":\"192.168.1.51\",\"name\":\"\","
                             "\"controlPort\":9000,\"inputPort\":8761,\"videoPort\":8762,\"audioPort\":8765,"
                             "\"system\":{\"id\":\"\",\"name\":\"\"},"
                             "\"adapter\":{\"id\":\"\",\"name\":\"\",\"version\":\"\"}}]"));
}
