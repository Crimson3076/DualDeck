#!/usr/bin/env python3
"""SPEC.md section 20 criterion (12): sustained-stability test.

Runs a real session against the patched melonDS remote server for a
target duration: continuous UDP input at ~120Hz (cycling through button
patterns so input processing is continuously exercised, not idle),
continuous video draining, periodic heartbeats. Logs progress every 15s
and writes a final summary. Detects: connection drops, stalled video (no
new frame for too long), and reports melonDS process RSS growth over the
run.

Usage:
    python3 stability_test.py <control_port> <input_port> <video_port> \\
        <auth_token> <duration_seconds> <melonds_pid>

<melonds_pid> is used only to sample /proc/<pid>/status for RSS and to
confirm the process is still alive at the end; find it with
`pgrep -f melonDS` or from your own launch command.
"""
import socket, struct, time, threading, sys, subprocess

MAGIC = 0x444D5231
VERSION = 1
DS_A = 1 << 0
DS_B = 1 << 1
DS_UP = 1 << 4
DS_DOWN = 1 << 7


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


def rss_kb(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        return -1
    return -1


def main():
    control_port, input_port, video_port, token, duration_s, melonds_pid = (
        int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), sys.argv[4],
        float(sys.argv[5]), int(sys.argv[6])
    )

    start_rss = rss_kb(melonds_pid)
    print(f"start: melonDS pid={melonds_pid} RSS={start_rss}kB", flush=True)

    payload = lp("stability-test") + lp("linux") + struct.pack('<HH', 1280, 800) + lp(token)
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
    last_frame_time = [time.time()]
    errors = []
    lock = threading.Lock()

    pattern = [0, DS_A, 0, DS_B, 0, DS_UP, 0, DS_DOWN, 0, DS_A | DS_UP]
    t0 = time.time()

    def input_sender():
        i = 0
        next_switch = time.time() + 1.0
        while not stop.is_set():
            now = time.time()
            if now >= next_switch:
                i = (i + 1) % len(pattern)
                current_buttons[0] = pattern[i]
                next_switch = now + 1.0
            try:
                udp.sendto(header(3, 29) + controller_state(seq_counter[0], current_buttons[0]),
                           ('127.0.0.1', input_port))
            except OSError as e:
                errors.append(f"input_sender error at {now - t0:.1f}s: {e}")
            seq_counter[0] += 1
            time.sleep(1 / 120)

    def heartbeat():
        while not stop.is_set():
            try:
                ctrl.sendall(header(4, 0))
            except OSError as e:
                errors.append(f"heartbeat error at {time.time() - t0:.1f}s: {e}")
                return
            time.sleep(0.5)

    def video_reader():
        while not stop.is_set():
            try:
                vh = recv_exact(video, 12)
                vsize = struct.unpack('<IHHI', vh)[3]
                payload = recv_exact(video, vsize)
            except (ConnectionError, OSError) as e:
                errors.append(f"video_reader error at {time.time() - t0:.1f}s: {e}")
                return
            with lock:
                latest_frame[0] = tuple(payload[0:4])
                frame_count[0] += 1
                last_frame_time[0] = time.time()

    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    video = socket.create_connection(('127.0.0.1', video_port), timeout=30)

    threading.Thread(target=input_sender, daemon=True).start()
    threading.Thread(target=heartbeat, daemon=True).start()
    threading.Thread(target=video_reader, daemon=True).start()

    max_stall = 0.0
    next_log = t0 + 15.0
    while time.time() - t0 < duration_s:
        time.sleep(1.0)
        with lock:
            fc = frame_count[0]
            lf = latest_frame[0]
            lft = last_frame_time[0]
        stall = time.time() - lft
        max_stall = max(max_stall, stall)
        if time.time() >= next_log:
            elapsed = time.time() - t0
            rss = rss_kb(melonds_pid)
            print(f"t={elapsed:6.1f}s frames={fc:6d} last_pixel={lf} stall={stall:.1f}s "
                  f"rss={rss}kB errors={len(errors)}", flush=True)
            next_log = time.time() + 15.0
        if errors:
            print("STOPPING EARLY due to error:", errors[-1], flush=True)
            break

    stop.set()
    elapsed_total = time.time() - t0
    end_rss = rss_kb(melonds_pid)

    ctrl.close()
    video.close()

    print("=" * 60, flush=True)
    print(f"SUMMARY: ran {elapsed_total:.1f}s (target {duration_s:.0f}s)", flush=True)
    print(f"total frames received: {frame_count[0]}", flush=True)
    print(f"max video stall observed: {max_stall:.2f}s", flush=True)
    delta = (end_rss - start_rss) if start_rss >= 0 and end_rss >= 0 else 'n/a'
    print(f"RSS: start={start_rss}kB end={end_rss}kB delta={delta}kB", flush=True)
    print(f"errors: {len(errors)}", flush=True)
    for e in errors:
        print("  -", e, flush=True)
    alive = subprocess.run(['kill', '-0', str(melonds_pid)]).returncode == 0
    print(f"melonDS process still alive: {alive}", flush=True)
    passed = not errors and elapsed_total >= duration_s * 0.95 and alive
    print("STABILITY TEST " + ("PASSED" if passed else "FAILED/INCOMPLETE"), flush=True)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
