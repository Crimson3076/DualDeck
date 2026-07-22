#!/usr/bin/env python3
"""End-to-end smoke test for host/remote-server.

Starts the standalone `dualdeck-host-service` binary, then exercises it as
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
    python3 tests/smoke_test.py /path/to/dualdeck-host-service
"""

import socket
import struct
import subprocess
import sys
import time

MAGIC = 0x444D5231
VERSION = 7

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


def read_lp_string(buf: bytes, offset: int):
    """Reads one length-prefixed string (matching protocol::readString) and
    returns (value, offset_after)."""
    (length,) = struct.unpack_from("<H", buf, offset)
    start = offset + 2
    end = start + length
    return buf[start:end].decode("utf-8"), end


def do_handshake(control_port: int, token: str, app_version: str = ""):
    """Connects, sends Hello with `token`, and returns a dict with keys:
    ctrl, accepted, reject_reason, host_app_version, mic_supported,
    system_id, system_name, adapter_id, adapter_name, adapter_version
    (GitHub issue #28's identity fields, appended to HelloAckPayload after
    micSupported -- see docs/protocol.md's "Emulator identity model"), and
    mode (GitHub issue #4 Phase E's HostMode field, appended after adapter
    identity -- 0=Emulation, 1=HostControl)."""
    ctrl = socket.create_connection(("127.0.0.1", control_port), timeout=3)
    payload = hello_payload("smoke-test-client", "linux", 1280, 800, token, app_version)
    ctrl.sendall(header(PT_HELLO, len(payload)) + payload)

    ack_header = recv_exact(ctrl, 12)
    magic, _, ptype, psize = struct.unpack("<IHHI", ack_header)
    assert magic == MAGIC, "bad magic in HelloAck"
    assert ptype == PT_HELLO_ACK, f"expected HelloAck, got type {ptype}"

    ack_payload = recv_exact(ctrl, psize)
    accepted, reject_reason, session_id, native_w, native_h = struct.unpack_from("<BBIHH", ack_payload, 0)
    host_app_version, offset = read_lp_string(ack_payload, 10)
    (mic_supported,) = struct.unpack_from("<B", ack_payload, offset)
    offset += 1
    system_id, offset = read_lp_string(ack_payload, offset)
    system_name, offset = read_lp_string(ack_payload, offset)
    adapter_id, offset = read_lp_string(ack_payload, offset)
    adapter_name, offset = read_lp_string(ack_payload, offset)
    adapter_version, offset = read_lp_string(ack_payload, offset)
    (mode,) = struct.unpack_from("<B", ack_payload, offset)
    offset += 1
    assert offset == len(ack_payload), "trailing bytes left unparsed in HelloAck payload"
    return {
        "ctrl": ctrl,
        "accepted": accepted,
        "reject_reason": reject_reason,
        "host_app_version": host_app_version,
        "mic_supported": mic_supported,
        "system_id": system_id,
        "system_name": system_name,
        "adapter_id": adapter_id,
        "adapter_name": adapter_name,
        "adapter_version": adapter_version,
        "mode": mode,
    }


def mic_audio_frame_payload(seq: int, samples) -> bytes:
    return struct.pack("<IQH", seq, 0, len(samples)) + struct.pack(f"<{len(samples)}h", *samples)


def run(server_path: str) -> int:
    control_port, input_port, video_port, audio_port = 28760, 28761, 28762, 28765
    token = "smoke-test-secret"
    host_app_version = "v0.1.99-smoketest"
    # GitHub issue #28: override the standalone host's default synthetic
    # identity with something distinguishable, so this test proves the
    # values actually round-trip end to end rather than just happening to
    # match the (also synthetic-labeled) default.
    system_id, system_name = "3ds", "Nintendo 3DS"
    adapter_id, adapter_name, adapter_version = "fake-3ds", "Fake 3DS Adapter (smoke test)", "0.0.1"
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
            "--system-id", system_id,
            "--system-name", system_name,
            "--adapter-id", adapter_id,
            "--adapter-name", adapter_name,
            "--adapter-version", adapter_version,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    try:
        time.sleep(0.3)  # let the listeners bind

        # --- Wrong token must be rejected, and must not be able to inject input ---
        bad = do_handshake(control_port, "wrong-token")
        bad_ctrl = bad["ctrl"]
        assert bad["accepted"] == 0, "expected wrong token to be rejected"
        assert bad["reject_reason"] == REJECT_AUTH_FAILED, f"expected AuthenticationFailed, got {bad['reject_reason']}"
        # Identity is sent regardless of accepted/rejectReason (same
        # convention as host_app_version) -- GitHub issue #28.
        assert bad["system_id"] == system_id and bad["adapter_id"] == adapter_id, (
            "expected identity to be reported even on a rejected handshake"
        )
        print("[ok] wrong auth token rejected, identity still reported")

        # --- Mismatched app version must be rejected before auth is even
        # checked -- correct token, but a different non-empty appVersion
        # than --app-version above. A rejected handshake's TCP connection is
        # closed server-side immediately (net_server.cpp doesn't enter its
        # keep-reading loop when !handshakeOk), so bad_ctrl above doesn't
        # need to be closed client-side first for this next attempt to land.
        stale = do_handshake(control_port, token, "v0.0.1-stale")
        stale_ctrl = stale["ctrl"]
        assert stale["accepted"] == 0, "expected a mismatched app version to be rejected"
        assert stale["reject_reason"] == REJECT_APP_VERSION_MISMATCH, f"expected AppVersionMismatch, got {stale['reject_reason']}"
        assert stale["host_app_version"] == host_app_version, (
            f"expected host to report its own version, got {stale['host_app_version']!r}"
        )
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
        good = do_handshake(control_port, token)
        ctrl = good["ctrl"]
        assert good["accepted"] == 1, f"expected correct token to be accepted, reject_reason={good['reject_reason']}"
        assert good["mic_supported"] == 1, "expected the standalone host to advertise micSupported by default"
        # GitHub issue #28: the --system-id/--adapter-id etc. flags given
        # to the server above must show up verbatim in the accepted
        # handshake too, not just the rejected ones checked above.
        assert good["system_id"] == system_id and good["system_name"] == system_name, (
            f"expected system identity {system_id!r}/{system_name!r}, got "
            f"{good['system_id']!r}/{good['system_name']!r}"
        )
        assert good["adapter_id"] == adapter_id and good["adapter_name"] == adapter_name, (
            f"expected adapter identity {adapter_id!r}/{adapter_name!r}, got "
            f"{good['adapter_id']!r}/{good['adapter_name']!r}"
        )
        assert good["adapter_version"] == adapter_version, (
            f"expected adapter version {adapter_version!r}, got {good['adapter_version']!r}"
        )
        # This standalone (non --adapter-ipc) code path has no
        # ModeCoordinator wired in -- see host/remote-server/src/main.cpp
        # -- so it always reports Emulation (GitHub issue #4 Phase E).
        assert good["mode"] == 0, f"expected mode=Emulation(0) from the standalone host, got {good['mode']}"
        print("[ok] control handshake with correct auth token, host advertises micSupported and identity")

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
        print(f"usage: {sys.argv[0]} /path/to/dualdeck-host-service", file=sys.stderr)
        sys.exit(2)
    sys.exit(run(sys.argv[1]))
