#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>

#include "host/adapter_bridge.h"
#include "host/host_control_adapter.h"
#include "host/kdialog_approval_prompt.h"
#include "host/logging_input_sink.h"
#include "host/logging_mic_audio_sink.h"
#include "host/mode_coordinator.h"
#include "host/net_server.h"
#include "host/synthetic_frame_source.h"
#include "melonds_remote/adapter/ipc/adapter_ipc_server.h"

namespace {
volatile std::sig_atomic_t g_stopRequested = 0;

void handleSignal(int) {
    g_stopRequested = 1;
}

// Reads "approve <id>" / "deny <id>" / "list" lines from stdin for as
// long as it stays open, so an operator running this console binary
// interactively can respond to device-approval requests without a GUI.
// `coordinator` is non-null only in --adapter-ipc mode (GitHub issue #4
// Phase D), adding "hostcontrol"/"resume"/"mode" manual-override
// commands to the same loop -- deliberately one loop, not two: two
// threads both calling std::getline(std::cin, ...) concurrently would
// race on stdin. Runs detached: when stdin closes (EOF -- e.g. this
// process was started without a terminal, or under CI) std::getline
// just returns false and the thread exits on its own; there's nothing
// to clean up, so there's no need to join it against shutdown.
void consoleLoop(melonds_remote::host::NetServer& server, melonds_remote::host::ModeCoordinator* coordinator) {
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
        } else if (coordinator && cmd == "hostcontrol") {
            coordinator->forceHostControl();
            std::printf("[mode] forcing host-control mode (type 'resume' to let it follow the "
                        "adapter connection again)\n");
        } else if (coordinator && cmd == "resume") {
            coordinator->clearOverride();
            std::printf("[mode] override cleared -- mode now follows adapter connection state\n");
        } else if (coordinator && cmd == "mode") {
            std::printf("[mode] currently: %s%s\n",
                        server.currentMode() == melonds_remote::HostMode::HostControl ? "host-control"
                                                                                        : "emulation",
                        coordinator->isOverridden() ? " (manually forced)" : "");
        } else if (!cmd.empty()) {
            std::printf("[console] unrecognized command '%s' -- try 'list', 'approve <id>', 'deny <id>'%s\n",
                        cmd.c_str(), coordinator ? ", 'hostcontrol', 'resume', or 'mode'" : "");
        }
    }
}

// Bridges DeviceApprovalManager's pending-request notifications (via
// NetServerConfig::onPendingRequestsChanged) to a kdialog Yes/No popup, so
// approving a device works with just a mouse/controller click -- no typing,
// and no visible console needed (see kdialog_approval_prompt.h). This is
// what lets host-control mode and out-of-process adapters (e.g. Azahar)
// use the same zero-typing approval flow melonDS's own in-process
// integration already has, instead of requiring a static shared secret.
//
// Tracks which device ids have already been prompted so a dialog isn't
// re-popped on every retry from the same still-pending client -- only a
// genuinely new arrival (or a re-arrival after this host forgot about it,
// e.g. after a restart) gets a fresh prompt.
class KdialogApprovalHook {
public:
    void setServer(melonds_remote::host::NetServer* server) {
        std::lock_guard<std::mutex> lock(mutex_);
        server_ = server;
    }

    void onPendingRequestsChanged(
        std::vector<melonds_remote::host::DeviceApprovalManager::PendingRequest> pending) {
        std::vector<melonds_remote::host::DeviceApprovalManager::PendingRequest> newlyPending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::unordered_set<std::string> stillPending;
            for (const auto& req : pending) {
                stillPending.insert(req.deviceId);
                if (prompted_.insert(req.deviceId).second) {
                    newlyPending.push_back(req);
                }
            }
            for (auto it = prompted_.begin(); it != prompted_.end();) {
                if (!stillPending.count(*it)) {
                    it = prompted_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const auto& req : newlyPending) {
            std::thread([this, req]() { promptAndDecide(req); }).detach();
        }
    }

private:
    void promptAndDecide(const melonds_remote::host::DeviceApprovalManager::PendingRequest& req) {
        auto result = melonds_remote::host::promptDeviceApprovalViaKdialog(req.clientName, req.address);

        // approveDevice()/denyDevice() synchronously invoke
        // DeviceApprovalManager's notifyChangedLocked(), which re-enters
        // onPendingRequestsChanged() (and thus this same object) on this
        // same thread -- so server_ must be read into a local and mutex_
        // released *before* calling either, or that reentrant call would
        // deadlock trying to re-lock mutex_ (a plain, non-recursive mutex).
        melonds_remote::host::NetServer* server;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            server = server_;
        }
        if (server == nullptr) {
            return;
        }
        if (result == melonds_remote::host::KdialogPromptResult::Approved) {
            server->approveDevice(req.deviceId);
        } else if (result == melonds_remote::host::KdialogPromptResult::Denied) {
            server->denyDevice(req.deviceId);
        }
        // Unavailable: leave pending -- the console ("list"/"approve <id>"/
        // "deny <id>") path documented in --help still works regardless.
    }

    std::mutex mutex_;
    melonds_remote::host::NetServer* server_ = nullptr;
    std::unordered_set<std::string> prompted_;
};

// Shared by both the default (synthetic-stub) and --adapter-ipc code
// paths in main() below: starts the console loop if applicable, blocks
// until SIGINT/SIGTERM, then returns so the caller can tear down
// whatever sink/frame-source objects it constructed.
void runUntilStopped(melonds_remote::host::NetServer& server, const melonds_remote::host::NetServerConfig& config,
                      melonds_remote::host::ModeCoordinator* coordinator = nullptr) {
    server.start();

    if (config.authToken.empty() || coordinator) {
        std::thread(consoleLoop, std::ref(server), coordinator).detach();
    }

    std::printf("DualDeck Host Service running. Press Ctrl+C to stop.\n");
    while (!g_stopRequested) {
        struct timespec ts{0, 100'000'000};
        nanosleep(&ts, nullptr);
    }

    std::printf("shutting down...\n");
    server.stop();
}
} // namespace

int main(int argc, char** argv) {
    melonds_remote::host::NetServerConfig config;
    // GitHub issue #28 Phase 2: when set, this process is driven by a
    // real adapter connected over the local IPC channel
    // (adapter-sdk/ipc/) instead of the built-in synthetic-stub
    // LoggingInputSink/SyntheticFrameSource pair -- see
    // docs/adr/0001-host-service-and-adapter-architecture.md section 4.
    bool useAdapterIpc = false;
    std::string adapterSocketPath; // empty -> defaultAdapterSocketPath()
    // Whether --system-id/--system-name or --adapter-id/--adapter-name/
    // --adapter-version were explicitly given -- if so, they win over
    // whatever a connected adapter reports in --adapter-ipc mode (see
    // below); otherwise the connected adapter's own real identity
    // (e.g. dualdeck-synthetic-adapter's "synthetic"/"Synthetic Test
    // System") replaces the static default/CLI-flag identity once it's
    // known, so this host doesn't keep reporting a generic
    // "Synthetic Test Adapter" identity once a specific real adapter is
    // actually driving it.
    bool systemIdentityExplicit = false;
    bool adapterIdentityExplicit = false;

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
        } else if (arg == "--audio-port") {
            config.audioPort = static_cast<uint16_t>(std::stoi(nextArg()));
        } else if (arg == "--no-mic") {
            config.micSupported = false;
        } else if (arg == "--app-version") {
            config.appVersion = nextArg();
        } else if (arg == "--self-update") {
            config.selfUpdateCommand = nextArg();
        } else if (arg == "--system-id") {
            config.systemIdentity.systemId = nextArg();
            systemIdentityExplicit = true;
        } else if (arg == "--system-name") {
            config.systemIdentity.systemName = nextArg();
            systemIdentityExplicit = true;
        } else if (arg == "--adapter-id") {
            config.adapterIdentity.adapterId = nextArg();
            adapterIdentityExplicit = true;
        } else if (arg == "--adapter-name") {
            config.adapterIdentity.adapterName = nextArg();
            adapterIdentityExplicit = true;
        } else if (arg == "--adapter-version") {
            config.adapterIdentity.adapterVersion = nextArg();
            adapterIdentityExplicit = true;
        } else if (arg == "--adapter-ipc") {
            useAdapterIpc = true;
        } else if (arg == "--adapter-socket") {
            adapterSocketPath = nextArg();
            useAdapterIpc = true;
        } else if (arg == "--help") {
            std::printf(
                "Usage: dualdeck-host-service [--bind ADDR] [--control-port N] "
                "[--input-port N] [--video-port N] [--timeout-ms N] [--auth-token TOKEN] "
                "[--state-dir PATH] [--pending-request-ttl-s N] [--stats-interval-ms N] "
                "[--discovery-port N] [--host-name NAME] [--no-discovery] "
                "[--audio-port N] [--no-mic] [--app-version STRING] [--self-update COMMAND] "
                "[--system-id ID] [--system-name NAME] [--adapter-id ID] "
                "[--adapter-name NAME] [--adapter-version STRING] "
                "[--adapter-ipc] [--adapter-socket PATH]\n"
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
                "The same request also pops a kdialog Yes/No prompt on this host's own\n"
                "desktop if kdialog is available, so approving/denying works with a\n"
                "mouse/controller click even with no visible console (e.g. Steam Gaming\n"
                "Mode) -- see kdialog_approval_prompt.h.\n"
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
                "testing instead.\n"
                "\n"
                "--app-version sets this host's release version string, sent to every\n"
                "client in HelloAck. If given and a connecting client reports a\n"
                "different non-empty version of its own, the handshake is rejected\n"
                "with AppVersionMismatch before authentication is even checked.\n"
                "Omitted by default (no version-mismatch check at all) -- the\n"
                "packaged release wires this from the archive's VERSION file.\n"
                "\n"
                "--self-update COMMAND runs COMMAND (detached, fire-and-forget) the\n"
                "moment a client whose device identity is ALREADY approved (never an\n"
                "unapproved one) hits an AppVersionMismatch rejection -- the client is\n"
                "told AppVersionMismatchUpdateTriggered instead, so it can show 'host is\n"
                "updating' rather than 'update one side to match the other'. Omitted by\n"
                "default (no self-update trigger at all). Only meaningful for a\n"
                "standalone process that can cleanly restart itself afterward -- the\n"
                "packaged persistent Host Control daemon wires this to its own\n"
                "internal/apply-update.sh, which already restarts the daemon if it was\n"
                "active; never set this for melonDS's in-process integration, which has\n"
                "no way to \"restart itself\" without losing the current game.\n"
                "\n"
                "Microphone support (spec/GitHub issue #2) is on by default: the\n"
                "client can capture its own microphone and stream it here over UDP\n"
                "on --audio-port (default 8765). This build only logs received\n"
                "audio's level -- it has no real destination to inject it into\n"
                "(see docs/melonds-integration-analysis.md); the melonDS integration\n"
                "feeds it into melonDS's own microphone input. --no-mic turns the\n"
                "feature off entirely (reported to the client as unsupported).\n"
                "\n"
                "GitHub issue #28 (decoupling DualDeck from melonDS): this host\n"
                "advertises which emulated system and which adapter/emulator is\n"
                "running it, in both LAN discovery and the Hello handshake, so client\n"
                "UI never has to guess. Defaults to a clearly-labeled synthetic/test\n"
                "identity ('synthetic'/'Synthetic Test System',\n"
                "'synthetic-test'/'Synthetic Test Adapter') since this binary never\n"
                "drives a real emulator -- override with --system-id/--system-name/\n"
                "--adapter-id/--adapter-name/--adapter-version, e.g. to stand this\n"
                "binary in as a fake fixture for a future 3DS/Wii U adapter's client\n"
                "UI tests without needing that real adapter built yet. The real\n"
                "melonDS integration always reports 'nds'/'Nintendo DS' and\n"
                "'melonds'/'melonDS' instead of these flags (hardcoded in\n"
                "RemoteServerBridge's constructor, see docs/architecture.md's\n"
                "'Emulator identity model' section) -- these only apply to this\n"
                "standalone binary.\n"
                "\n"
                "GitHub issue #28 Phase 2: --adapter-ipc switches this process from\n"
                "the built-in synthetic test-pattern stub to being driven by a real\n"
                "adapter connected over a local Unix-domain-socket IPC channel (see\n"
                "docs/adr/0001-host-service-and-adapter-architecture.md) -- e.g. the\n"
                "dualdeck-synthetic-adapter binary, or eventually a real emulator\n"
                "adapter. --adapter-socket PATH overrides the socket location\n"
                "(implies --adapter-ipc); default is $XDG_RUNTIME_DIR/dualdeck/adapter.sock.\n"
                "Microphone audio is not bridged through this path yet -- it is still\n"
                "only ever logged, same as the default mode, regardless of --no-mic.\n"
                "The client<->host wire protocol (kProtocolVersion) does not change\n"
                "in this mode; only where this process gets its frames/sends its\n"
                "input from/to internally changes.\n"
                "\n"
                "GitHub issue #4 Phase D: in --adapter-ipc mode this host no longer\n"
                "waits for an adapter before a client can connect -- it starts in\n"
                "host-control mode (a virtual gamepad, via Linux's uinput, for\n"
                "navigating the host's own UI) and automatically switches to emulation\n"
                "mode once an adapter connects, switching back on disconnect. Type\n"
                "'hostcontrol' at this console to force host-control mode even while an\n"
                "adapter is connected, 'resume' to clear that override, or 'mode' to\n"
                "check the current mode.\n");
            return 0;
        } else {
            std::fprintf(stderr, "unrecognized argument: %s\n", arg.c_str());
            return 1;
        }
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    // Device-approval mode (the default, when --auth-token is omitted) has
    // no console in Steam Gaming Mode to type approve/deny into, so this
    // hook pops a kdialog Yes/No prompt on the host's own desktop instead --
    // giving Azahar/host-control the same zero-typing approval flow
    // melonDS's in-process Qt integration already has.
    KdialogApprovalHook approvalHook;
    if (config.authToken.empty()) {
        config.onPendingRequestsChanged =
            [&approvalHook](std::vector<melonds_remote::host::DeviceApprovalManager::PendingRequest> pending) {
                approvalHook.onPendingRequestsChanged(std::move(pending));
            };
    }

    melonds_remote::host::LoggingMicAudioSink micSink; // unchanged either way -- see --help's note above

    if (useAdapterIpc) {
        melonds_remote::adapter::ipc::AdapterIpcServer adapterServer(adapterSocketPath);
        if (!adapterServer.start()) {
            std::fprintf(stderr, "failed to start adapter IPC server -- see stderr above for why\n");
            return 1;
        }

        // GitHub issue #4 Phase D: no longer blocks here waiting for an
        // adapter to connect before a client can do anything at all --
        // NetServer starts immediately in host-control mode
        // (HostControlAdapter, a virtual gamepad for navigating the
        // host's own UI) and ModeCoordinator swaps to emulation mode
        // automatically once an adapter connects over the IPC channel,
        // swapping back on disconnect. See docs/adr/
        // 0001-host-service-and-adapter-architecture.md sections 6-8.
        melonds_remote::host::HostControlAdapter hostControlAdapter;
        if (!hostControlAdapter.isDeviceReady()) {
            std::fprintf(stderr,
                          "warning: host-control mode's virtual gamepad is unavailable (see the "
                          "HostControlAdapter message above) -- a client can still connect, but won't "
                          "be able to navigate the host while no emulator is running.\n");
        }
        melonds_remote::host::AdapterBridge bridge(adapterServer);

        // Lets an out-of-process adapter's own frontend (e.g. Azahar's Qt
        // window) mirror melonDS's in-process "show top screen only while
        // a client is streaming" behavior, even though that adapter's own
        // process has no other way to learn a client connected -- see
        // ipc_protocol.h's ClientConnectionChanged comment. Set before
        // NetServer is constructed below since NetServerConfig is copied
        // in, not referenced.
        config.onClientConnectionChanged = [&adapterServer](bool connected) {
            adapterServer.notifyClientConnectionChanged(connected);
        };

        // config's own systemIdentity/adapterIdentity is irrelevant here
        // beyond providing the --system-id/--adapter-id fallback values
        // below -- ModeCoordinator::start() immediately overwrites
        // NetServer's actual reported identity via the same setTarget()
        // call every later mode transition uses, before any client could
        // possibly connect.
        melonds_remote::host::NetServer server(config, hostControlAdapter, hostControlAdapter, micSink);
        approvalHook.setServer(&server);
        melonds_remote::host::ModeCoordinator coordinator(
            server, adapterServer, bridge, bridge, hostControlAdapter, hostControlAdapter,
            systemIdentityExplicit, adapterIdentityExplicit, config.systemIdentity, config.adapterIdentity);
        coordinator.start();

        std::printf(
            "starting in host-control mode -- waiting for an adapter to connect over the local "
            "IPC channel (e.g. dualdeck-synthetic-adapter) to switch to emulation mode "
            "automatically. Type 'hostcontrol' to force host-control mode even once one "
            "connects, 'resume' to clear that override, or 'mode' to check the current mode.\n");
        runUntilStopped(server, config, &coordinator);
        coordinator.stop();
        adapterServer.stop();
    } else {
        melonds_remote::host::LoggingInputSink inputSink;
        melonds_remote::host::SyntheticFrameSource frameSource(60);
        frameSource.start();

        melonds_remote::host::NetServer server(config, inputSink, frameSource, micSink);
        approvalHook.setServer(&server);
        runUntilStopped(server, config);
        frameSource.stop();
    }

    return 0;
}
