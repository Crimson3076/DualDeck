#!/usr/bin/env python3
"""Regression test for scripts/retrodeck-setup.sh -- the script that
applies/removes the three flatpak override --user grants (plus a
~/.local/bin/cemu symlink) confirmed on real hardware to make RetroDECK
launch DualDeck's patched Cemu, through its own game list, the CLI, and
Tender alike. See docs/retrodeck-compatibility.md's "Real, confirmed
blockers" section and docs/known-limitations.md's 2026-08-29 entries for
the investigation this script distills.

Uses a fake `flatpak` fixture on PATH (no real Flatpak/RetroDECK
install needed) that only answers the exact calls this script is
expected to make, logging each one for the test to inspect -- an
unexpected call fails loudly rather than silently succeeding.

Covers:
  - apply: requires flatpak on PATH and RetroDECK "installed" (fake
    `flatpak info` success), fails clearly otherwise
  - apply: requires the patched Cemu AppImage to already exist, fails
    clearly (and points at emudeck-replace-in-place.sh) otherwise
  - apply: creates the ~/.local/bin/cemu symlink pointing at the real
    AppImage, and applies exactly the three documented overrides, in a
    real subprocess (not just reading the script's source)
  - --dry-run: makes no filesystem changes and issues no real flatpak
    override calls
  - --status: reports the symlink and AppImage state without changing
    anything
  - --restore: removes the symlink and issues a full `flatpak override
    --user --reset` -- not targeted --unset-env/--nofilesystem flags,
    which real-hardware testing (2026-08-29) found could leave RetroDECK
    unable to launch at all instead of cleanly reverting to defaults

Usage:
    python3 tests/retrodeck_setup_test.py
"""

import os
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCRIPT = REPO_ROOT / "scripts" / "retrodeck-setup.sh"

FAKE_FLATPAK = """#!/usr/bin/env bash
case "$1 $2" in
  "info net.retrodeck.retrodeck")
    if [[ -f "$FAKE_RETRODECK_NOT_INSTALLED" ]]; then
      exit 1
    fi
    exit 0
    ;;
esac
if [[ "$1" == "override" ]]; then
  echo "$*" >> "$FAKE_FLATPAK_LOG"
  exit 0
fi
echo "unhandled fake flatpak call: $*" >&2
exit 1
"""

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)
        print(f"FAIL: {message}")
    else:
        print(f"ok: {message}")


class Fixture:
    def __init__(self, tmp: Path):
        self.tmp = tmp
        self.home = tmp / "home"
        self.bin = tmp / "bin"
        self.home.joinpath("Applications").mkdir(parents=True)
        self.bin.mkdir()
        self.log = tmp / "flatpak-calls.log"
        self.log.touch()
        self.not_installed_marker = tmp / "retrodeck-not-installed"

        flatpak = self.bin / "flatpak"
        flatpak.write_text(FAKE_FLATPAK)
        flatpak.chmod(flatpak.stat().st_mode | stat.S_IEXEC)

        self.cemu_appimage = self.home / "Applications" / "Cemu.AppImage"

    def install_cemu_appimage(self):
        self.cemu_appimage.write_text("fake appimage")
        self.cemu_appimage.chmod(self.cemu_appimage.stat().st_mode | stat.S_IEXEC)

    def run(self, *args):
        env = dict(os.environ)
        env["HOME"] = str(self.home)
        env["PATH"] = f"{self.bin}:{env['PATH']}"
        env["FAKE_FLATPAK_LOG"] = str(self.log)
        env["FAKE_RETRODECK_NOT_INSTALLED"] = str(self.not_installed_marker)
        return subprocess.run(
            ["bash", str(SCRIPT), *args],
            env=env,
            capture_output=True,
            text=True,
        )

    def log_lines(self):
        return [line for line in self.log.read_text().splitlines() if line]

    def cemu_symlink(self):
        return self.home / ".local" / "bin" / "cemu"


def test_apply_requires_flatpak_installed():
    with tempfile.TemporaryDirectory() as tmp:
        fx = Fixture(Path(tmp))
        fx.install_cemu_appimage()
        fx.not_installed_marker.touch()
        result = fx.run()
        check(result.returncode != 0, "apply fails when RetroDECK is not installed")
        check("not installed" in result.stderr, "apply's error mentions RetroDECK is not installed")
        check(not fx.log_lines(), "apply makes no flatpak override calls when RetroDECK isn't installed")


def test_apply_requires_cemu_appimage():
    with tempfile.TemporaryDirectory() as tmp:
        fx = Fixture(Path(tmp))
        # Deliberately not calling fx.install_cemu_appimage()
        result = fx.run()
        check(result.returncode != 0, "apply fails when the patched Cemu AppImage is missing")
        check("emudeck-replace-in-place.sh" in result.stderr, "apply's error points at emudeck-replace-in-place.sh")
        check(not fx.log_lines(), "apply makes no flatpak override calls when the AppImage is missing")


def test_apply_creates_symlink_and_three_overrides():
    with tempfile.TemporaryDirectory() as tmp:
        fx = Fixture(Path(tmp))
        fx.install_cemu_appimage()
        result = fx.run()
        check(result.returncode == 0, f"apply succeeds with RetroDECK installed and the AppImage present (stderr: {result.stderr})")

        symlink = fx.cemu_symlink()
        check(symlink.is_symlink(), "apply creates ~/.local/bin/cemu as a symlink")
        check(os.path.realpath(symlink) == os.path.realpath(fx.cemu_appimage), "the symlink points at the real patched AppImage")

        calls = fx.log_lines()
        check(any("--filesystem=xdg-run/dualdeck:create" in c for c in calls), "apply grants --filesystem=xdg-run/dualdeck:create")
        check(any("--env=APPIMAGE_EXTRACT_AND_RUN=1" in c for c in calls), "apply grants --env=APPIMAGE_EXTRACT_AND_RUN=1")
        check(any("--env=PATH=" in c and str(fx.home / ".local" / "bin") in c for c in calls), "apply grants a PATH override that includes ~/.local/bin")
        check(all("net.retrodeck.retrodeck" in c for c in calls), "every override call targets net.retrodeck.retrodeck")


def test_dry_run_makes_no_changes():
    with tempfile.TemporaryDirectory() as tmp:
        fx = Fixture(Path(tmp))
        fx.install_cemu_appimage()
        result = fx.run("--dry-run")
        check(result.returncode == 0, "--dry-run exits 0")
        check(not fx.cemu_symlink().exists(), "--dry-run does not create the symlink")
        check(not fx.log_lines(), "--dry-run issues no real flatpak override calls")
        check("would run:" in result.stdout, "--dry-run reports what it would have done")


def test_status_reports_without_changing_anything():
    with tempfile.TemporaryDirectory() as tmp:
        fx = Fixture(Path(tmp))
        fx.install_cemu_appimage()
        result = fx.run("--status")
        check(result.returncode == 0, "--status exits 0")
        check("(missing)" in result.stdout, "--status reports the symlink as missing before apply")
        check("present and executable" in result.stdout, "--status reports the AppImage as present")
        check(not fx.cemu_symlink().exists(), "--status does not create the symlink")


def test_restore_removes_symlink_via_full_reset():
    # Real hardware finding, 2026-08-29: targeted `--unset-env=PATH` +
    # `--nofilesystem=xdg-run/dualdeck` left RetroDECK unable to launch
    # at all, rather than cleanly reverting to its shipped defaults --
    # Flatpak's own override-removal semantics don't behave like a
    # simple "undo the earlier --env/--filesystem call" here. --restore
    # was switched to a full `flatpak override --user --reset` instead,
    # confirmed on real hardware to actually leave RetroDECK working
    # afterward -- blunter (clears every override for this app, not just
    # these three) but the one approach that's actually safe.
    with tempfile.TemporaryDirectory() as tmp:
        fx = Fixture(Path(tmp))
        fx.install_cemu_appimage()
        apply_result = fx.run()
        check(apply_result.returncode == 0, "apply succeeds before testing restore")
        fx.log.write_text("")  # clear the log so restore's own calls are isolated

        result = fx.run("--restore")
        check(result.returncode == 0, "--restore exits 0")
        check(not fx.cemu_symlink().exists(), "--restore removes the ~/.local/bin/cemu symlink")

        calls = fx.log_lines()
        check(any("--reset" in c for c in calls), "--restore uses a full flatpak override --reset (confirmed safe on real hardware; targeted --unset-env broke RetroDECK entirely)")
        check(not any("--unset-env" in c for c in calls), "--restore no longer uses --unset-env, which was found to leave RetroDECK unable to launch")


if __name__ == "__main__":
    test_apply_requires_flatpak_installed()
    test_apply_requires_cemu_appimage()
    test_apply_creates_symlink_and_three_overrides()
    test_dry_run_makes_no_changes()
    test_status_reports_without_changing_anything()
    test_restore_removes_symlink_via_full_reset()

    if failures:
        print(f"\n{len(failures)} failure(s)")
        sys.exit(1)
    print("\nAll checks passed")
