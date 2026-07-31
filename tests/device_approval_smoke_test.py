#!/usr/bin/env python3
"""Integration smoke test for device-approval authentication (spec
section 13, `host/remote-server/include/host/device_approval_manager.h`).
Replaces the earlier 6-digit-pairing-code flow, which required typing a
code on the client -- unworkable since Steam Input doesn't reliably bring
up a virtual keyboard in Gaming Mode (see docs/known-limitations.md).

Starts the real `dualdeck-host-service` binary with NO `--auth-token`
(device-approval mode, the default), then exercises the same state
machine a real client + host operator would hit:

  1. An unrecognized device identity is rejected with `ApprovalRequired`,
     and the server's log gets a pending-request line naming it.
  2. Sending 'approve <id>' on the server's stdin approves it; retrying
     the same identity is then accepted.
  3. Reconnecting with that identity again is accepted silently (no new
     prompt, no new state).
  4. A different, never-seen identity is still rejected (not accidentally
     approved by the first approval).
  5. Sending 'deny <id>' for that second identity is accepted by the
     console loop but leaves it rejected on the next attempt.
  6. Restarting the server with `--state-dir` pointed at the same
     directory remembers the first identity's approval across the
     restart.

This is a protocol-level test (raw sockets, not the actual SDL3 client) --
see `host/melonds-patches/README.md` for the corresponding real-client
verification account. Complements `tests/smoke_test.py`, which covers the
static `--auth-token` path instead.

Usage:
    python3 tests/device_approval_smoke_test.py /path/to/dualdeck-host-service
"""

import re
import secrets
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

MAGIC = 0x444D5231
VERSION = 10

PT_HELLO = 1
PT_HELLO_ACK = 2

REJECT_NONE = 0
REJECT_APPROVAL_REQUIRED = 4


def header(packet_type: int, payload_size: int) -> bytes:
    return struct.pack("<IHHI", MAGIC, VERSION, packet_type, payload_size)


def lp_string(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack("<H", len(b)) + b


def hello_payload(name: str, platform: str, width: int, height: int, device_id: str) -> bytes:
    # Trailing empty appVersion: this server is never started with
    # --app-version, so the AppVersionMismatch check is skipped regardless
    # of what's sent here -- see protocol.h's HelloPayload::appVersion.
    #
    # Final 0 byte: HelloPayload::videoQuality (protocol v9) -- 0 means
    # "defer to the host's own configured default," irrelevant to what
    # this file actually tests (device approval, not video).
    return (
        lp_string(name) + lp_string(platform) + struct.pack("<HH", width, height) + lp_string(device_id)
        + lp_string("")
        + struct.pack("<B", 0)
    )


def recv_exact(sock: socket.socket, size: int) -> bytes:
    buf = b""
    while len(buf) < size:
        chunk = sock.recv(size - len(buf))
        if not chunk:
            raise ConnectionError("peer closed before sending expected bytes")
        buf += chunk
    return buf


def random_device_id() -> str:
    return secrets.token_hex(16)  # 32 hex chars, matching client/src/device_identity.cpp


def do_handshake(control_port: int, device_id: str, client_name: str = "device-approval-smoke-test"):
    """Connects, sends Hello with `device_id`, returns (accepted, reject_reason)."""
    ctrl = socket.create_connection(("127.0.0.1", control_port), timeout=3)
    payload = hello_payload(client_name, "linux", 1280, 800, device_id)
    ctrl.sendall(header(PT_HELLO, len(payload)) + payload)

    ack_header = recv_exact(ctrl, 12)
    magic, _, ptype, psize = struct.unpack("<IHHI", ack_header)
    assert magic == MAGIC, "bad magic in HelloAck"
    assert ptype == PT_HELLO_ACK, f"expected HelloAck, got type {ptype}"

    ack_payload = recv_exact(ctrl, psize)
    accepted, reject_reason = struct.unpack_from("<BB", ack_payload, 0)
    ctrl.close()
    return accepted, reject_reason


def wait_for_pending_request(proc: subprocess.Popen, device_id: str, deadline_s: float = 3.0):
    """Polls the server's stdout for a pending-request line naming `device_id`."""
    deadline = time.time() + deadline_s
    buf = ""
    while time.time() < deadline:
        line = proc.stdout.readline()
        if not line:
            time.sleep(0.05)
            continue
        buf += line
        if "pending connection request" in line and device_id in line:
            return
    raise AssertionError(f"no pending-request log line for {device_id} appeared:\n{buf}")


def send_console_command(proc: subprocess.Popen, command: str):
    assert proc.stdin is not None
    proc.stdin.write(command + "\n")
    proc.stdin.flush()


def start_server(server_path: str, control_port: int, input_port: int, video_port: int, state_dir: str):
    proc = subprocess.Popen(
        [
            server_path,
            "--bind", "127.0.0.1",
            "--control-port", str(control_port),
            "--input-port", str(input_port),
            "--video-port", str(video_port),
            "--state-dir", state_dir,
            "--no-discovery",
        ],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    time.sleep(0.3)  # let the listeners bind
    return proc


def stop_server(proc: subprocess.Popen):
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()


def run(server_path: str) -> int:
    control_port, input_port, video_port = 28870, 28871, 28872
    state_dir = tempfile.mkdtemp(prefix="melonds-remote-approval-test-")
    device_a = random_device_id()
    device_b = random_device_id()

    try:
        proc = start_server(server_path, control_port, input_port, video_port, state_dir)
        try:
            # --- First attempt: unrecognized device -- must be rejected
            # with ApprovalRequired, and a pending-request line must show up ---
            accepted, reason = do_handshake(control_port, device_a)
            assert accepted == 0, "expected first unapproved attempt to be rejected"
            assert reason == REJECT_APPROVAL_REQUIRED, f"expected ApprovalRequired, got {reason}"
            print("[ok] unapproved connection rejected with ApprovalRequired")

            wait_for_pending_request(proc, device_a)
            print(f"[ok] host logged a pending request for {device_a}")

            # --- Approving via the console lets the same device connect ---
            send_console_command(proc, f"approve {device_a}")
            time.sleep(0.2)
            accepted, reason = do_handshake(control_port, device_a)
            assert accepted == 1, f"expected approved device to be accepted, reason={reason}"
            print("[ok] approving via console lets the device connect")

            # --- Reconnecting again works silently, no re-approval needed ---
            accepted, reason = do_handshake(control_port, device_a)
            assert accepted == 1, "expected reconnect with an already-approved device to be accepted"
            print("[ok] reconnecting with an already-approved device succeeds silently")

            # --- A different, never-seen device is still rejected ---
            accepted, reason = do_handshake(control_port, device_b)
            assert accepted == 0, "expected a different unapproved device to be rejected"
            assert reason == REJECT_APPROVAL_REQUIRED
            print("[ok] a different device is not accidentally approved")

            # --- Denying via the console leaves it rejected ---
            wait_for_pending_request(proc, device_b)
            send_console_command(proc, f"deny {device_b}")
            time.sleep(0.2)
            accepted, reason = do_handshake(control_port, device_b)
            assert accepted == 0, "expected a denied device to remain rejected"
            print("[ok] denying via console leaves the device rejected")

            if proc.poll() is not None:
                print("[FAIL] server exited during approval flow")
                return 1
        finally:
            stop_server(proc)

        # --- Restarting with the same --state-dir remembers device_a's approval ---
        proc2 = start_server(server_path, control_port, input_port, video_port, state_dir)
        try:
            accepted, reason = do_handshake(control_port, device_a)
            assert accepted == 1, "expected the approved device to survive a host restart"
            print("[ok] approved device survives a host restart (--state-dir persistence)")

            accepted, reason = do_handshake(control_port, device_b)
            assert accepted == 0, "expected the denied device to still be unapproved after restart"
            print("[ok] denied device is still unapproved after a host restart")
        finally:
            stop_server(proc2)

        print("\nDEVICE APPROVAL SMOKE TEST PASSED")
        return 0
    finally:
        shutil.rmtree(state_dir, ignore_errors=True)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} /path/to/dualdeck-host-service", file=sys.stderr)
        sys.exit(2)
    sys.exit(run(sys.argv[1]))
