#include "discovery_json.h"

#include "dualdeck/json_string.h"

namespace dualdeck::client {

std::string discoveredHostsToJson(const std::vector<DiscoveredHost>& hosts) {
    std::string out = "[";
    for (size_t i = 0; i < hosts.size(); ++i) {
        const DiscoveredHost& h = hosts[i];
        if (i > 0) out += ",";
        out += "{\"address\":" + jsonQuote(h.address) + ",\"name\":" + jsonQuote(h.hostName) +
               ",\"controlPort\":" + std::to_string(h.controlPort) +
               ",\"inputPort\":" + std::to_string(h.inputPort) +
               ",\"videoPort\":" + std::to_string(h.videoPort) +
               ",\"audioPort\":" + std::to_string(h.audioPort) +
               ",\"system\":{\"id\":" + jsonQuote(h.system.systemId) +
               ",\"name\":" + jsonQuote(h.system.systemName) + "}" +
               ",\"adapter\":{\"id\":" + jsonQuote(h.adapter.adapterId) +
               ",\"name\":" + jsonQuote(h.adapter.adapterName) +
               ",\"version\":" + jsonQuote(h.adapter.adapterVersion) + "}}";
    }
    out += "]";
    return out;
}

} // namespace dualdeck::client
