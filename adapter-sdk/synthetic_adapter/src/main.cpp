// A standalone process: runs SyntheticEmulatorAdapter and connects it
// to a Host Service's adapter IPC socket via AdapterIpcClient (GitHub
// issue #28 Phase 2). Not wired into any packaged release or default
// launch path -- a development/testing tool proving the IPC channel
// end to end with a real separate process, not just an in-test-process
// server+client pair. See docs/adr/0001-host-service-and-adapter-architecture.md.
//
// There is no AdapterIpcServer running in production anywhere yet
// (host/remote-server's NetServer does not speak this channel) -- this
// binary is only useful today pointed at another instance of itself's
// test harness, or a future Host Service build that does listen on it.

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "melonds_remote/adapter/ipc/adapter_ipc_client.h"
#include "melonds_remote/adapter/synthetic/synthetic_emulator_adapter.h"

namespace {
volatile std::sig_atomic_t g_stopRequested = 0;
void handleSignal(int) {
    g_stopRequested = 1;
}
} // namespace

int main(int argc, char** argv) {
    std::string socketPath; // empty -> defaultAdapterSocketPath()
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--socket" && i + 1 < argc) {
            socketPath = argv[++i];
        } else if (arg == "--help") {
            std::printf(
                "Usage: dualdeck-synthetic-adapter [--socket PATH]\n\n"
                "Runs a self-contained synthetic (test-pattern) emulator adapter and\n"
                "connects it to a Host Service over the local adapter IPC channel\n"
                "(GitHub issue #28 Phase 2). --socket overrides the default path\n"
                "($XDG_RUNTIME_DIR/dualdeck/adapter.sock). Retries the connection\n"
                "with a fixed backoff until a Host Service is listening.\n");
            return 0;
        } else {
            std::fprintf(stderr, "unrecognized argument: %s\n", arg.c_str());
            return 1;
        }
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    melonds_remote::adapter::synthetic::SyntheticEmulatorAdapter adapter;
    adapter.start();

    melonds_remote::adapter::ipc::AdapterIpcClient client(adapter, socketPath);

    std::printf("dualdeck-synthetic-adapter: connecting...\n");
    while (!g_stopRequested) {
        if (!client.isConnected()) {
            if (client.connect()) {
                std::printf("dualdeck-synthetic-adapter: connected\n");
            } else {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
        }
        struct timespec ts{0, 200'000'000};
        nanosleep(&ts, nullptr);
        if (!client.isConnected()) {
            std::printf("dualdeck-synthetic-adapter: disconnected, will retry\n");
        }
    }

    std::printf("dualdeck-synthetic-adapter: shutting down\n");
    client.disconnect();
    adapter.stop();
    return 0;
}
