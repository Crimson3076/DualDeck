#pragma once

// The IEmulatorInputSink/IFrameSource NetServer/ModeCoordinator point at
// while no emulator adapter is connected over the local IPC channel --
// see ModeCoordinator (host/remote-server/include/host/mode_coordinator.h)
// for what actually decides when to swap to/from this target.
//
// Replaces the former HostControlAdapter (GitHub issue #4 Phase C: a
// virtual Xbox-360-style gamepad, via Linux's uinput, letting a client
// navigate the host's own desktop UI while no emulator was running).
// Real-usage feedback: this never turned into something worth using --
// navigating a Big Picture/window-manager UI one-handed through a
// virtual gamepad while staring at a static placeholder screen wasn't a
// workflow anyone actually adopted, and it cost real maintenance surface
// (a uinput device, a DS-button-to-gamepad translation table, a whole
// class of "is /dev/uinput even available" degradation handling) for a
// feature nobody used. See docs/known-limitations.md's matching entry.
//
// ModeCoordinator's own actual job -- detecting when an adapter connects/
// disconnects and swapping NetServer's target so a client can stay
// connected across an emulator starting and exiting -- is unaffected by
// this removal; only WHAT it swaps to during that gap changed, from a
// virtual gamepad to simply nothing. Input received during this state is
// silently discarded (there is nothing to apply it to) and no video
// frame is ever available -- the client shows a plain "waiting for
// emulator" screen instead (see main.cpp's renderHostControlScreen()).

#include "host/emulator_input_sink.h"
#include "host/frame_source.h"

namespace melonds_remote::host {

class NoAdapterInputSink : public IEmulatorInputSink {
public:
    void applyControllerState(const ControllerState&) override {}
    void releaseAll() override {}
};

class NoAdapterFrameSource : public IFrameSource {
public:
    bool getLatestFrame(std::vector<uint8_t>&, uint64_t&, uint16_t&, uint16_t&) override { return false; }
};

} // namespace melonds_remote::host
