#!/usr/bin/env python3
"""Drives the real dualdeck-host-ui window (SDL's dummy video driver, no
display needed) over its stdin protocol and checks what it answers:
menu navigation on each screen style, confirm/message dialogs, Back, and
exiting when the script goes away. Optionally renders every screen to PNG
for reviewing UI changes.

Usage:
    python3 tests/host_ui_window_test.py build/host/ui/dualdeck-host-ui [preview-dir]
"""

import os
import select
import subprocess
import sys
from pathlib import Path

HOME = [
    "header\tv0.1.999",
    "menu\thome\tDualDeck Host\t",
    "status\ton\tA Deck can connect any time, even with no emulator open.",
    "item\tlaunch\tPlay\tLaunch an emulator and stream it to your Deck. The Deck switches to the game on its own.\t\t\t\tDS|melonDS,3DS|Azahar,WII U|Cemu,DESKTOP",
    "item\thostcontrol-daemon\tAlways-on Host Control\tStarts at login, so a Deck can use this PC any time\tmonitor\t\ton\t",
    "item\tsteam-add\tAdd to Steam\tShows DualDeck Host in Gaming Mode and Big Picture\tplus\t\t\t",
    "item\tupdate\tCheck for updates\tDownload the newest DualDeck release\trefresh\t\t\t",
    "item\treconfigure-controls\tFix Cemu controls\tWhen Deck input does nothing in Cemu\tgamepad\t\t\t",
    "item\temudeck\tUse my EmuDeck / RetroDECK emulators\tStream the emulators you already have installed\tlist\tExperimental\twarn\t",
    "item\tadvanced\tAdvanced\tInstallation branch and other options\tgear\t\t\t",
    "item\tsteam-remove\tRemove from Steam and uninstall\tYour ROMs, saves and firmware are never touched\ttrash\t\tdanger\t",
    "end",
]
CARDS = [
    "menu\tcards\tChoose a system\tThe Deck connects as soon as the emulator is up.",
    "item\tds\tDS\tmelonDS, with both screens and the touch screen on the Deck\tds\t\t\t",
    "item\tn3ds\t3DS\tAzahar\t3ds\tExperimental\twarn\t",
    "item\twiiu\tWii U\tCemu, with the GamePad screen on the Deck\twiiu\tExperimental\twarn\t",
    "item\thostcontrol\tDesktop\tHost Control only: use this PC from the Deck, no emulator\tdesktop\tExperimental\twarn\t",
    "item\tcustom\tCustom\tPatch and run my own emulator build\tcustom\t\t\t",
    "end",
]
LIST = [
    "menu\tlist\tAdvanced\tInstallation branch: main (installed: 6d246d9, v0.1.183)",
    "item\tchange-branch\tChange installation branch\tChoose which GitHub branch updates come from\tbranch\t\t\t",
    "item\trefresh-branches\tRefresh branch list\tFetch the latest branches from GitHub\trefresh\t\t\t",
    "item\tinstall-branch\tInstall selected branch\tDownload and install the branch you picked\tdownload\t\t\t",
    "end",
]
CONFIRM = "confirm\tRemove DualDeck Host?\tThis removes the Steam shortcut, the installed files, and the Distrobox container if one was created. Your ROMs, saves, and firmware are never touched.\tRemove\tKeep\t1"
MESSAGE = "message\t\tAdded DualDeck Host to Steam. Restart Steam (or switch to Gaming Mode) to see it, and set its Controller Layout to a plain Gamepad template once it's there."
TEXTVIEW = "textview\tHost Control service status\t" + "\\n".join(
    ["● dualdeck-host-control.service - DualDeck Host Control",
     "     Loaded: loaded (/home/deck/.config/systemd/user/dualdeck-host-control.service; enabled)",
     "     Active: active (running) since Fri 2026-10-09 18:02:11 BST; 14min ago",
     "   Main PID: 4412 (dualdeck-host-s)",
     "      Tasks: 6 (limit: 18432)",
     "     Memory: 21.4M",
     "Oct 09 18:02:11 bazzite systemd[1288]: Started DualDeck Host Control.",
     "Oct 09 18:02:11 bazzite dualdeck-host-service[4412]: listening on 0.0.0.0:47990"])
BUSY = "busy\tChecking for updates…"


class Window:
    def __init__(self, binary: str):
        env = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy")
        self.proc = subprocess.Popen([binary, "--windowed"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     text=True, env=env)

    def send(self, *lines: str):
        for line in lines:
            self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def read(self, timeout: float = 5.0) -> str | None:
        ready, _, _ = select.select([self.proc.stdout], [], [], timeout)
        if not ready:
            return None
        line = self.proc.stdout.readline()
        return line.rstrip("\n") if line else None


def run() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    failures = []

    def check(condition: bool, message: str):
        print(f"[{'ok' if condition else 'FAIL'}] {message}")
        if not condition:
            failures.append(message)

    w = Window(binary)
    w.send("hello")
    check(w.read() == "ready 1", "answers hello with its protocol version")

    w.send(*HOME, "press\ta")
    check(w.read() == "launch", "home: A on the focused hero picks Play")
    w.send(*HOME, "press\tright", "press\tdown", "press\tdown", "press\ta")
    check(w.read() == "update", "home: right into the list, down twice")
    w.send(*HOME, "press\tleft", "press\tright", "press\ta")
    check(w.read() == "update", "home: list position is remembered across the hero")
    w.send(*HOME, "press\tb")
    check(w.read() == "cancel", "home: B answers cancel")

    w.send(*CARDS, "press\tright", "press\tright", "press\ta")
    check(w.read() == "wiiu", "cards: left/right moves between systems")
    w.send(*LIST, "press\tdown", "press\tdown", "press\tdown", "press\ta")
    check(w.read() == "install-branch", "list: down stops at the last entry")

    w.send(CONFIRM, "press\ta")
    check(w.read() == "no", "danger confirm starts on the safe choice")
    w.send(CONFIRM, "press\tleft", "press\ta")
    check(w.read() == "yes", "confirm: left then A confirms")
    w.send(CONFIRM, "press\tleft", "press\tb")
    check(w.read() == "no", "confirm: B always declines")
    w.send(MESSAGE, "press\tb")
    check(w.read() == "ok", "message: any button closes it")
    w.send(BUSY, "press\ta", "press\tb")
    check(w.read(timeout=0.5) is None, "busy: ignores input and answers nothing")
    w.send(TEXTVIEW, "press\tdown", "press\ta")
    check(w.read() == "ok", "text view: A closes it")

    w.proc.stdin.close()
    try:
        w.proc.wait(timeout=5)
        check(w.proc.returncode == 0, "exits when the script closes the pipe")
    except subprocess.TimeoutExpired:
        w.proc.kill()
        check(False, "exits when the script closes the pipe")

    # Screenshots, for review (CI uploads them).
    if len(sys.argv) > 2:
        out = Path(sys.argv[2])
        out.mkdir(parents=True, exist_ok=True)
        shots = {
            "1-home": HOME,
            "2-home-list-focus": HOME + ["press\tright", "press\tdown"],
            "3-choose-system": ["header\tv0.1.999"] + CARDS,
            "4-advanced": ["header\tv0.1.999"] + LIST + ["press\tdown"],
            "5-confirm-uninstall": HOME + [CONFIRM],
            "6-message": HOME + ["press\tright", "press\tdown", "press\ta", MESSAGE],
            "7-busy": HOME + [BUSY],
            "8-service-status": HOME + ["press\tright", "press\ta", TEXTVIEW],
        }
        for name, lines in shots.items():
            for scale in ("1", "1.5"):
                suffix = "" if scale == "1" else "@1.5x"
                path = out / f"{name}{suffix}.png"
                proc = subprocess.run([binary, "--screenshot", str(path), "--scale", scale],
                                      input="\n".join(lines) + "\n", capture_output=True, text=True, timeout=30)
                check(proc.returncode == 0 and path.exists(), f"renders {path.name}")

    print()
    if failures:
        print(f"{len(failures)} check(s) failed")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(run())
