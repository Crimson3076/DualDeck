#!/usr/bin/env python3
"""End-to-end smoke test for host/remote-server.

Starts the standalone `melonds-remote-server` binary, then exercises it as
a client would: TCP control handshake, a UDP ControllerState packet, a
malformed UDP packet (must be silently rejected, not crash the server), a
TCP video-frame read, and finally a control disconnect that must trigger
"release all inputs" on the host side.

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
PT_VIDEO_FRAME = 7

FRAME_BYTES = 256 * 192 * 4


def header(packet_type: int, payload_size: int) -> bytes:
    return struct.pack("<IHHI", MAGIC, VERSION, packet_type, payload_size)


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


def run(server_path: str) -> int:
    control_port, input_port, video_port = 28760, 28761, 28762
    proc = subprocess.Popen(
        [
            server_path,
            "--bind", "127.0.0.1",
            "--control-port", str(control_port),
            "--input-port", str(input_port),
            "--video-port", str(video_port),
            "--timeout-ms", "500",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    try:
        time.sleep(0.3)  # let the listeners bind

        ctrl = socket.create_connection(("127.0.0.1", control_port), timeout=3)
        ctrl.sendall(header(PT_HELLO, 0))
        ack = recv_exact(ctrl, 12)
        magic, _, ptype, _ = struct.unpack("<IHHI", ack)
        assert magic == MAGIC, "bad magic in HelloAck"
        assert ptype == PT_HELLO_ACK, f"expected HelloAck, got type {ptype}"
        print("[ok] control handshake")

        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        payload = controller_state_payload(seq=1, buttons=0x0001, touch_x=128, touch_y=96)
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
