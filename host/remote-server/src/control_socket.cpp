#include "host/control_socket.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "host/mode_coordinator.h"
#include "host/net_server.h"
#include "dualdeck/adapter/ipc/socket_path.h"
#include "dualdeck/json_string.h"

namespace dualdeck::host {

namespace {

// A command is a verb and at most one device id; anything longer is not
// a command this socket understands.
constexpr size_t kMaxLineBytes = 1024;
constexpr int kPollTimeoutMs = 200;

std::string errorReply(const std::string& message) {
    return "{\"ok\":false,\"error\":" + jsonQuote(message) + "}";
}

std::string pendingJson(NetServer& server) {
    std::string out = "[";
    bool first = true;
    for (const auto& request : server.pendingRequests()) {
        if (!first) out += ",";
        first = false;
        out += "{\"id\":" + jsonQuote(request.deviceId) + ",\"name\":" + jsonQuote(request.clientName) +
               ",\"address\":" + jsonQuote(request.address) + "}";
    }
    out += "]";
    return out;
}

} // namespace

std::string defaultControlSocketPath() {
    std::string adapterPath = adapter::ipc::defaultAdapterSocketPath();
    if (adapterPath.empty()) return {};
    return adapterPath.substr(0, adapterPath.find_last_of('/') + 1) + "host-control.sock";
}

std::string handleControlCommand(const std::string& line, NetServer& server, ModeCoordinator* coordinator) {
    std::istringstream iss(line);
    std::string cmd, arg;
    iss >> cmd >> arg;

    if (cmd == "status") {
        const SystemIdentity system = server.currentSystemIdentity();
        const AdapterIdentity adapter = server.currentAdapterIdentity();
        return std::string("{\"ok\":true,\"mode\":") +
               (server.currentMode() == HostMode::HostControl ? "\"host-control\"" : "\"emulation\"") +
               ",\"overridden\":" + (coordinator && coordinator->isOverridden() ? "true" : "false") +
               ",\"system\":{\"id\":" + jsonQuote(system.systemId) + ",\"name\":" + jsonQuote(system.systemName) +
               "},\"adapter\":{\"id\":" + jsonQuote(adapter.adapterId) +
               ",\"name\":" + jsonQuote(adapter.adapterName) +
               ",\"version\":" + jsonQuote(adapter.adapterVersion) + "},\"pending\":" + pendingJson(server) + "}";
    }
    if (cmd == "pending") {
        return "{\"ok\":true,\"pending\":" + pendingJson(server) + "}";
    }
    if (cmd == "approve" || cmd == "deny") {
        if (arg.empty()) return errorReply(cmd + " needs a device id");
        bool matched = cmd == "approve" ? server.approveDevice(arg) : server.denyDevice(arg);
        return matched ? "{\"ok\":true}" : errorReply("no pending request matches '" + arg + "'");
    }
    if (cmd == "hostcontrol" || cmd == "resume") {
        if (!coordinator) return errorReply(cmd + " needs the service running with --adapter-ipc");
        if (cmd == "hostcontrol") {
            coordinator->forceHostControl();
        } else {
            coordinator->clearOverride();
        }
        return "{\"ok\":true}";
    }
    if (cmd.empty()) return errorReply("empty command");
    return errorReply("unknown command '" + cmd + "'");
}

ControlSocketServer::ControlSocketServer(std::string socketPath, Handler handler)
    : socketPath_(std::move(socketPath)), handler_(std::move(handler)) {}

ControlSocketServer::~ControlSocketServer() {
    stop();
}

bool ControlSocketServer::start() {
    if (running_.load()) return true;

    sockaddr_un addr{};
    if (socketPath_.empty() || socketPath_.size() >= sizeof(addr.sun_path)) {
        std::fprintf(stderr, "ControlSocketServer: unusable socket path '%s'\n", socketPath_.c_str());
        return false;
    }
    if (!adapter::ipc::ensureSocketDirectory(socketPath_)) {
        std::fprintf(stderr, "ControlSocketServer: couldn't create the directory for %s\n", socketPath_.c_str());
        return false;
    }

    listenFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        std::perror("ControlSocketServer: socket");
        return false;
    }

    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, socketPath_.c_str(), socketPath_.size() + 1);

    // A previous run that was killed leaves its socket file behind, and
    // bind() refuses an existing path -- but a socket some other running
    // service still answers on is left alone rather than taken over.
    if (int probeFd = ::socket(AF_UNIX, SOCK_STREAM, 0); probeFd >= 0) {
        bool inUse = ::connect(probeFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        ::close(probeFd);
        if (inUse) {
            std::fprintf(stderr, "ControlSocketServer: %s is already served by another process\n",
                         socketPath_.c_str());
            ::close(listenFd_);
            listenFd_ = -1;
            return false;
        }
    }
    ::unlink(socketPath_.c_str());
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("ControlSocketServer: bind");
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    ::chmod(socketPath_.c_str(), 0600);
    if (::listen(listenFd_, 4) < 0) {
        std::perror("ControlSocketServer: listen");
        ::close(listenFd_);
        listenFd_ = -1;
        ::unlink(socketPath_.c_str());
        return false;
    }

    running_ = true;
    acceptThread_ = std::thread(&ControlSocketServer::acceptLoop, this);
    return true;
}

void ControlSocketServer::stop() {
    if (!running_.exchange(false)) return;
    if (acceptThread_.joinable()) acceptThread_.join();
    if (listenFd_ >= 0) ::close(listenFd_);
    listenFd_ = -1;
    ::unlink(socketPath_.c_str());
}

void ControlSocketServer::acceptLoop() {
    while (running_.load()) {
        pollfd pfd{listenFd_, POLLIN, 0};
        int ready = ::poll(&pfd, 1, kPollTimeoutMs);
        if (ready <= 0) continue;

        int clientFd = ::accept(listenFd_, nullptr, nullptr);
        if (clientFd < 0) continue;
        serveClient(clientFd);
        ::close(clientFd);
    }
}

// Clients are local and short-lived (connect, send a command or a few,
// read the replies, disconnect), so they are served one at a time on the
// accept thread.
void ControlSocketServer::serveClient(int clientFd) {
    std::string buffer;
    char chunk[256];
    while (running_.load()) {
        pollfd pfd{clientFd, POLLIN, 0};
        int ready = ::poll(&pfd, 1, kPollTimeoutMs);
        if (ready == 0) continue;
        if (ready < 0) return;

        ssize_t n = ::recv(clientFd, chunk, sizeof(chunk), 0);
        if (n <= 0) return;
        buffer.append(chunk, static_cast<size_t>(n));

        size_t newline;
        while ((newline = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();

            std::string reply = handler_(line) + "\n";
            if (::send(clientFd, reply.data(), reply.size(), MSG_NOSIGNAL) < 0) return;
        }
        if (buffer.size() > kMaxLineBytes) {
            std::string reply = "{\"ok\":false,\"error\":\"line too long\"}\n";
            ::send(clientFd, reply.data(), reply.size(), MSG_NOSIGNAL);
            return;
        }
    }
}

} // namespace dualdeck::host
