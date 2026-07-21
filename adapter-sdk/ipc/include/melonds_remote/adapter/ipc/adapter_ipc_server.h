#pragma once

// The Host Service side of the local adapter IPC channel (GitHub issue
// #28 Phase 2 / the ADR's IPC decision). Listens on a Unix domain
// socket and, once an adapter process connects and completes the Hello
// handshake, proxies IEmulatorAdapter calls to it over that socket --
// callers of AdapterIpcServer cannot tell a real out-of-process adapter
// apart from an in-process one, by design (the whole point of both
// implementing the same interface).
//
// One adapter connection at a time, exactly like NetServer's existing
// single-control-connection policy elsewhere in this codebase: a
// second simultaneous connection attempt is rejected outright.
// "Replacement" (GitHub issue #28's required test coverage) falls out
// of this naturally rather than needing separate logic -- once a
// connected adapter disconnects (graceful Disconnect message, EOF, or
// a read error), the accept loop is free to accept a new one, which
// then simply becomes the current session.
//
// Not wired into host/remote-server's NetServer yet -- this is new,
// additive infrastructure proven by the tests in adapter-sdk/tests/,
// not yet the production path a real client's traffic flows through.
// See docs/adr/0001-host-service-and-adapter-architecture.md.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "melonds_remote/adapter/adapter_contract.h"

namespace melonds_remote::adapter::ipc {

class AdapterIpcServer : public IEmulatorAdapter {
public:
    // `socketPath` defaults to defaultAdapterSocketPath() (see
    // socket_path.h) when empty.
    explicit AdapterIpcServer(std::string socketPath = {});
    ~AdapterIpcServer() override;

    AdapterIpcServer(const AdapterIpcServer&) = delete;
    AdapterIpcServer& operator=(const AdapterIpcServer&) = delete;

    // Binds the socket and starts the accept loop on its own thread.
    // Returns false if the socket couldn't be created/bound (e.g. the
    // parent directory couldn't be secured to mode 0700 -- see
    // socket_path.h's ensureSocketDirectory()).
    bool start();
    void stop();

    // True once an adapter has completed the Hello handshake and is
    // currently connected (not yet disconnected/timed out).
    bool hasConnectedAdapter() const;

    // IEmulatorAdapter -- proxied to whatever adapter is currently
    // connected, or a safe no-op/empty-default answer when none is.
    AdapterCapabilities capabilities() const override;
    SessionState currentState() const override;
    void applyGenericInput(const GenericInputState& state) override;
    void releaseAllInputs() override;
    bool latestFrame(const std::string& surfaceId, SurfaceFrame& outFrame) override;

private:
    void acceptLoop();
    // Handles exactly one connection from Hello through disconnect,
    // then returns so acceptLoop() can accept the next one.
    void serveConnection(int clientFd);
    void resetSessionLocked(); // caller holds sessionMutex_

    std::string socketPath_;
    std::atomic<bool> running_{false};
    int listenFd_ = -1;
    std::thread acceptThread_;

    // Guards every field below -- both the accept/serve thread and any
    // caller thread invoking applyGenericInput()/releaseAllInputs()/
    // capabilities()/currentState()/latestFrame() touch these.
    mutable std::mutex sessionMutex_;
    std::atomic<int> clientFd_{-1}; // separate from sessionMutex_: read/write happen without holding it

    // Guards every send() on clientFd_ specifically. serveConnection()'s
    // heartbeat thread and any caller thread invoking
    // applyGenericInput()/releaseAllInputs() can both be writing to the
    // same fd concurrently (the receive loop only ever reads); without
    // this, two sendMessage() calls racing on the socket can interleave
    // their bytes mid-message, corrupting the framing the other end's
    // recvMessage() relies on -- silently garbling or dropping whatever
    // message lost the race, rather than crashing outright. Separate
    // from sessionMutex_ so a slow/blocking send() never holds up an
    // unrelated capabilities()/currentState() read.
    std::mutex writeMutex_;
    bool connected_ = false;
    AdapterCapabilities capabilities_;
    SessionState state_ = SessionState::Available;
    std::unordered_map<std::string, SurfaceFrame> latestFrames_;
};

} // namespace melonds_remote::adapter::ipc
