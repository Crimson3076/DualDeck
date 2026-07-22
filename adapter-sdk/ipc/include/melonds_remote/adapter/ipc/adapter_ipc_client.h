#pragma once

// The adapter-process side of the local adapter IPC channel (GitHub
// issue #28 Phase 2 / the ADR's IPC decision). Wraps any local
// IEmulatorAdapter implementation and exposes it to a Host Service
// (AdapterIpcServer) running elsewhere on the same machine, over a Unix
// domain socket -- this is what turns an in-process fixture like
// FakeDsAdapter, or a real emulator adapter, into something that can
// run as its own out-of-process program.
//
// AdapterIpcClient does not implement IEmulatorAdapter itself -- it
// drives one, in the other direction from AdapterIpcServer (which
// receives IEmulatorAdapter calls and forwards them out; this class
// receives IPC messages and forwards them into a local
// IEmulatorAdapter).

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>

#include "melonds_remote/adapter/adapter_contract.h"

namespace melonds_remote::adapter::ipc {

class AdapterIpcClient {
public:
    // `localAdapter` must outlive this object. `socketPath` defaults to
    // defaultAdapterSocketPath() when empty.
    explicit AdapterIpcClient(IEmulatorAdapter& localAdapter, std::string socketPath = {});
    ~AdapterIpcClient();

    AdapterIpcClient(const AdapterIpcClient&) = delete;
    AdapterIpcClient& operator=(const AdapterIpcClient&) = delete;

    // Connects, sends Hello with localAdapter's capabilities, and waits
    // for HelloAck. Returns false on any failure (socket error,
    // rejected handshake) -- does not retry; callers wanting
    // auto-reconnect should call this again themselves (mirroring
    // NetClient::connect()'s same non-retrying contract, and
    // client/src/main.cpp's own reconnect-loop-on-a-background-thread
    // pattern built on top of it).
    bool connect();
    void disconnect();
    bool isConnected() const { return connected_.load(); }

    // Optional -- if set, invoked from readLoop()'s own thread (never
    // the caller's) whenever a ClientConnectionChanged message arrives.
    // Set this before connect() so an early message right after the
    // handshake isn't missed. No-op (message is just dropped) if unset,
    // same as any other message type this adapter doesn't care about.
    void setConnectionStateCallback(std::function<void(bool)> callback);

private:
    void readLoop();  // blocking recv: InputState/ReleaseInputs/ClientConnectionChanged/Heartbeat/Disconnect -> localAdapter
    void writeLoop(); // periodic: localAdapter's frames/state -> Frame/StateChanged/Heartbeat messages

    IEmulatorAdapter& localAdapter_;
    std::string socketPath_;

    std::atomic<int> fd_{-1};
    std::atomic<bool> connected_{false};
    std::thread readThread_;
    std::thread writeThread_;

    // Tracks the last frameIndex sent per surface, so writeLoop() only
    // sends a Frame message when localAdapter_ actually produced a new
    // one -- avoids re-sending an unchanged frame every poll tick.
    std::unordered_map<std::string, uint64_t> lastSentFrameIndex_;
    SessionState lastSentState_ = SessionState::Available;

    std::function<void(bool)> connectionStateCallback_;
};

} // namespace melonds_remote::adapter::ipc
