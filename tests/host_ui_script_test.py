#!/usr/bin/env python3
"""Tests dualdeck-host.sh's side of the DualDeck Host window protocol
(host/ui/src/protocol.h) without SDL: a stand-in internal/dualdeck-host-ui
logs every request it gets and answers from a scripted list, the way the
real window answers with whatever the user picked.

Covers: the script starts the window and drives its menus instead of the
terminal prompts, sends busy/message/confirm requests with escaped text,
closes the window before exec'ing into an emulator, honors "no" on a
confirm, ends when the window is closed, and falls back to the terminal
menu when the window never says it's ready.

Usage:
    python3 tests/host_ui_script_test.py
"""

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from host_client_menu_lifecycle_test import build_host_harness, run_menu, write_exec  # noqa: E402

FAKE_UI = r'''#!/usr/bin/env python3
import os, sys
log = open(os.environ["FAKE_UI_LOG"], "a")
answers = [a for a in os.environ.get("FAKE_UI_ANSWERS", "").split(",") if a]
if os.environ.get("FAKE_UI_BROKEN"):
    sys.exit(1)
def reply(text):
    sys.stdout.write(text + "\n")
    sys.stdout.flush()
for line in sys.stdin:
    line = line.rstrip("\n")
    log.write(line + "\n")
    log.flush()
    cmd = line.split("\t")[0]
    if cmd == "hello":
        reply("ready 1")
    elif cmd in ("end", "confirm"):
        if not answers:
            # Out of answers: like the user closing the window.
            sys.exit(0)
        reply(answers.pop(0))
    elif cmd in ("message", "textview"):
        reply("ok")
    elif cmd == "quit":
        log.write("<exit>\n")
        sys.exit(0)
'''


def harness(root: Path, failing=frozenset()) -> Path:
    host_dir = build_host_harness(root, failing)
    write_exec(host_dir / "internal" / "dualdeck-host-ui", FAKE_UI)
    return host_dir


def run_with_ui(host_dir: Path, answers: list[str], extra: dict | None = None):
    log = host_dir.parent / "ui.log"
    if log.exists():
        log.unlink()
    log.touch()
    env = {"DISPLAY": ":99", "FAKE_UI_LOG": str(log), "FAKE_UI_ANSWERS": ",".join(answers)}
    if extra:
        env.update(extra)
    # No terminal choices: anything that falls through to the terminal
    # menu reads EOF and exits, which the checks below would notice.
    proc = run_menu(host_dir, "dualdeck-host.sh", [], env)
    return proc, log.read_text().splitlines()


def run() -> int:
    failures = []

    def check(condition: bool, message: str):
        print(f"[{'ok' if condition else 'FAIL'}] {message}")
        if not condition:
            failures.append(message)

    with tempfile.TemporaryDirectory() as tmp:
        host_dir = harness(Path(tmp) / "a")
        proc, log = run_with_ui(host_dir, ["update", "cancel"])
        check(proc.returncode == 0, f"exits cleanly after Exit (rc={proc.returncode}, stderr={proc.stderr[-300:]!r})")
        check(log[:1] == ["hello"], "says hello first")
        check(any(line.startswith("menu\thome\tDualDeck Host") for line in log), "sends the home menu")
        keys = [line.split("\t")[1] for line in log if line.startswith("item\t")]
        check({"launch", "hostcontrol-daemon", "steam-add", "update", "reconfigure-controls",
               "emudeck", "advanced", "steam-remove"} <= set(keys),
              "home menu has every action the terminal menu has")
        check("busy\tChecking for updates…" in log, "shows a spinner while checking for updates")
        check(any(line.startswith("message\t\tDualDeck 0.1.0") for line in log), "shows the update report")
        check(sum(1 for line in log if line.startswith("menu\thome")) == 2, "returns to the home menu after the action")
        check(log[-2:] == ["quit", "<exit>"], "closes the window when the script ends")
        check("9) Exit" not in proc.stderr, "never shows the terminal menu")

    with tempfile.TemporaryDirectory() as tmp:
        host_dir = harness(Path(tmp) / "b")
        proc, log = run_with_ui(host_dir, ["launch", "ds"])
        check("stub: launch-host.sh called" in proc.stderr, "Play > DS launches melonDS")
        check("<exit>" in log and log.index("quit") > max(i for i, line in enumerate(log) if line == "end"),
              "closes the window before handing over to the emulator")
        check(any(line.startswith("menu\tcards\tChoose a system") for line in log), "system picker is the card screen")

    with tempfile.TemporaryDirectory() as tmp:
        host_dir = harness(Path(tmp) / "c", failing={"install-host-control-daemon.sh"})
        proc, log = run_with_ui(host_dir, ["hostcontrol-daemon", "start", "cancel"])
        messages = [line for line in log if line.startswith("message\t")]
        check(len(messages) == 1 and "\\n\\nstub: install-host-control-daemon.sh simulated failure" in messages[0],
              "multi-line output reaches the window escaped on one line")
        check(proc.returncode == 0, "a failed action returns to the menu and exits cleanly")

    with tempfile.TemporaryDirectory() as tmp:
        host_dir = harness(Path(tmp) / "d")
        proc, log = run_with_ui(host_dir, ["steam-remove", "no", "cancel"])
        confirms = [line.split("\t") for line in log if line.startswith("confirm\t")]
        check(len(confirms) == 1 and confirms[0][1] == "Remove DualDeck Host?" and confirms[0][3:6] == ["Remove", "Keep", "1"],
              "uninstall asks with a titled, danger-styled confirm")
        check("uninstall-steam-shortcut.sh called" not in proc.stderr, "answering no leaves everything installed")

    with tempfile.TemporaryDirectory() as tmp:
        host_dir = harness(Path(tmp) / "e")
        # Window closed while the Advanced menu is up.
        proc, log = run_with_ui(host_dir, ["advanced"])
        check(proc.returncode == 0, f"closing the window ends the script (rc={proc.returncode})")
        check("9) Exit" not in proc.stderr, "and doesn't fall back to the terminal menu")

    with tempfile.TemporaryDirectory() as tmp:
        host_dir = harness(Path(tmp) / "f")
        proc, log = run_with_ui(host_dir, [], {"FAKE_UI_BROKEN": "1"})
        check("9) Exit" in proc.stderr, "falls back to the terminal menu when the window can't start")
        check(proc.returncode == 0, "and that menu still exits cleanly")

    with tempfile.TemporaryDirectory() as tmp:
        host_dir = harness(Path(tmp) / "g")
        proc, log = run_with_ui(host_dir, [], {"DUALDECK_HOST_UI": "0"})
        check(log == [] and "9) Exit" in proc.stderr, "DUALDECK_HOST_UI=0 skips the window")

    print()
    if failures:
        print(f"{len(failures)} check(s) failed")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(run())
