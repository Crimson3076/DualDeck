#pragma once

// GitHub issue #4 Phase D: watches whether an adapter is connected over
// the local IPC channel and swaps NetServer's active target accordingly
// -- HostMode::Emulation (a real adapter, e.g. melonDS, driving the
// session) whenever one is connected, HostMode::HostControl (no adapter
// connected -- see no_adapter_target.h) otherwise. This is what lets a
// client stay connected across an emulator starting and exiting, per
// docs/adr/0001-host-service-and-adapter-architecture.md sections 6-8.
//
// HostMode::HostControl used to mean "navigate the host's own desktop via
// a virtual gamepad" (GitHub issue #4 Phase C's HostControlAdapter,
// removed -- see no_adapter_target.h's comment and
// docs/known-limitations.md). The manual "hostcontrol"/"resume" console
// override that used to force that mode on demand was removed along with
// it -- forcing "no adapter connected" manually while a real adapter *is*
// connected has no use once there's nothing to navigate to.

#include <atomic>
#include <thread>

#include "host/net_server.h"
#include "melonds_remote/adapter/ipc/adapter_ipc_server.h"

namespace melonds_remote::host {

// Fixed identity reported while HostMode::HostControl is active -- no
// emulator is running, so there's no real SystemIdentity/AdapterIdentity
// to report; these are the fixed placeholders a client UI keys off of to
// show its "waiting for emulator" screen (see main.cpp's
// renderHostControlScreen()).
inline const SystemIdentity kNoAdapterSystemIdentity{"none", ""};
inline const AdapterIdentity kNoAdapterAdapterIdentity{"none", "Waiting for emulator", ""};

// Pure decision, no I/O -- unit-tested directly (test_mode_coordinator.cpp)
// without needing a real AdapterIpcServer/NetServer at all.
HostMode computeDesiredMode(bool adapterConnected);

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
    std::thread pollThread_;
};

} // namespace melonds_remote::host
