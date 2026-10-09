#include "host/mode_coordinator.h"

#include <chrono>
#include <cstdio>

namespace dualdeck::host {

HostMode computeDesiredMode(bool adapterConnected, bool manualHostControlOverride) {
    if (manualHostControlOverride) return HostMode::HostControl;
    return adapterConnected ? HostMode::Emulation : HostMode::HostControl;
}

HostSessionState computeDesiredHostSessionState(bool adapterConnected,
                                                 dualdeck::adapter::SessionState adapterState,
                                                 bool manualHostControlOverride) {
    // Matches computeDesiredMode()'s own override-always-wins rule.
    // CompanionModeActive (not Connected) since a forced override with
    // no adapter connected is exactly "operator stepped into host
    // navigation" -- the same real-world situation Connected -> the
    // no-adapter-yet case also describes, but distinguishable here
    // because the override is a deliberate act, not just "nothing has
    // connected yet."
    if (manualHostControlOverride) return HostSessionState::CompanionModeActive;
    if (!adapterConnected) return HostSessionState::Connected;

    switch (adapterState) {
        case dualdeck::adapter::SessionState::Available: return HostSessionState::Connected;
        case dualdeck::adapter::SessionState::Starting:  return HostSessionState::Launching;
        case dualdeck::adapter::SessionState::Running:   return HostSessionState::EmulatorRunning;
        // No separate wire concept for "emulating but paused" yet --
        // still EmulatorRunning from the host-session perspective; the
        // adapter-level distinction stays visible via
        // AdapterIpcServer::currentState() directly for anything that
        // needs it.
        case dualdeck::adapter::SessionState::Paused:    return HostSessionState::EmulatorRunning;
        case dualdeck::adapter::SessionState::Stopped:   return HostSessionState::Connected;
        case dualdeck::adapter::SessionState::Error:     return HostSessionState::Error;
    }
    return HostSessionState::Error;
}

ModeCoordinator::ModeCoordinator(NetServer& server, dualdeck::adapter::ipc::AdapterIpcServer& adapterServer,
                                  IEmulatorInputSink& emulationInputSink, IFrameSource& emulationFrameSource,
                                  IEmulatorInputSink& hostControlInputSink, IFrameSource& hostControlFrameSource,
                                  bool systemIdentityExplicit, bool adapterIdentityExplicit,
                                  SystemIdentity fallbackSystemIdentity, AdapterIdentity fallbackAdapterIdentity)
    : server_(server), adapterServer_(adapterServer), emulationInputSink_(emulationInputSink),
      emulationFrameSource_(emulationFrameSource), hostControlInputSink_(hostControlInputSink),
      hostControlFrameSource_(hostControlFrameSource), systemIdentityExplicit_(systemIdentityExplicit),
      adapterIdentityExplicit_(adapterIdentityExplicit), fallbackSystemIdentity_(std::move(fallbackSystemIdentity)),
      fallbackAdapterIdentity_(std::move(fallbackAdapterIdentity)) {}

ModeCoordinator::~ModeCoordinator() {
    stop();
}

void ModeCoordinator::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }

    // Correctly seeds NetServer's currentMode()/identity bookkeeping
    // before any client could possibly connect -- NetServer's own
    // constructor has no notion of "initial mode," only an initial
    // target, so this first setTarget() call (reusing the exact same,
    // already-tested code path pollLoop() uses for every later
    // transition) is what actually establishes HostMode::HostControl as
    // current rather than NetServer's own construction-time default of
    // HostMode::Emulation.
    applyMode(HostMode::HostControl);

    pollThread_ = std::thread(&ModeCoordinator::pollLoop, this);
}

void ModeCoordinator::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (pollThread_.joinable()) pollThread_.join();
}

void ModeCoordinator::forceHostControl() {
    manualOverride_ = true;
}

void ModeCoordinator::clearOverride() {
    manualOverride_ = false;
}

void ModeCoordinator::pollLoop() {
    // 100ms: fast enough that a mode swap (an adapter connecting/
    // disconnecting, or a manual override command) reaches an already-
    // connected client well within the reaction time this project's
    // other UI feedback already targets, without polling so often it
    // shows up as meaningful CPU use for what is otherwise an idle
    // background thread.
    while (running_.load()) {
        HostMode desired = computeDesiredMode(adapterServer_.hasConnectedAdapter(), manualOverride_.load());
        if (desired != server_.currentMode()) {
            applyMode(desired);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

HostSessionState ModeCoordinator::currentSessionState() const {
    return computeDesiredHostSessionState(adapterServer_.hasConnectedAdapter(), adapterServer_.currentState(),
                                           manualOverride_.load());
}

void ModeCoordinator::applyMode(HostMode mode) {
    if (mode == HostMode::Emulation) {
        SystemIdentity system =
            systemIdentityExplicit_ ? fallbackSystemIdentity_ : adapterServer_.capabilities().system;
        AdapterIdentity adapter =
            adapterIdentityExplicit_ ? fallbackAdapterIdentity_ : adapterServer_.capabilities().adapter;
        // No prior visibility at all into whether/when this switch ever
        // happens -- a diagnostic gap found while tracing a real report
        // of "video never sends" all the way from AzaharAdapter's
        // capture through to NetServer's stats: without this, "the
        // adapter connected but mode never left HostControl" and "mode
        // switched fine but capture itself never produces a frame" both
        // look identical (silence) from the host's own terminal.
        std::fprintf(stderr, "ModeCoordinator: switching to Emulation mode (system=%s, adapter=%s)\n",
                      system.systemName.c_str(), adapter.adapterName.c_str());
        server_.setTarget(emulationInputSink_, emulationFrameSource_, HostMode::Emulation, std::move(system),
                           std::move(adapter));
    } else {
        std::fprintf(stderr, "ModeCoordinator: switching to HostControl mode\n");
        server_.setTarget(hostControlInputSink_, hostControlFrameSource_, HostMode::HostControl,
                           kHostControlSystemIdentity, kHostControlAdapterIdentity);
    }
}

} // namespace dualdeck::host
