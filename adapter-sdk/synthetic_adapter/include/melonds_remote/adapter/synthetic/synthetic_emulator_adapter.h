#pragma once

// A real, self-contained IEmulatorAdapter that generates its own
// animated test-pattern frames on a timer thread -- no dependency on
// host/remote-server (adapter-sdk is meant to be a layer that
// host/remote-server will eventually depend on, not the other way
// around, so this intentionally does not reuse
// host::SyntheticFrameSource even though the two are similar in
// spirit).
//
// Distinct from adapter-sdk/fake_adapters/'s FakeDsAdapter etc., which
// are unit-test fixtures fed frames manually via pushFrame() and never
// run standalone: this class is what
// adapter-sdk/synthetic_adapter/src/main.cpp wraps in an
// AdapterIpcClient to become a real, independently-runnable process
// that a Host Service (AdapterIpcServer) can connect to over the local
// IPC channel -- the literal "make the synthetic adapter connect
// through the same contract real adapters will use" item from GitHub
// issue #28's Phase 2 checklist.

#include <atomic>
#include <mutex>
#include <thread>

#include "melonds_remote/adapter/adapter_contract.h"

namespace melonds_remote::adapter::synthetic {

class SyntheticEmulatorAdapter : public IEmulatorAdapter {
public:
    explicit SyntheticEmulatorAdapter(int targetFps = 60);
    ~SyntheticEmulatorAdapter() override;

    SyntheticEmulatorAdapter(const SyntheticEmulatorAdapter&) = delete;
    SyntheticEmulatorAdapter& operator=(const SyntheticEmulatorAdapter&) = delete;

    // Starts/stops the frame-generator thread. capabilities() is valid
    // to call even before start(); latestFrame() returns false until
    // the first frame has actually been generated.
    void start();
    void stop();

    // IEmulatorAdapter
    AdapterCapabilities capabilities() const override;
    SessionState currentState() const override;
    void applyGenericInput(const GenericInputState& state) override;
    void releaseAllInputs() override;
    bool latestFrame(const std::string& surfaceId, SurfaceFrame& outFrame) override;

private:
    void generatorLoop();
    void renderPattern(std::vector<uint8_t>& pixels, uint64_t frameIndex) const;

    int targetFps_;
    std::atomic<bool> running_{false};
    std::thread thread_;

    mutable std::mutex mutex_;
    SessionState state_ = SessionState::Running; // this fixture is always "running" once started
    GenericInputState lastInput_;
    SurfaceFrame latestFrame_;
    bool hasFrame_ = false;
};

} // namespace melonds_remote::adapter::synthetic
