#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "host/logging_input_sink.h"
#include "host/net_server.h"
#include "host/synthetic_frame_source.h"

namespace {
volatile std::sig_atomic_t g_stopRequested = 0;

void handleSignal(int) {
    g_stopRequested = 1;
}

// Reads "approve <id>" / "deny <id>" / "list" lines from stdin for as
// long as it stays open, so an operator running this console binary
// interactively can respond to device-approval requests without a GUI.
// Runs detached: when stdin closes (EOF -- e.g. this process was started
// without a terminal, or under CI) std::getline just returns false and
// the thread exits on its own; there's nothing to clean up, so there's
// no need to join it against shutdown.
void approvalConsoleLoop(melonds_remote::host::NetServer& server) {
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream iss(line);
        std::string cmd, arg;
        iss >> cmd >> arg;

        if (cmd == "approve" && !arg.empty()) {
            if (server.approveDevice(arg)) {
                std::printf("[approval] approved device %s\n", arg.c_str());
            } else {
                std::printf("[approval] no pending request matches '%s'\n", arg.c_str());
            }
        } else if (cmd == "deny" && !arg.empty()) {
            if (server.denyDevice(arg)) {
                std::printf("[approval] denied device %s\n", arg.c_str());
            } else {
                std::printf("[approval] no pending request matches '%s'\n", arg.c_str());
            }
        } else if (cmd == "list") {
            auto pending = server.pendingRequests();
            if (pending.empty()) {
                std::printf("[approval] no pending requests\n");
            } else {
                for (const auto& r : pending) {
                    std::printf("  %s  '%s' at %s\n", r.deviceId.c_str(), r.clientName.c_str(),
                                r.address.c_str());
                }
            }
        } else if (!cmd.empty()) {
            std::printf("[approval] unrecognized command '%s' -- try 'list', 'approve <id>', "
                        "or 'deny <id>'\n",
                        cmd.c_str());
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    melonds_remote::host::NetServerConfig config;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto nextArg = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", arg.c_str());
                std::exit(1);
            }
            return argv[++i];
        };

        if (arg == "--bind") {
            config.bindAddress = nextArg();
        } else if (arg == "--control-port") {
            config.controlPort = static_cast<uint16_t>(std::stoi(nextArg()));
        } else if (arg == "--input-port") {
            config.inputPort = static_cast<uint16_t>(std::stoi(nextArg()));
        } else if (arg == "--video-port") {
            config.videoPort = static_cast<uint16_t>(std::stoi(nextArg()));
        } else if (arg == "--timeout-ms") {
            config.inputTimeoutUs = static_cast<uint64_t>(std::stoll(nextArg())) * 1000;
        } else if (arg == "--auth-token") {
            config.authToken = nextArg();
        } else if (arg == "--state-dir") {
            config.approvalStateFilePath = nextArg() + "/approved_devices.txt";
        } else if (arg == "--pending-request-ttl-s") {
            config.pendingRequestTtl = std::chrono::seconds(std::stoll(nextArg()));
        } else if (arg == "--stats-interval-ms") {
            config.statsLoggingIntervalUs = static_cast<uint64_t>(std::stoll(nextArg())) * 1000;
        } else if (arg == "--discovery-port") {
            config.discoveryPort = static_cast<uint16_t>(std::stoi(nextArg()));
        } else if (arg == "--host-name") {
            config.hostName = nextArg();
        } else if (arg == "--no-discovery") {
            config.discoveryEnabled = false;
        } else if (arg == "--help") {
            std::printf(
                "Usage: melonds-remote-server [--bind ADDR] [--control-port N] "
                "[--input-port N] [--video-port N] [--timeout-ms N] [--auth-token TOKEN] "
                "[--state-dir PATH] [--pending-request-ttl-s N] [--stats-interval-ms N] "
                "[--discovery-port N] [--host-name NAME] [--no-discovery]\n"
                "\n"
                "Phase 1 prototype: serves a synthetic 256x192 test-pattern bottom\n"
                "screen and logs received controller/touch state. Not yet wired to\n"
                "a real melonDS instance -- see docs/melonds-integration-analysis.md.\n"
                "\n"
                "If --auth-token is omitted (recommended for normal use), the server\n"
                "runs in device-approval mode instead of accepting any client\n"
                "unauthenticated: an unrecognized client's connection request is queued\n"
                "here (this process's stdout/stdin) for you to approve or deny by name\n"
                "and address -- type 'approve <id>', 'deny <id>', or 'list' and press\n"
                "Enter. No code is ever typed on the client (spec section 13's later\n"
                "pairing options, adapted since Steam Input's virtual keyboard doesn't\n"
                "reliably come up in Gaming Mode -- see docs/known-limitations.md).\n"
                "Once approved, that same device reconnects silently forever, no\n"
                "reprompting, unless the host's approved-device state is deleted.\n"
                "\n"
                "--state-dir PATH persists approved device identities (as\n"
                "PATH/approved_devices.txt) so they stay approved across host restarts.\n"
                "Without it, approval still works but is forgotten when this process exits.\n"
                "\n"
                "--pending-request-ttl-s sets how long a pending request is kept without\n"
                "a fresh retry before being evicted from the queue (default 60).\n"
                "\n"
                "If --auth-token IS given, it's a static pre-shared secret checked\n"
                "instead of device-approval mode entirely (spec section 13's 'pre-shared\n"
                "token' option) -- useful for scripting/CI (see tests/smoke_test.py).\n"
                "\n"
                "--stats-interval-ms controls how often aggregated diagnostics\n"
                "(input packet rate, out-of-order/malformed counts, video frame\n"
                "rate, dropped frames, latency) are logged (default 5000).\n"
                "\n"
                "LAN discovery is on by default (spec section 8.1): the client can\n"
                "broadcast to find this host instead of needing --host on its side.\n"
                "This only ever reveals a host name and port numbers, no auth\n"
                "bypass -- --discovery-port sets the UDP port (default 8763),\n"
                "--host-name sets the friendly name shown to clients (default: this\n"
                "machine's hostname), --no-discovery turns it off entirely.\n"
                "\n"
                "--bind defaults to 0.0.0.0 (all interfaces) so a client elsewhere\n"
                "on the LAN can actually reach what discovery just told it about --\n"
                "pass --bind 127.0.0.1 explicitly to restrict to same-machine-only\n"
                "testing instead.\n");
            return 0;
        } else {
            std::fprintf(stderr, "unrecognized argument: %s\n", arg.c_str());
            return 1;
        }
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    melonds_remote::host::LoggingInputSink inputSink;
    melonds_remote::host::SyntheticFrameSource frameSource(60);
    frameSource.start();

    melonds_remote::host::NetServer server(config, inputSink, frameSource);
    server.start();

    if (config.authToken.empty()) {
        std::thread(approvalConsoleLoop, std::ref(server)).detach();
    }

    std::printf("melonds-remote-server running. Press Ctrl+C to stop.\n");
    while (!g_stopRequested) {
        struct timespec ts{0, 100'000'000};
        nanosleep(&ts, nullptr);
    }

    std::printf("shutting down...\n");
    server.stop();
    frameSource.stop();
    return 0;
}
