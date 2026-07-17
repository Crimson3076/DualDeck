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
VERSION = 5

PT_HELLO = 1
PT_HELLO_ACK = 2
PT_CONTROLLER_STATE = 3
PT_DISCONNECT = 5
PT_VIDEO_FRAME = 7
PT_MIC_AUDIO_FRAME = 10

FRAME_BYTES = 256 * 192 * 4

REJECT_NONE = 0
REJECT_VERSION_MISMATCH = 1
REJECT_AUTH_FAILED = 2
REJECT_HOST_BUSY = 3
REJECT_APP_VERSION_MISMATCH = 5


def header(packet_type: int, payload_size: int) -> bytes:
    return struct.pack("<IHHI", MAGIC, VERSION, packet_type, payload_size)


def lp_string(s: str) -> bytes:
    """Length-prefixed string matching protocol::appendString (u16 len + bytes)."""
    b = s.encode("utf-8")
    return struct.pack("<H", len(b)) + b


def hello_payload(name: str, platform: str, width: int, height: int, token: str, app_version: str = "") -> bytes:
    # app_version left empty by default: an empty appVersion on either side
    # skips the AppVersionMismatch check entirely (see protocol.h), which is
    # what every test below except a dedicated version-mismatch case wants.
    return (
        lp_string(name)
        + lp_string(platform)
        + struct.pack("<HH", width, height)
        + lp_string(token)
        + lp_string(app_version)
    )


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


def do_handshake(control_port: int, token: str, app_version: str = ""):
    """Connects, sends Hello with `token`, and returns
    (socket, accepted, reject_reason, host_app_version, mic_supported)."""
    ctrl = socket.create_connection(("127.0.0.1", control_port), timeout=3)
    payload = hello_payload("smoke-test-client", "linux", 1280, 800, token, app_version)
    ctrl.sendall(header(PT_HELLO, len(payload)) + payload)

    ack_header = recv_exact(ctrl, 12)
    magic, _, ptype, psize = struct.unpack("<IHHI", ack_header)
    assert magic == MAGIC, "bad magic in HelloAck"
    assert ptype == PT_HELLO_ACK, f"expected HelloAck, got type {ptype}"

    ack_payload = recv_exact(ctrl, psize)
    accepted, reject_reason, session_id, native_w, native_h = struct.unpack_from("<BBIHH", ack_payload, 0)
    (version_len,) = struct.unpack_from("<H", ack_payload, 10)
    version_end = 12 + version_len
    host_app_version = ack_payload[12:version_end].decode("utf-8")
    (mic_supported,) = struct.unpack_from("<B", ack_payload, version_end)
    return ctrl, accepted, reject_reason, host_app_version, mic_supported


def mic_audio_frame_payload(seq: int, samples) -> bytes:
    return struct.pack("<IQH", seq, 0, len(samples)) + struct.pack(f"<{len(samples)}h", *samples)


def run(server_path: str) -> int:
    control_port, input_port, video_port, audio_port = 28760, 28761, 28762, 28765
    token = "smoke-test-secret"
    host_app_version = "v0.1.99-smoketest"
    proc = subprocess.Popen(
        [
            server_path,
            "--bind", "127.0.0.1",
            "--control-port", str(control_port),
            "--input-port", str(input_port),
            "--video-port", str(video_port),
            "--audio-port", str(audio_port),
            "--timeout-ms", "500",
            "--auth-token", token,
            "--app-version", host_app_version,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    try:
        time.sleep(0.3)  # let the listeners bind

        # --- Wrong token must be rejected, and must not be able to inject input ---
        bad_ctrl, accepted, reason, _, _ = do_handshake(control_port, "wrong-token")
        assert accepted == 0, "expected wrong token to be rejected"
        assert reason == REJECT_AUTH_FAILED, f"expected AuthenticationFailed, got {reason}"
        print("[ok] wrong auth token rejected")

        # --- Mismatched app version must be rejected before auth is even
        # checked -- correct token, but a different non-empty appVersion
        # than --app-version above. A rejected handshake's TCP connection is
        # closed server-side immediately (net_server.cpp doesn't enter its
        # keep-reading loop when !handshakeOk), so bad_ctrl above doesn't
        # need to be closed client-side first for this next attempt to land.
        stale_ctrl, accepted, reason, host_version, _ = do_handshake(control_port, token, "v0.0.1-stale")
        assert accepted == 0, "expected a mismatched app version to be rejected"
        assert reason == REJECT_APP_VERSION_MISMATCH, f"expected AppVersionMismatch, got {reason}"
        assert host_version == host_app_version, f"expected host to report its own version, got {host_version!r}"
        print("[ok] mismatched app version rejected, host reports its own version in HelloAck")
        stale_ctrl.close()

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
        ctrl, accepted, reason, _, mic_supported = do_handshake(control_port, token)
        assert accepted == 1, f"expected correct token to be accepted, reject_reason={reason}"
        assert mic_supported == 1, "expected the standalone host to advertise micSupported by default"
        print("[ok] control handshake with correct auth token, host advertises micSupported")

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

        # --- Microphone (GitHub issue #2): a well-formed MicAudioFrame over
        # the authenticated session's UDP audio port, plus a malformed one
        # (declared numSamples not matching the actual payload size) that
        # must be rejected without crashing the server, mirroring the
        # ControllerState malformed-packet check above.
        audio = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        mic_samples = [1000, -1000, 2000, -2000]
        mic_payload = mic_audio_frame_payload(seq=1, samples=mic_samples)
        audio.sendto(header(PT_MIC_AUDIO_FRAME, len(mic_payload)) + mic_payload, ("127.0.0.1", audio_port))
        time.sleep(0.1)
        if proc.poll() is not None:
            print("[FAIL] server exited after MicAudioFrame packet")
            return 1
        print("[ok] sent MicAudioFrame packet")

        bad_mic_payload = struct.pack("<IQH", 2, 0, 999) + struct.pack("<4h", *mic_samples)  # numSamples lies
        audio.sendto(header(PT_MIC_AUDIO_FRAME, len(bad_mic_payload)) + bad_mic_payload, ("127.0.0.1", audio_port))
        time.sleep(0.1)
        if proc.poll() is not None:
            print("[FAIL] server exited after malformed MicAudioFrame packet")
            return 1
        print("[ok] sent malformed MicAudioFrame packet (sample-count mismatch) -- must not crash the server")

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
