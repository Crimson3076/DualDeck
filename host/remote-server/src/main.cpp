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
        } else if (arg == "--stats-interval-ms") {
            config.statsLoggingIntervalUs = static_cast<uint64_t>(std::stoll(nextArg())) * 1000;
        } else if (arg == "--help") {
            std::printf(
                "Usage: melonds-remote-server [--bind ADDR] [--control-port N] "
                "[--input-port N] [--video-port N] [--timeout-ms N] [--auth-token TOKEN] "
                "[--stats-interval-ms N]\n"
                "\n"
                "Phase 1 prototype: serves a synthetic 256x192 test-pattern bottom\n"
                "screen and logs received controller/touch state. Not yet wired to\n"
                "a real melonDS instance -- see docs/melonds-integration-analysis.md.\n"
                "\n"
                "If --auth-token is omitted, the server accepts any client that can\n"
                "reach it (spec section 13 requires this to be a conscious, warned-\n"
                "about choice for anything beyond local testing).\n"
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
