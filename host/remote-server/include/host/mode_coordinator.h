#pragma once

// GitHub issue #4 Phase D: watches whether an adapter is connected over
// the local IPC channel and swaps NetServer's active target accordingly
// -- HostMode::Emulation (a real adapter, e.g. melonDS, driving the
// session) whenever one is connected, HostMode::HostControl (a virtual
// gamepad for navigating the host's own UI -- see host_control_adapter.h,
// GitHub issue #4 Phase C) otherwise. This is what lets a client stay
// connected across an emulator starting and exiting, per
// docs/adr/0001-host-service-and-adapter-architecture.md sections 6-8.

#include <atomic>
#include <thread>

#include "host/host_session_state.h"
#include "host/net_server.h"
#include "melonds_remote/adapter/ipc/adapter_ipc_server.h"

namespace melonds_remote::host {

// Fixed identity reported while HostMode::HostControl is active -- no
// emulator is running, so there's no real SystemIdentity/AdapterIdentity
// to report; these are the fixed placeholders a client UI can key off of
// (GitHub issue #4 Phase E, not yet built).
inline const SystemIdentity kHostControlSystemIdentity{"host", "Host Menu"};
inline const AdapterIdentity kHostControlAdapterIdentity{"host-control", "Host Control", ""};

// Pure decision, no I/O -- unit-tested directly (test_mode_coordinator.cpp)
// without needing a real AdapterIpcServer/NetServer at all. Manual
// override always wins, even while an adapter is connected: that is the
// entire point of a "manual override" (an operator stepping back out to
// host navigation without having to disconnect the emulator adapter
// first).
HostMode computeDesiredMode(bool adapterConnected, bool manualHostControlOverride);

// Same pure-decision style as computeDesiredMode() above, but for the
// richer HostSessionState (host_session_state.h) rather than the wire's
// 2-value HostMode -- see that header for the full state list and why
// most of it has no real signal source yet. `adapterState` is only
// meaningful when `adapterConnected` is true: AdapterIpcServer
// deliberately leaves its last-known SessionState in place across a
// disconnect (see adapter_ipc_server.cpp's resetSessionLocked() comment)
// rather than resetting it, so a stale Running/Starting/etc. value can
// still be sitting there after the adapter that reported it is long
// gone -- computeDesiredHostSessionState() ignores adapterState entirely
// once adapterConnected is false, rather than trusting that stale value.
HostSessionState computeDesiredHostSessionState(bool adapterConnected,
                                                 melonds_remote::adapter::SessionState adapterState,
                                                 bool manualHostControlOverride);

class ModeCoordinator {
public:
    // `emulationInputSink`/`emulationFrameSource` and
    // `hostControlInputSink`/`hostControlFrameSource` are two already-
    // constructed IEmulatorInputSink/IFrameSource pairs this class picks
    // between via `server.setTarget()` -- ownership stays with the
    // caller (main.cpp), matching how NetServer itself never owns its
    // own targets either. `systemIdentityExplicit`/`adapterIdentityExplicit`
    // and the fallback identities mirror main.cpp's existing
    // --system-id/--adapter-id CLI-override logic: when explicit, they
    // win over whatever a connected adapter reports for itself.
    ModeCoordinator(NetServer& server, melonds_remote::adapter::ipc::AdapterIpcServer& adapterServer,
                     IEmulatorInputSink& emulationInputSink, IFrameSource& emulationFrameSource,
                     IEmulatorInputSink& hostControlInputSink, IFrameSource& hostControlFrameSource,
                     bool systemIdentityExplicit, bool adapterIdentityExplicit,
                     SystemIdentity fallbackSystemIdentity, AdapterIdentity fallbackAdapterIdentity);
    ~ModeCoordinator();

    ModeCoordinator(const ModeCoordinator&) = delete;
    ModeCoordinator& operator=(const ModeCoordinator&) = delete;

    // Immediately applies the correct initial mode (HostControl -- an
    // adapter can't possibly be connected yet the instant this runs)
    // and starts the background polling thread. Idempotent: a second
    // call is a no-op.
    void start();
    void stop();

    // Manual override (GitHub issue #4 Phase D): force HostMode::HostControl
    // regardless of adapter connection state, or clear that override and
    // let auto-detection resume immediately. Safe from any thread (e.g.
    // a console command handler running on its own thread).
    void forceHostControl();
    void clearOverride();

    bool isOverridden() const { return manualOverride_.load(); }

    // Derived live from adapterServer_.hasConnectedAdapter()/
    // currentState() and the current override, via
    // computeDesiredHostSessionState() above -- not cached, so this
    // always reflects the live signals rather than whatever HostMode was
    // last actually applied to NetServer (those two can disagree for up
    // to one poll interval, ~100ms, after a real transition). Not yet
    // wired to anything client-visible (see host_session_state.h);
    // exposed today for logging/diagnostics and so this class's own
    // adapter-state-awareness is independently testable.
    HostSessionState currentSessionState() const;

private:
    void pollLoop();
    void applyMode(HostMode mode);

    NetServer& server_;
    melonds_remote::adapter::ipc::AdapterIpcServer& adapterServer_;
    IEmulatorInputSink& emulationInputSink_;
    IFrameSource& emulationFrameSource_;
    IEmulatorInputSink& hostControlInputSink_;
    IFrameSource& hostControlFrameSource_;
    bool systemIdentityExplicit_;
    bool adapterIdentityExplicit_;
    SystemIdentity fallbackSystemIdentity_;
    AdapterIdentity fallbackAdapterIdentity_;

    std::atomic<bool> running_{false};
    std::atomic<bool> manualOverride_{false};
    std::thread pollThread_;
};

} // namespace melonds_remote::host
