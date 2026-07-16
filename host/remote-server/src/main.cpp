#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "host/logging_input_sink.h"
#include "host/net_server.h"
#include "host/synthetic_frame_source.h"

namespace {
volatile std::sig_atomic_t g_stopRequested = 0;

void handleSignal(int) {
    g_stopRequested = 1;
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
            config.pairingStateFilePath = nextArg() + "/paired_devices.txt";
        } else if (arg == "--pairing-code-ttl-s") {
            config.pairingCodeTtl = std::chrono::seconds(std::stoll(nextArg()));
        } else if (arg == "--stats-interval-ms") {
            config.statsLoggingIntervalUs = static_cast<uint64_t>(std::stoll(nextArg())) * 1000;
        } else if (arg == "--help") {
            std::printf(
                "Usage: melonds-remote-server [--bind ADDR] [--control-port N] "
                "[--input-port N] [--video-port N] [--timeout-ms N] [--auth-token TOKEN] "
                "[--state-dir PATH] [--pairing-code-ttl-s N] [--stats-interval-ms N]\n"
                "\n"
                "Phase 1 prototype: serves a synthetic 256x192 test-pattern bottom\n"
                "screen and logs received controller/touch state. Not yet wired to\n"
                "a real melonDS instance -- see docs/melonds-integration-analysis.md.\n"
                "\n"
                "If --auth-token is omitted (recommended for normal use), the server\n"
                "runs in pairing mode instead of accepting any client unauthenticated:\n"
                "an unrecognized connection attempt gets a 6-digit code printed to this\n"
                "log, which the user enters on the client once. The host then issues a\n"
                "persistent token the client remembers, so future reconnects are silent\n"
                "(spec section 13's 'six-digit pairing code' option).\n"
                "\n"
                "--state-dir PATH persists issued pairing tokens (as PATH/paired_devices.txt)\n"
                "so paired clients stay paired across host restarts. Without it, pairing\n"
                "still works but is forgotten when this process exits.\n"
                "\n"
                "--pairing-code-ttl-s sets how long a generated code stays valid\n"
                "(default 300 = 5 minutes).\n"
                "\n"
                "If --auth-token IS given, it's a static pre-shared secret checked\n"
                "instead of pairing mode entirely (spec section 13's 'pre-shared\n"
                "token' option) -- useful for scripting/CI (see tests/smoke_test.py).\n"
                "\n"
                "--stats-interval-ms controls how often aggregated diagnostics\n"
                "(input packet rate, out-of-order/malformed counts, video frame\n"
                "rate, dropped frames, latency) are logged (default 5000).\n");
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
