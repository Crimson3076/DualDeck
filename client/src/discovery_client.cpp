#include "discovery_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <unordered_map>

#include "melonds_remote/protocol.h"

namespace melonds_remote::client {

std::vector<DiscoveredHost> discoverHosts(uint16_t discoveryPort, int timeoutMs) {
    std::unordered_map<std::string, DiscoveredHost> found;

    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::perror("socket (discovery)");
        return {};
    }

    int broadcast = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    sockaddr_in broadcastAddr{};
    broadcastAddr.sin_family = AF_INET;
    broadcastAddr.sin_port = htons(discoveryPort);
    broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST;

    ByteBuffer request = buildDiscoveryRequestPacket();
    ::sendto(fd, request.data(), request.size(), 0,
             reinterpret_cast<sockaddr*>(&broadcastAddr), sizeof(broadcastAddr));

    // A total-elapsed deadline, not a per-call timeout: SO_RCVTIMEO would
    // reset on every recvfrom(), so if replies kept trickling in just
    // before each timeout the whole scan could run far longer than
    // `timeoutMs`. select() lets the remaining budget be recomputed and
    // enforced across the whole loop instead.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    uint8_t buf[512];
    while (true) {
        auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) {
            break;
        }
        auto remainingUs = std::chrono::duration_cast<std::chrono::microseconds>(remaining).count();

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        timeval selectTimeout{};
        selectTimeout.tv_sec = static_cast<time_t>(remainingUs / 1'000'000);
        selectTimeout.tv_usec = static_cast<suseconds_t>(remainingUs % 1'000'000);

        int ready = ::select(fd + 1, &readfds, nullptr, nullptr, &selectTimeout);
        if (ready <= 0) {
            break; // timed out (or error) -- return whatever we collected
        }

        sockaddr_in fromAddr{};
        socklen_t fromLen = sizeof(fromAddr);
        ssize_t n = ::recvfrom(fd, buf, sizeof(buf), 0,
                                reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
        if (n <= 0) {
            continue;
        }

        auto header = parseHeader(buf, static_cast<size_t>(n));
        if (!header || header->protocolVersion != kProtocolVersion ||
            header->type != PacketType::DiscoveryResponse ||
            header->payloadSize > static_cast<size_t>(n) - kPacketHeaderWireSize) {
            continue; // not a well-formed DiscoveryResponse -- ignore
        }

        auto response = parseDiscoveryResponsePayload(buf + kPacketHeaderWireSize, header->payloadSize);
        if (!response) {
            continue;
        }

        char addrStr[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &fromAddr.sin_addr, addrStr, sizeof(addrStr));

        DiscoveredHost host;
        host.address = addrStr;
        host.hostName = response->hostName;
        host.controlPort = response->controlPort;
        host.inputPort = response->inputPort;
        host.videoPort = response->videoPort;
        found[host.address] = host; // last reply from a given address wins
    }

    ::close(fd);

    std::vector<DiscoveredHost> result;
    result.reserve(found.size());
    for (auto& [addr, host] : found) {
        result.push_back(std::move(host));
    }
    return result;
}

} // namespace melonds_remote::client
