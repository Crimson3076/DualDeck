#!/usr/bin/env python3
"""Integration smoke test for the pairing-code flow (spec section 13's
"six-digit pairing code" option, `host/remote-server/include/host/pairing_manager.h`).

Starts the real `melonds-remote-server` binary with NO `--auth-token`
(pairing mode, the default), then exercises the same state machine a real
client would hit:

  1. An unrecognized connection attempt is rejected with `PairingRequired`,
     and the server's log gets a 6-digit code.
  2. Presenting that code is accepted and returns a fresh persistent
     pairing token.
  3. Reconnecting with that token is accepted silently (no new token).
  4. The (now-consumed, single-use) original code no longer works.
  5. Restarting the server with `--state-dir` pointed at the same
     directory remembers the token across the restart.

This is a protocol-level test (raw sockets, not the actual SDL3 client) --
see `host/melonds-patches/README.md` item 8 for the corresponding
real-client verification account. Complements `tests/smoke_test.py`,
which covers the static `--auth-token` path instead.

Usage:
    python3 tests/pairing_smoke_test.py /path/to/melonds-remote-server
"""

import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

MAGIC = 0x444D5231
VERSION = 2

PT_HELLO = 1
PT_HELLO_ACK = 2

REJECT_NONE = 0
REJECT_AUTH_FAILED = 2
REJECT_PAIRING_REQUIRED = 4


def header(packet_type: int, payload_size: int) -> bytes:
    return struct.pack("<IHHI", MAGIC, VERSION, packet_type, payload_size)


def lp_string(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack("<H", len(b)) + b


def hello_payload(name: str, platform: str, width: int, height: int, token: str) -> bytes:
    return lp_string(name) + lp_string(platform) + struct.pack("<HH", width, height) + lp_string(token)


def recv_exact(sock: socket.socket, size: int) -> bytes:
    buf = b""
    while len(buf) < size:
        chunk = sock.recv(size - len(buf))
        if not chunk:
            raise ConnectionError("peer closed before sending expected bytes")
        buf += chunk
    return buf


def do_handshake(control_port: int, token: str):
    """Connects, sends Hello with `token`, returns (accepted, reject_reason, pairing_token)."""
    ctrl = socket.create_connection(("127.0.0.1", control_port), timeout=3)
    payload = hello_payload("pairing-smoke-test", "linux", 1280, 800, token)
    ctrl.sendall(header(PT_HELLO, len(payload)) + payload)

    ack_header = recv_exact(ctrl, 12)
    magic, _, ptype, psize = struct.unpack("<IHHI", ack_header)
    assert magic == MAGIC, "bad magic in HelloAck"
    assert ptype == PT_HELLO_ACK, f"expected HelloAck, got type {ptype}"

    ack_payload = recv_exact(ctrl, psize)
    accepted, reject_reason = struct.unpack_from("<BB", ack_payload, 0)
    token_len = struct.unpack_from("<H", ack_payload, 10)[0]
    pairing_token = ack_payload[12:12 + token_len].decode("utf-8")
    ctrl.close()
    return accepted, reject_reason, pairing_token


def wait_for_code(proc: subprocess.Popen, deadline_s: float = 3.0):
    """Polls the server's stdout for a freshly-logged pairing code."""
    deadline = time.time() + deadline_s
    buf = ""
    while time.time() < deadline:
        line = proc.stdout.readline()
        if not line:
            time.sleep(0.05)
            continue
        buf += line
        m = re.search(r"pairing code: (\d{6})", line)
        if m:
            return m.group(1)
    raise AssertionError(f"no pairing code appeared in server output:\n{buf}")


def start_server(server_path: str, control_port: int, input_port: int, video_port: int, state_dir: str):
    proc = subprocess.Popen(
        [
            server_path,
            "--bind", "127.0.0.1",
            "--control-port", str(control_port),
            "--input-port", str(input_port),
            "--video-port", str(video_port),
            "--state-dir", state_dir,
        ],
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
    control_port, input_port, video_port = 28860, 28861, 28862
    state_dir = tempfile.mkdtemp(prefix="melonds-remote-pairing-test-")

    try:
        proc = start_server(server_path, control_port, input_port, video_port, state_dir)
        try:
            # --- First attempt: no token, nothing paired yet -- must be rejected
            # with PairingRequired, and a code must show up in the log ---
            accepted, reason, _ = do_handshake(control_port, "")
            assert accepted == 0, "expected first unpaired attempt to be rejected"
            assert reason == REJECT_PAIRING_REQUIRED, f"expected PairingRequired, got {reason}"
            print("[ok] unpaired connection rejected with PairingRequired")

            code = wait_for_code(proc)
            print(f"[ok] host displayed a pairing code ({code})")

            # --- Presenting the code pairs successfully and issues a token ---
            accepted, reason, token = do_handshake(control_port, code)
            assert accepted == 1, f"expected code to be accepted, reason={reason}"
            assert len(token) == 32, f"expected a 32-char pairing token, got {token!r}"
            print("[ok] entering the code pairs successfully and issues a persistent token")

            # --- Reconnecting with the token works silently (no new token issued) ---
            accepted, reason, new_token = do_handshake(control_port, token)
            assert accepted == 1, "expected reconnect with saved token to be accepted"
            assert new_token == "", "expected no new token on an already-paired reconnect"
            print("[ok] reconnecting with the saved token succeeds silently")

            # --- The original code is single-use: it must not work again ---
            accepted, reason, _ = do_handshake(control_port, code)
            assert accepted == 0, "expected the already-consumed code to be rejected"
            print("[ok] the original code cannot be reused")

            if proc.poll() is not None:
                print("[FAIL] server exited during pairing flow")
                return 1
        finally:
            stop_server(proc)

        # --- Restarting with the same --state-dir remembers the token ---
        proc2 = start_server(server_path, control_port, input_port, video_port, state_dir)
        try:
            accepted, reason, _ = do_handshake(control_port, token)
            assert accepted == 1, "expected the saved token to survive a host restart"
            print("[ok] paired token survives a host restart (--state-dir persistence)")
        finally:
            stop_server(proc2)

        print("\nPAIRING SMOKE TEST PASSED")
        return 0
    finally:
        shutil.rmtree(state_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(run(sys.argv[1]))
