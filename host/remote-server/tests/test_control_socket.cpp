// ControlSocketServer and handleControlCommand() (control_socket.h):
// the local control API front ends use instead of the service's stdin.

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "host/control_socket.h"
#include "host/logging_input_sink.h"
#include "host/logging_mic_audio_sink.h"
#include "host/net_server.h"
#include "host/synthetic_frame_source.h"
#include "test_framework.h"

using namespace melonds_remote;
using namespace melonds_remote::host;

namespace {

// A socket path inside a fresh private directory (mkdtemp creates it
// 0700) under the test's working directory -- relative, so it stays well
// inside sun_path's limit however deep the build tree is -- and under a
// "sub" directory the server must create itself. Empty if the directory
// can't be made, which makes start() fail the test.
std::string tempSocketPath() {
    std::string dir = "control-socket-test-XXXXXX";
    if (::mkdtemp(dir.data()) == nullptr) return {};
    return dir + "/sub/host-control.sock";
}

// Removes tempSocketPath()'s directory when the test ends.
struct TempSocket {
    std::string path = tempSocketPath();
    ~TempSocket() {
        if (!path.empty()) std::filesystem::remove_all(path.substr(0, path.find('/')));
    }
};

sockaddr_un unixAddress(const std::string& path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.data(), std::min(path.size(), sizeof(addr.sun_path) - 1));
    return addr;
}

// Connects, sends `request` (one or more newline-terminated lines), and
// reads until `expectedLines` replies have arrived or the server closes.
std::string roundTrip(const std::string& socketPath, const std::string& request, int expectedLines) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr = unixAddress(socketPath);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return "<connect failed>";
    }
    ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);

    timeval timeout{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    std::string reply;
    char buf[512];
    int lines = 0;
    while (lines < expectedLines) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; ++i) {
            reply += buf[i];
            if (buf[i] == '\n') ++lines;
        }
    }
    ::close(fd);
    return reply;
}

} // namespace

MDR_TEST(control_socket_round_trips_each_line_through_the_handler) {
    const TempSocket temp;
    const std::string& path = temp.path;
    ControlSocketServer server(path, [](const std::string& line) { return "echo:" + line; });
    MDR_CHECK(server.start());

    struct stat st{};
    MDR_CHECK(::stat(path.c_str(), &st) == 0);
    MDR_CHECK_EQ(st.st_mode & 0777, 0600u);

    MDR_CHECK_EQ(roundTrip(path, "status\npending\r\n", 2), std::string("echo:status\necho:pending\n"));

    server.stop();
    MDR_CHECK(::stat(path.c_str(), &st) != 0); // socket file removed on stop
}

MDR_TEST(control_socket_replaces_a_stale_socket_but_not_a_live_one) {
    const TempSocket temp;
    const std::string& path = temp.path;
    ControlSocketServer first(path, [](const std::string&) { return std::string("first"); });
    MDR_CHECK(first.start());

    ControlSocketServer second(path, [](const std::string&) { return std::string("second"); });
    MDR_CHECK(!second.start()); // first still answers: left alone
    MDR_CHECK_EQ(roundTrip(path, "x\n", 1), std::string("first\n"));

    // Simulate a killed process: the file stays but nothing listens.
    first.stop();
    int staleFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr = unixAddress(path);
    MDR_CHECK(::bind(staleFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    ::close(staleFd);

    MDR_CHECK(second.start());
    MDR_CHECK_EQ(roundTrip(path, "x\n", 1), std::string("second\n"));
    second.stop();
}

MDR_TEST(control_socket_rejects_an_overlong_line) {
    const TempSocket temp;
    const std::string& path = temp.path;
    ControlSocketServer server(path, [](const std::string&) { return std::string("ok"); });
    MDR_CHECK(server.start());
    std::string reply = roundTrip(path, std::string(4096, 'a'), 1);
    MDR_CHECK(reply.find("line too long") != std::string::npos);
    server.stop();
}

MDR_TEST(handle_control_command_reports_status_and_answers_approvals) {
    LoggingInputSink inputSink;
    SyntheticFrameSource frameSource(60);
    LoggingMicAudioSink micSink;
    NetServerConfig config;
    NetServer server(config, inputSink, frameSource, micSink);

    std::string status = handleControlCommand("status", server, nullptr);
    MDR_CHECK(status.rfind("{\"ok\":true,\"mode\":\"emulation\",\"overridden\":false", 0) == 0);
    MDR_CHECK(status.find("\"system\":{\"id\":\"synthetic\"") != std::string::npos);
    MDR_CHECK(status.find("\"pending\":[]") != std::string::npos);

    MDR_CHECK_EQ(handleControlCommand("pending", server, nullptr), std::string("{\"ok\":true,\"pending\":[]}"));
    MDR_CHECK_EQ(handleControlCommand("approve abc", server, nullptr),
                 std::string("{\"ok\":false,\"error\":\"no pending request matches 'abc'\"}"));
    MDR_CHECK_EQ(handleControlCommand("deny", server, nullptr),
                 std::string("{\"ok\":false,\"error\":\"deny needs a device id\"}"));
    MDR_CHECK_EQ(handleControlCommand("hostcontrol", server, nullptr),
                 std::string("{\"ok\":false,\"error\":\"hostcontrol needs the service running with --adapter-ipc\"}"));
    MDR_CHECK_EQ(handleControlCommand("bogus \"x\"", server, nullptr),
                 std::string("{\"ok\":false,\"error\":\"unknown command 'bogus'\"}"));
}

MDR_TEST(default_control_socket_path_sits_next_to_the_adapter_socket) {
    const char* saved = std::getenv("XDG_RUNTIME_DIR");
    std::string savedValue = saved ? saved : "";
    ::setenv("XDG_RUNTIME_DIR", "/run/user/1234", 1);
    MDR_CHECK_EQ(defaultControlSocketPath(), std::string("/run/user/1234/dualdeck/host-control.sock"));
    if (saved) {
        ::setenv("XDG_RUNTIME_DIR", savedValue.c_str(), 1);
    } else {
        ::unsetenv("XDG_RUNTIME_DIR");
    }
}
