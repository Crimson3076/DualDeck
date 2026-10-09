"""Client for dualdeck-host-service's local control socket.

Matches host/remote-server/include/host/control_socket.h: one command per
line in, one JSON object per line out --

    "status\\n"        -> {"ok":true,"mode":"emulation"|"host-control",
                          "overridden":bool,"system":{...},"adapter":{...},
                          "pending":[{"id":..,"name":..,"address":..}]}
    "approve <id>\\n"  -> {"ok":true} | {"ok":false,"error":"..."}
    "deny <id>\\n"     -> same
    "hostcontrol\\n"   -> force Host Control mode
    "resume\\n"        -> clear that override

The socket only exists on the machine running the host service (the
dualdeck-host-control.service daemon, or run-host.sh), so this is what the
plugin uses when it runs on the host PC itself. Like management_client.py,
it has no Decky-specific imports so it can be exercised on its own.
"""
import json
import os
import socket
import stat

_SOCKET_NAME = os.path.join("dualdeck", "host-control.sock")


class HostControlError(Exception):
    """Raised when the host service answers {"ok": false, ...}."""


def socket_path_candidates(home=None, uid=None):
    """Where the host service puts its socket, most likely first: the
    service's own default ($XDG_RUNTIME_DIR, else ~/.cache), plus
    /run/user/<uid> for when this process's environment has no
    XDG_RUNTIME_DIR but the service's (a systemd user unit) did."""
    candidates = []
    runtime_dir = os.environ.get("XDG_RUNTIME_DIR")
    if runtime_dir:
        candidates.append(os.path.join(runtime_dir, _SOCKET_NAME))
    if uid is not None:
        candidates.append(os.path.join(f"/run/user/{uid}", _SOCKET_NAME))
    home = home or os.environ.get("HOME")
    if home:
        candidates.append(os.path.join(home, ".cache", _SOCKET_NAME))
    seen = set()
    return [p for p in candidates if not (p in seen or seen.add(p))]


def find_socket(candidates):
    """Returns the first candidate that exists as a socket, or None."""
    for path in candidates:
        try:
            if stat.S_ISSOCK(os.stat(path).st_mode):
                return path
        except OSError:
            continue
    return None


def send(socket_path, command, timeout=3.0):
    """Sends one command and returns its decoded JSON reply. Raises
    HostControlError when the service reports a failure, OSError when it
    can't be reached."""
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(timeout)
        sock.connect(socket_path)
        sock.sendall((command + "\n").encode("utf-8"))
        data = b""
        while not data.endswith(b"\n"):
            chunk = sock.recv(4096)
            if not chunk:
                break
            data += chunk
    reply = json.loads(data.decode("utf-8"))
    if not reply.get("ok"):
        raise HostControlError(reply.get("error", "unknown error"))
    return reply


def status(socket_path):
    return send(socket_path, "status")


def answer_device(socket_path, device_id, approve):
    send(socket_path, f"{'approve' if approve else 'deny'} {device_id}")


def set_host_control(socket_path, force):
    send(socket_path, "hostcontrol" if force else "resume")


if __name__ == "__main__":
    # Manual check against a running host service, e.g.:
    #   python3 host_control_client.py status
    #   python3 host_control_client.py approve 3f2a
    import sys

    path = find_socket(socket_path_candidates(uid=os.getuid()))
    if path is None:
        sys.exit("no host control socket found (is dualdeck-host-service running?)")
    print(json.dumps(send(path, " ".join(sys.argv[1:]) or "status"), indent=2))
