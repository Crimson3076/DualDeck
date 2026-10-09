#pragma once

// Local control API for dualdeck-host-service: a Unix stream socket that
// a front end on the same machine (the planned Decky plugin, a script)
// uses to read status and answer device-approval requests, instead of
// typing into the service's stdin or relying on its kdialog popup.
//
// Protocol: one command per line, one JSON object per line in reply.
//
//   status            {"ok":true,"mode":"emulation","overridden":false,
//                      "system":{...},"adapter":{...},"pending":[...]}
//   pending           {"ok":true,"pending":[{"id":..,"name":..,"address":..}]}
//   approve <id>      {"ok":true} or {"ok":false,"error":"..."}
//   deny <id>         same
//   hostcontrol       force Host Control mode (only with --adapter-ipc)
//   resume            clear that override (only with --adapter-ipc)
//
// <id> is a device id or an unambiguous prefix of one, the same as the
// stdin console's approve/deny. The socket is created mode 0600 inside a
// 0700 directory, so only the user running the service can connect.

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace melonds_remote::host {

class NetServer;
class ModeCoordinator;

// $XDG_RUNTIME_DIR/dualdeck/host-control.sock, falling back to
// $HOME/.cache/dualdeck/host-control.sock, or empty if neither is set.
// Same directory as the adapter IPC socket.
std::string defaultControlSocketPath();

// Runs one command line against `server` (and `coordinator`, when the
// service runs with --adapter-ipc; may be null) and returns the JSON
// reply, without a trailing newline.
std::string handleControlCommand(const std::string& line, NetServer& server, ModeCoordinator* coordinator);

class ControlSocketServer {
public:
    using Handler = std::function<std::string(const std::string& line)>;

    ControlSocketServer(std::string socketPath, Handler handler);
    ~ControlSocketServer();

    ControlSocketServer(const ControlSocketServer&) = delete;
    ControlSocketServer& operator=(const ControlSocketServer&) = delete;

    // Creates the socket (replacing a stale one left at the same path)
    // and starts serving. Returns false, after logging why, if the
    // socket can't be created.
    bool start();
    void stop();

    const std::string& socketPath() const { return socketPath_; }

private:
    void acceptLoop();
    void serveClient(int clientFd);

    std::string socketPath_;
    Handler handler_;
    int listenFd_ = -1;
    std::atomic<bool> running_{false};
    std::thread acceptThread_;
};

} // namespace melonds_remote::host
