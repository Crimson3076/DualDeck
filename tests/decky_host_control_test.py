#!/usr/bin/env python3
"""Drives the Decky plugin's host-side backend (decky-plugin/main.py's
get_local_host / answer_device / set_host_control) against the real
`dualdeck-host-service` and its local control socket.

Decky Loader isn't available here, so a minimal stand-in `decky` module
provides the two constants main.py reads. The service runs with
XDG_RUNTIME_DIR pointed at a temp directory, and so does this process,
which is how the plugin finds the socket on a real host.

  1. With no service running, get_local_host reports running: False.
  2. With the service up (--adapter-ipc, as the Host Control daemon runs
     it), a device that tries to connect is listed as pending.
  3. answer_device(approve) lets that device connect; deny keeps a
     second one out.
  4. set_host_control(True/False) sets and clears the Host Control
     override.
  5. discover_hosts runs the installed client's `--discover` (a stand-in
     script here, since this job doesn't build the client) with its
     bundled lib/ on LD_LIBRARY_PATH, and reports a missing client or
     unexpected output as an error.

Usage:
    python3 tests/decky_host_control_test.py /path/to/dualdeck-host-service
"""

import asyncio
import os
import shutil
import subprocess
import sys
import tempfile
import time
import types

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tests"))
sys.path.insert(0, os.path.join(REPO, "decky-plugin"))

import host_control_client  # noqa: E402 -- decky-plugin/host_control_client.py
from device_approval_smoke_test import (  # noqa: E402
    REJECT_APPROVAL_REQUIRED,
    do_handshake,
    random_device_id,
    stop_server,
)


def load_plugin(home):
    decky = types.ModuleType("decky")
    decky.DECKY_USER_HOME = home
    decky.DECKY_USER = "nobody-in-particular"  # not a real user: exercises the getuid() fallback
    decky.logger = None
    sys.modules["decky"] = decky
    import main  # noqa: E402 -- decky-plugin/main.py, needs the stub above first
    return main.Plugin()


def wait_for(predicate, what, deadline_s=3.0):
    deadline = time.time() + deadline_s
    while time.time() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {what}")


def check_discover(plugin, home, call):
    assert "isn't installed" in call(plugin.discover_hosts())["error"]

    root = os.path.join(home, ".config", "dualdeck-client", "install")
    os.makedirs(os.path.join(root, "lib"))
    stand_in = os.path.join(root, "dualdeck-client")
    host = ('{"address":"192.168.1.50","name":"htpc","controlPort":8760,"inputPort":8761,'
            '"videoPort":8762,"audioPort":8765,"system":{"id":"nds","name":"Nintendo DS"},'
            '"adapter":{"id":"melonds","name":"melonDS","version":"1"}}')
    with open(stand_in, "w") as f:
        f.write("#!/bin/sh\n"
                f'[ "$1" = --discover ] || exit 3\n'
                f'case "$LD_LIBRARY_PATH" in "{root}/lib"*) ;; *) exit 4 ;; esac\n'
                f"echo '[{host}]'\n")
    os.chmod(stand_in, 0o755)
    hosts = call(plugin.discover_hosts())["hosts"]
    assert [h["address"] for h in hosts] == ["192.168.1.50"] and hosts[0]["system"]["name"] == "Nintendo DS", hosts

    with open(stand_in, "w") as f:
        f.write("#!/bin/sh\necho not json\n")
    assert "unexpected" in call(plugin.discover_hosts())["error"]
    print("[ok] discover_hosts runs the installed client's --discover and parses its JSON")


def run(server_path):
    control_port, input_port, video_port = 28880, 28881, 28882
    work = tempfile.mkdtemp(prefix="dd-decky-")
    runtime_dir = os.path.join(work, "run")
    os.mkdir(runtime_dir, 0o700)
    os.environ["XDG_RUNTIME_DIR"] = runtime_dir
    plugin = load_plugin(os.path.join(work, "home"))

    def call(coro):
        return asyncio.run(coro)

    try:
        assert call(plugin.get_local_host()) == {"running": False}
        assert call(plugin.answer_device("abc", True)).startswith("error:")
        print("[ok] no host service: the panel section stays hidden")

        # Decky's backend may not inherit the session's XDG_RUNTIME_DIR;
        # the plugin then tries /run/user/<uid>, then ~/.cache.
        del os.environ["XDG_RUNTIME_DIR"]
        assert host_control_client.socket_path_candidates(home="/home/deck", uid=1000) == [
            "/run/user/1000/dualdeck/host-control.sock",
            "/home/deck/.cache/dualdeck/host-control.sock",
        ]
        os.environ["XDG_RUNTIME_DIR"] = runtime_dir

        proc = subprocess.Popen(
            [
                server_path,
                "--bind", "127.0.0.1",
                "--control-port", str(control_port),
                "--input-port", str(input_port),
                "--video-port", str(video_port),
                "--state-dir", os.path.join(work, "state"),
                "--no-discovery",
                "--no-approval-popup",
                "--adapter-ipc",
            ],
            env=dict(os.environ),
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            host = wait_for(lambda: (h := call(plugin.get_local_host())).get("mode") and h, "the control socket")
            # No emulator attached over adapter IPC yet, so the daemon is
            # in Host Control on its own (not overridden).
            assert host["running"] and host["mode"] == "host-control" and not host["overridden"], host
            assert host["pending"] == [], host
            print("[ok] running host service found through XDG_RUNTIME_DIR")

            device_a, device_b = random_device_id(), random_device_id()
            for device in (device_a, device_b):
                accepted, reason = do_handshake(control_port, device, client_name="Deck in the den")
                assert accepted == 0 and reason == REJECT_APPROVAL_REQUIRED
            pending = call(plugin.get_local_host())["pending"]
            assert {p["id"] for p in pending} == {device_a, device_b}, pending
            assert all(p["name"] == "Deck in the den" for p in pending), pending
            print("[ok] devices asking to connect are listed as pending")

            assert call(plugin.answer_device(device_a, True)) == "ok"
            assert call(plugin.answer_device(device_b, False)) == "ok"
            assert call(plugin.answer_device(device_b, True)).startswith("error: no pending request")
            assert call(plugin.get_local_host())["pending"] == []
            time.sleep(0.2)
            assert do_handshake(control_port, device_a)[0] == 1, "approved device should connect"
            assert do_handshake(control_port, device_b)[0] == 0, "denied device should stay out"
            print("[ok] approve lets a device in, deny keeps it out")

            assert call(plugin.set_host_control(True)) == "ok"
            host = call(plugin.get_local_host())
            assert host["mode"] == "host-control" and host["overridden"], host
            assert call(plugin.set_host_control(False)) == "ok"
            host = call(plugin.get_local_host())
            assert host["mode"] == "host-control" and not host["overridden"], host
            print("[ok] Host Control override goes on and back off")
        finally:
            stop_server(proc)

        check_discover(plugin, os.path.join(work, "home"), call)

        print("\nDECKY HOST CONTROL TEST PASSED")
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} /path/to/dualdeck-host-service", file=sys.stderr)
        sys.exit(2)
    sys.exit(run(sys.argv[1]))
