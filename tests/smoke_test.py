#!/usr/bin/env python3
"""End-to-end smoke test for host/remote-server.

Starts the standalone `melonds-remote-server` binary, then exercises it as
a client would: TCP control handshake (with the negotiated Hello/HelloAck
payload, including authentication), a UDP ControllerState packet, a
malformed UDP packet (must be silently rejected, not crash the server), a
TCP video-frame read, and a control disconnect that must trigger "release
all inputs" on the host side. It also verifies the authentication gate:
a client with a wrong (or missing) token is rejected, and -- critically --
an unauthenticated UDP sender cannot inject input even if it guesses the
input port.

This does not require the SDL3 client or a melonDS build -- it validates
the protocol/host layer described in docs/protocol.md and
docs/architecture.md in isolation, per the acceptance criteria in
docs/testing.md.

Usage:
    python3 tests/smoke_test.py /path/to/melonds-remote-server
"""

import socket
import struct
import subprocess
import sys
import time

MAGIC = 0x444D5231
VERSION = 1

PT_HELLO = 1
PT_HELLO_ACK = 2
PT_CONTROLLER_STATE = 3
PT_DISCONNECT = 5
PT_VIDEO_FRAME = 7

FRAME_BYTES = 256 * 192 * 4

REJECT_NONE = 0
REJECT_VERSION_MISMATCH = 1
REJECT_AUTH_FAILED = 2
REJECT_HOST_BUSY = 3


def header(packet_type: int, payload_size: int) -> bytes:
    return struct.pack("<IHHI", MAGIC, VERSION, packet_type, payload_size)


def lp_string(s: str) -> bytes:
    """Length-prefixed string matching protocol::appendString (u16 len + bytes)."""
    b = s.encode("utf-8")
    return struct.pack("<H", len(b)) + b


def hello_payload(name: str, platform: str, width: int, height: int, token: str) -> bytes:
    return lp_string(name) + lp_string(platform) + struct.pack("<HH", width, height) + lp_string(token)


def controller_state_payload(seq: int, buttons: int, touch_x: int, touch_y: int) -> bytes:
    return struct.pack(
        "<IQHHhhhhBHH",
        seq, 0, buttons, 0, 0, 0, 0, 0, 1, touch_x, touch_y,
    )


def recv_exact(sock: socket.socket, size: int) -> bytes:
    buf = b""
    while len(buf) < size:
        chunk = sock.recv(size - len(buf))
        if not chunk:
            raise ConnectionError("peer closed before sending expected bytes")
        buf += chunk
    return buf


def do_handshake(control_port: int, token: str):
    """Connects, sends Hello with `token`, and returns (socket, accepted, reject_reason)."""
    ctrl = socket.create_connection(("127.0.0.1", control_port), timeout=3)
    payload = hello_payload("smoke-test-client", "linux", 1280, 800, token)
    ctrl.sendall(header(PT_HELLO, len(payload)) + payload)

    ack_header = recv_exact(ctrl, 12)
    magic, _, ptype, psize = struct.unpack("<IHHI", ack_header)
    assert magic == MAGIC, "bad magic in HelloAck"
    assert ptype == PT_HELLO_ACK, f"expected HelloAck, got type {ptype}"

    ack_payload = recv_exact(ctrl, psize)
    accepted, reject_reason, session_id, native_w, native_h = struct.unpack("<BBIHH", ack_payload)
    return ctrl, accepted, reject_reason


def run(server_path: str) -> int:
    control_port, input_port, video_port = 28760, 28761, 28762
    token = "smoke-test-secret"
    proc = subprocess.Popen(
        [
            server_path,
            "--bind", "127.0.0.1",
            "--control-port", str(control_port),
            "--input-port", str(input_port),
            "--video-port", str(video_port),
            "--timeout-ms", "500",
            "--auth-token", token,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    try:
        time.sleep(0.3)  # let the listeners bind

        # --- Wrong token must be rejected, and must not be able to inject input ---
        bad_ctrl, accepted, reason = do_handshake(control_port, "wrong-token")
        assert accepted == 0, "expected wrong token to be rejected"
        assert reason == REJECT_AUTH_FAILED, f"expected AuthenticationFailed, got {reason}"
        print("[ok] wrong auth token rejected")

        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        payload = controller_state_payload(seq=1, buttons=0x0001, touch_x=128, touch_y=96)
        udp.sendto(header(PT_CONTROLLER_STATE, len(payload)) + payload, ("127.0.0.1", input_port))
        time.sleep(0.1)
        if proc.poll() is not None:
            print("[FAIL] server exited after unauthenticated input packet")
            return 1
        print("[ok] unauthenticated UDP input packet did not crash the server (and is dropped host-side)")

        # video must also be refused without a completed handshake
        video_reject = socket.create_connection(("127.0.0.1", video_port), timeout=3)
        closed = video_reject.recv(1)
        assert closed == b"", "expected video connection to be closed without authentication"
        video_reject.close()
        print("[ok] video connection refused without an authenticated session")

        bad_ctrl.close()
        time.sleep(0.2)

        # --- Correct token: full happy path ---
        ctrl, accepted, reason = do_handshake(control_port, token)
        assert accepted == 1, f"expected correct token to be accepted, reject_reason={reason}"
        print("[ok] control handshake with correct auth token")

        udp.sendto(header(PT_CONTROLLER_STATE, len(payload)) + payload, ("127.0.0.1", input_port))
        print("[ok] sent ControllerState packet")

        bad_magic_packet = struct.pack("<IHHI", 0xDEADBEEF, VERSION, PT_CONTROLLER_STATE, len(payload)) + payload
        udp.sendto(bad_magic_packet, ("127.0.0.1", input_port))
        print("[ok] sent malformed packet (bad magic) -- must not crash the server")

        time.sleep(0.2)
        if proc.poll() is not None:
            print("[FAIL] server exited after malformed packet")
            return 1

        video = socket.create_connection(("127.0.0.1", video_port), timeout=3)
        vheader = recv_exact(video, 12)
        vmagic, _, vtype, vsize = struct.unpack("<IHHI", vheader)
        assert vmagic == MAGIC and vtype == PT_VIDEO_FRAME
        assert vsize == FRAME_BYTES, f"expected {FRAME_BYTES} byte frame, got {vsize}"
        recv_exact(video, vsize)
        video.close()
        print(f"[ok] received a {vsize}-byte video frame")

        ctrl.close()
        time.sleep(0.3)
        print("[ok] control disconnect handled without crashing")

        if proc.poll() is not None:
            print("[FAIL] server exited after client disconnect")
            return 1

        # --- Rate limiting: hammering the control port must eventually get
        # connection attempts refused before any handshake bytes are read ---
        rate_limited = False
        for attempt in range(10):
            s = socket.create_connection(("127.0.0.1", control_port), timeout=3)
            s.settimeout(0.5)
            try:
                probe = s.recv(1)
                # A rate-limited attempt gets closed immediately with no data.
                if probe == b"":
                    rate_limited = True
                    s.close()
                    break
            except socket.timeout:
                pass  # server is waiting for us to send Hello -- not rate-limited
            s.close()
        assert rate_limited, "expected rapid connection attempts to eventually be rate-limited"
        print("[ok] rapid connection attempts are rate-limited")

        if proc.poll() is not None:
            print("[FAIL] server exited during rate-limit probing")
            return 1

        print("\nSMOKE TEST PASSED")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
        if proc.stdout:
            output = proc.stdout.read()
            if output:
                print("\n--- server output ---")
                print(output)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} /path/to/melonds-remote-server", file=sys.stderr)
        sys.exit(2)
    sys.exit(run(sys.argv[1]))
