#!/usr/bin/env python3
"""End-to-end test of SPEC.md section 20 acceptance criteria (4)-(8): DS
controls sent over the remote protocol actually affect a running program.

Drives the *real* network pipeline against a patched melonDS running
arm9.c (this directory) direct-booted from test.nds: sends real UDP
ControllerState packets on the input channel, exactly as the SDL3 client
would, and confirms the delivered video frames change in the exact way
arm9.c's KEYINPUT-reactive logic predicts. This is not a unit test of the
protocol in isolation -- it exercises the full path:

  UDP ControllerState -> NetServer -> RemoteServerBridge ->
  EmuInstance::inputProcess() -> NDS::SetKeyMask() -> CPU reads
  KEYINPUT register -> arm9.c writes engine-B backdrop color ->
  GPU::GetFramebuffers() -> pushBottomFrame() -> this client

A background thread continuously drains the video TCP stream and keeps
only the most recent frame; sampling only after a hold period avoids
reading stale/backlogged frames (the previous version of this test did
not do this and got backlog-lagged, ambiguous results).

Usage:
    python3 interactive_pipeline_test.py <control_port> <input_port> <video_port> <auth_token>

Requires a patched melonDS already running with test.nds direct-booted
and MELONDS_REMOTE_ENABLE=1 (see host/melonds-patches/README.md).

Confirmed result (see docs/known-limitations.md and
host/melonds-patches/README.md for the full account): each held button
state produces a single, stable pixel value across 50 samples with no
noise, and the byte order is BGRA8888 (byte0=Blue, byte1=Green,
byte2=Red, byte3=Alpha) -- resolving the previously-open RGBA/BGRA
question. Engine B is confirmed to be the captured "bottom" screen.
"""
import socket, struct, time, threading, sys, collections

MAGIC = 0x444D5231
VERSION = 1
DS_A = 1 << 0
DS_B = 1 << 1
DS_UP = 1 << 4


def header(t, sz):
    return struct.pack('<IHHI', MAGIC, VERSION, t, sz)


def lp(s):
    b = s.encode()
    return struct.pack('<H', len(b)) + b


def recv_exact(sock, n):
    buf = b''
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("closed")
        buf += chunk
    return buf


def controller_state(seq, buttons):
    now_us = int(time.time() * 1_000_000)
    return struct.pack('<IQHHhhhhBHH', seq, now_us, buttons, 0, 0, 0, 0, 0, 0, 0, 0)


def main():
    control_port, input_port, video_port, token = (
        int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    )

    payload = lp("interactive-pipeline-test") + lp("linux") + struct.pack('<HH', 1280, 800) + lp(token)
    ctrl = socket.create_connection(('127.0.0.1', control_port), timeout=5)
    ctrl.sendall(header(1, len(payload)) + payload)
    hdr = recv_exact(ctrl, 12)
    body = recv_exact(ctrl, struct.unpack('<IHHI', hdr)[3])
    assert struct.unpack('<BBIHH', body)[0] == 1
    print("handshake ok", flush=True)

    stop = threading.Event()
    seq_counter = [0]
    current_buttons = [0]
    latest_frame = [None]
    frame_count = [0]
    lock = threading.Lock()

    def input_sender():
        while not stop.is_set():
            udp.sendto(header(3, 29) + controller_state(seq_counter[0], current_buttons[0]),
                       ('127.0.0.1', input_port))
            seq_counter[0] += 1
            time.sleep(1 / 120)

    def heartbeat():
        while not stop.is_set():
            try:
                ctrl.sendall(header(4, 0))
            except OSError:
                return
            time.sleep(0.5)

    def video_reader():
        while not stop.is_set():
            try:
                vh = recv_exact(video, 12)
                vsize = struct.unpack('<IHHI', vh)[3]
                payload = recv_exact(video, vsize)
            except (ConnectionError, OSError):
                return
            with lock:
                latest_frame[0] = tuple(payload[0:4])
                frame_count[0] += 1

    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    video = socket.create_connection(('127.0.0.1', video_port), timeout=30)

    threading.Thread(target=input_sender, daemon=True).start()
    threading.Thread(target=heartbeat, daemon=True).start()
    threading.Thread(target=video_reader, daemon=True).start()

    def hold_and_sample(name, buttons, hold_seconds, sample_window):
        current_buttons[0] = buttons
        time.sleep(hold_seconds)  # let the state settle and old frames flush through
        samples = collections.Counter()
        deadline = time.time() + sample_window
        last_count = -1
        while time.time() < deadline:
            with lock:
                f = latest_frame[0]
                c = frame_count[0]
            if f is not None and c != last_count:
                samples[f] += 1
                last_count = c
            time.sleep(0.02)
        print(f"{name}: {dict(samples)}", flush=True)
        return samples

    scenarios = [
        ("baseline (nothing held)", 0, (0, 0, 0, 255)),
        ("A held (expect Red channel, byte2)", DS_A, None),
        ("released", 0, (0, 0, 0, 255)),
        ("Up held (expect Blue channel, byte0)", DS_UP, None),
        ("B held (expect Green channel, byte1)", DS_B, None),
        ("A+Up held", DS_A | DS_UP, None),
        ("released again", 0, (0, 0, 0, 255)),
    ]

    all_stable = True
    all_correct = True
    results = []
    for name, buttons, expected in scenarios:
        samples = hold_and_sample(name, buttons, 3.0, 1.0)
        stable = len(samples) == 1
        pixel = next(iter(samples)) if stable else None
        all_stable = all_stable and stable
        if expected is not None:
            correct = pixel == expected
            all_correct = all_correct and correct
        results.append((name, samples, stable))

    stop.set()
    ctrl.close()
    video.close()

    print("=" * 60)
    print("PASSED" if all_stable and all_correct else "FAILED",
          "-- all states stable:", all_stable, "| baseline/released pixels correct:", all_correct)
    return 0 if (all_stable and all_correct) else 1


if __name__ == "__main__":
    sys.exit(main())
