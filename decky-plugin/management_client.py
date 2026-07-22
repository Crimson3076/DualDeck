"""Thin client for DualDeck's management control channel.

Matches the wire protocol implemented by ManagementServer in
host/melonds-patches/0001-remote-server-integration.patch
(src/frontend/qt_sdl/remote_server/ManagementServer.h/.cpp): one line in,
one line out, per connection --

    "<token> ENABLE\n" | "<token> DISABLE\n" | "<token> STATUS\n"
    -> "OK ENABLED\n" | "OK DISABLED\n" | "ERROR <reason>\n"

Deliberately a plain module with no Decky-specific imports, so it can be
exercised on its own (see the bottom of this file, or run it directly
against a real patched melonDS with a management token configured) --
main.py just calls into this.
"""
import socket


class ManagementError(Exception):
    """Raised when the host rejects the command (bad token, or the
    remote server itself failed to start/stop for some reason)."""


def _send_command(host: str, port: int, token: str, command: str, timeout: float = 5.0) -> str:
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.sendall(f"{token} {command}\n".encode("utf-8"))
        sock.settimeout(timeout)
        data = b""
        while not data.endswith(b"\n"):
            chunk = sock.recv(256)
            if not chunk:
                break
            data += chunk
    response = data.decode("utf-8", errors="replace").strip()
    if response.startswith("ERROR"):
        reason = response[len("ERROR "):] if len(response) > len("ERROR ") else response
        raise ManagementError(reason)
    return response


def enable(host: str, port: int, token: str) -> bool:
    """Starts remote streaming (idempotent). Returns True on success."""
    return _send_command(host, port, token, "ENABLE") == "OK ENABLED"


def disable(host: str, port: int, token: str) -> bool:
    """Stops remote streaming (idempotent). Returns True on success."""
    return _send_command(host, port, token, "DISABLE") == "OK DISABLED"


def status(host: str, port: int, token: str) -> bool:
    """Returns True if remote streaming is currently running."""
    return _send_command(host, port, token, "STATUS") == "OK ENABLED"


if __name__ == "__main__":
    # Manual smoke test against a real running host, e.g.:
    #   python3 management_client.py 192.168.1.50 8764 my-management-token status
    import sys

    _host, _port, _token, _cmd = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    _fn = {"enable": enable, "disable": disable, "status": status}[_cmd.lower()]
    print(_fn(_host, _port, _token))
