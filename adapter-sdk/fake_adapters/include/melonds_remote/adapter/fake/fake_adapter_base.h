#pragma once

// Shared plumbing for the fake DS/3DS/Wii U capability fixtures in this
// directory (GitHub issue #28: "Add generic fake adapters for DS, 3DS,
// and Wii U capability fixtures" -- Phase 1 of the issue). These are
// test fixtures proving the contract in adapter_contract.h can describe
// each system's shape, not real emulator integrations -- there is no
// actual DS/3DS/Wii U emulation anywhere in this directory.
//
// Not part of the contract itself (IEmulatorAdapter has no dependency
// on this class) -- just avoids duplicating the same per-surface
// latest-frame-wins storage and lifecycle/input bookkeeping three
// times across FakeDsAdapter/FakeThreeDsAdapter/FakeWiiUAdapter.

#include <mutex>
#include <string>
#include <unordered_map>

#include "melonds_remote/adapter/adapter_contract.h"

namespace melonds_remote::adapter::fake {

class FakeAdapterBase : public IEmulatorAdapter {
public:
    explicit FakeAdapterBase(AdapterCapabilities capabilities);

    // IEmulatorAdapter
    AdapterCapabilities capabilities() const override;
    SessionState currentState() const override;
    void applyGenericInput(const GenericInputState& state) override;
    void releaseAllInputs() override;
    bool latestFrame(const std::string& surfaceId, SurfaceFrame& outFrame) override;

    // Test-only helpers below, not part of IEmulatorAdapter -- a real
    // adapter wouldn't expose these (frames arrive from the actual
    // emulator, lifecycle changes from the actual emulation process),
    // but the fixtures need some way to be driven from a test.

    // Pushes a new frame for `surfaceId`, incrementing that surface's
    // own frameIndex. Silently accepted even for an undeclared surface
    // ID (mirrors production code choosing not to crash on an
    // unexpected-but-harmless value -- see the ADR's "malformed input
    // must not crash the Host Service" note). width/height default to
    // 0x0 (an adapter that hasn't set SurfaceFrame's own real size --
    // see adapter_contract.h's contract v2 comment) since most tests
    // don't care about it; pass real values to test the width/height
    // path itself (e.g. AdapterBridge's own real-size-over-declared-
    // default fix).
    void pushFrame(const std::string& surfaceId, std::vector<uint8_t> pixels,
                   uint16_t width = 0, uint16_t height = 0);

    // Attempts the transition via isValidTransition(); returns whether
    // it was applied. An invalid transition leaves state() unchanged
    // rather than throwing/asserting -- a fixture proving the contract
    // is safe to drive with an unexpected sequence, not just the happy
    // path.
    bool setState(SessionState state);

    GenericInputState lastAppliedInput() const;

    // True from construction until the first applyGenericInput() call,
    // and again after every releaseAllInputs() call -- lets a test
    // assert "this adapter is currently holding no input" without
    // inspecting GenericInputState's fields by hand.
    bool isReleased() const;

private:
    AdapterCapabilities capabilities_;

    mutable std::mutex mutex_;
    SessionState state_ = SessionState::Available;
    GenericInputState lastInput_;
    bool released_ = true;
    std::unordered_map<std::string, SurfaceFrame> latestFrames_;
    std::unordered_map<std::string, uint64_t> nextFrameIndex_;
};

} // namespace melonds_remote::adapter::fake
