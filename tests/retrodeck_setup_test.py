#!/usr/bin/env python3
"""Regression test for scripts/retrodeck-setup.sh -- the script that
applies/removes the three flatpak override --user grants (plus a
~/.local/bin symlink per emulator) confirmed on real hardware to make
RetroDECK launch DualDeck's patched Cemu, melonDS, and Azahar, through
its own game list, the CLI, and Tender alike. See
docs/retrodeck-compatibility.md's "Real, confirmed blockers" section and
docs/known-limitations.md's 2026-08-29 entries for the investigation
this script distills.

Uses a fake `flatpak` fixture on PATH (no real Flatpak/RetroDECK
install needed) that only answers the exact calls this script is
expected to make, logging each one for the test to inspect -- an
unexpected call fails loudly rather than silently succeeding.

Covers:
  - apply: requires flatpak on PATH and RetroDECK "installed" (fake
    `flatpak info` success), fails clearly otherwise
  - apply: requires at least one patched AppImage to already exist,
    fails clearly (and points at emudeck-replace-in-place.sh) otherwise
  - apply: with no --emulator given, creates a symlink for every
    installed emulator (skipping, not failing on, one with no AppImage
    installed) and applies exactly the three documented overrides once
  - apply: --emulator (repeatable) restricts to a subset; an unknown
    name is rejected before anything runs
  - apply: covering melonds prints the required manual "switch DS to
    melonDS (Standalone) in RetroDECK itself" reminder; covering only
    cemu/azahar does not
  - --dry-run: makes no filesystem changes and issues no real flatpak
    override calls
  - --status: reports each emulator's AppImage/symlink state without
    changing anything
  - --restore: removes every emulator's symlink and issues a full
    `flatpak override --user --reset` -- not targeted --unset-env/
    --nofilesystem flags, which real-hardware testing (2026-08-29) found
    could leave RetroDECK unable to launch at all instead of cleanly
    reverting to defaults

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

# emulator -> (AppImage basename, expected systempath symlink name)
EMULATORS = {
    "cemu": ("Cemu.AppImage", "cemu"),
    "melonds": ("melonDS.AppImage", "melonds"),
    "azahar": ("azahar.AppImage", "azahar"),
}

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

    def appimage_path(self, emulator: str) -> Path:
        basename, _ = EMULATORS[emulator]
        return self.home / "Applications" / basename

    def install_appimage(self, emulator: str):
        path = self.appimage_path(emulator)
        path.write_text("fake appimage")
        path.chmod(path.stat().st_mode | stat.S_IEXEC)

    def symlink(self, emulator: str) -> Path:
        _, systempath_name = EMULATORS[emulator]
        return self.home / ".local" / "bin" / systempath_name

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


def test_apply_requires_flatpak_installed():
    with tempfile.TemporaryDirectory() as tmp:
        fx = Fixture(Path(tmp))
        fx.install_appimage("cemu")
        fx.not_installed_marker.touch()
        result = fx.run()
        check(result.returncode != 0, "apply fails when RetroDECK is not installed")
        check("not installed" in result.stderr, "apply's error mentions RetroDECK is not installed")
        check(not fx.log_lines(), "apply makes no flatpak override calls when RetroDECK isn't installed")


def test_apply_requires_at_least_one_appimage():
    with tempfile.TemporaryDirectory() as tmp:
        fx = Fixture(Path(tmp))
        # Deliberately not installing any emulator's AppImage
        result = fx.run()
        check(result.returncode != 0, "apply fails when no patched AppImage is installed for any emulator")
        check("emudeck-replace-in-place.sh" in result.stderr, "apply's error points at emudeck-replace-in-place.sh")
        check(not fx.log_lines(), "apply makes no flatpak override calls when nothing is installed")


def test_apply_with_no_emulator_flag_covers_everything_installed():
    with tempfile.TemporaryDirectory() as tmp:
        fx = Fixture(Path(tmp))
        fx.install_appimage("cemu")
        fx.install_appimage("azahar")
        # melonds deliberately left uninstalled -- should be skipped, not fail the whole run
        result = fx.run()
        check(result.returncode == 0, f"apply succeeds when at least one emulator is installed (stderr: {result.stderr})")

        for emulator in ("cemu", "azahar"):
            symlink = fx.symlink(emulator)
            check(symlink.is_symlink(), f"apply creates the {emulator} symlink")
            check(os.path.realpath(symlink) == os.path.realpath(fx.appimage_path(emulator)),
                  f"the {emulator} symlink points at its real patched AppImage")
        check(not fx.symlink("melonds").exists(), "apply skips melonds when its AppImage isn't installed")
        check("skipping" in result.stdout and "melonDS" in result.stdout,
              "apply reports skipping melonDS by name rather than silently ignoring it")

        calls = fx.log_lines()
        check(any("--filesystem=xdg-run/dualdeck:create" in c for c in calls), "apply grants --filesystem=xdg-run/dualdeck:create")
        check(any("--env=APPIMAGE_EXTRACT_AND_RUN=1" in c for c in calls), "apply grants --env=APPIMAGE_EXTRACT_AND_RUN=1")
        check(any("--env=PATH=" in c and str(fx.home / ".local" / "bin") in c for c in calls), "apply grants a PATH override that includes ~/.local/bin")
        check(all("net.retrodeck.retrodeck" in c for c in calls), "every override call targets net.retrodeck.retrodeck")
        # The three overrides are sandbox-wide, applied once regardless of
        # how many emulators were covered -- not once per emulator.
        override_calls = [c for c in calls if "--filesystem=xdg-run/dualdeck:create" in c]
        check(len(override_calls) == 1, "the shared overrides are applied exactly once, not once per emulator")


def test_apply_emulator_flag_restricts_to_a_subset():
    with tempfile.TemporaryDirectory() as tmp:
        fx = Fixture(Path(tmp))
        fx.install_appimage("cemu")
        fx.install_appimage("azahar")
        result = fx.run("--emulator", "azahar")
        check(result.returncode == 0, f"apply --emulator azahar succeeds (stderr: {result.stderr})")
        check(fx.symlink("azahar").is_symlink(), "--emulator azahar creates the azahar symlink")
        check(not fx.symlink("cemu").exists(), "--emulator azahar does not touch cemu, even though it's installed")


def test_apply_rejects_unknown_emulator_name():
    with tempfile.TemporaryDirectory() as tmp:
        fx = Fixture(Path(tmp))
        fx.install_appimage("cemu")
        result = fx.run("--emulator", "bogus")
        check(result.returncode != 0, "apply rejects an unknown --emulator name")
        check("cemu melonds azahar" in result.stderr or "melonds azahar cemu" in result.stderr.replace("\n", " ") or "must be one of" in result.stderr,
              "the error names the valid emulator choices")
        check(not fx.log_lines(), "apply makes no flatpak override calls for an invalid --emulator name")


def test_apply_covering_melonds_prints_the_ds_manual_step_reminder():
    with tempfile.TemporaryDirectory() as tmp:
        fx = Fixture(Path(tmp))
        fx.install_appimage("melonds")
        result = fx.run("--emulator", "melonds")
        check(result.returncode == 0, f"apply --emulator melonds succeeds (stderr: {result.stderr})")
        check("melonDS (Standalone)" in result.stdout,
              "apply reminds the user to manually switch RetroDECK's DS system to melonDS (Standalone)")


def test_apply_covering_only_cemu_does_not_print_the_ds_reminder():
    with tempfile.TemporaryDirectory() as tmp:
        fx = Fixture(Path(tmp))
        fx.install_appimage("cemu")
        result = fx.run("--emulator", "cemu")
        check(result.returncode == 0, f"apply --emulator cemu succeeds (stderr: {result.stderr})")
        check("melonDS (Standalone)" not in result.stdout,
              "apply does not print the DS-specific reminder when melonds isn't in scope")


def test_dry_run_makes_no_changes():
    with tempfile.TemporaryDirectory() as tmp:
        fx = Fixture(Path(tmp))
        fx.install_appimage("cemu")
        result = fx.run("--dry-run")
        check(result.returncode == 0, "--dry-run exits 0")
        check(not fx.symlink("cemu").exists(), "--dry-run does not create the symlink")
        check(not fx.log_lines(), "--dry-run issues no real flatpak override calls")
        check("would run:" in result.stdout, "--dry-run reports what it would have done")


def test_status_reports_every_emulator_without_changing_anything():
    with tempfile.TemporaryDirectory() as tmp:
        fx = Fixture(Path(tmp))
        fx.install_appimage("cemu")
        result = fx.run("--status")
        check(result.returncode == 0, "--status exits 0")
        for emulator in EMULATORS:
            check(emulator != "cemu" or "present and executable" in result.stdout,
                  "--status reports cemu's AppImage as present")
        check("(missing)" in result.stdout, "--status reports at least one symlink as missing before apply")
        check(not fx.symlink("cemu").exists(), "--status does not create any symlink")
        check("melonDS (Standalone)" in result.stdout,
              "--status also surfaces the DS manual-step note so it isn't only shown once, at apply time")


def test_restore_removes_every_symlink_via_full_reset():
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
        fx.install_appimage("cemu")
        fx.install_appimage("melonds")
        fx.install_appimage("azahar")
        apply_result = fx.run()
        check(apply_result.returncode == 0, "apply succeeds before testing restore")
        fx.log.write_text("")  # clear the log so restore's own calls are isolated

        result = fx.run("--restore")
        check(result.returncode == 0, "--restore exits 0")
        for emulator in EMULATORS:
            check(not fx.symlink(emulator).exists(), f"--restore removes the {emulator} symlink")

        calls = fx.log_lines()
        check(any("--reset" in c for c in calls), "--restore uses a full flatpak override --reset (confirmed safe on real hardware; targeted --unset-env broke RetroDECK entirely)")
        check(not any("--unset-env" in c for c in calls), "--restore no longer uses --unset-env, which was found to leave RetroDECK unable to launch")


if __name__ == "__main__":
    test_apply_requires_flatpak_installed()
    test_apply_requires_at_least_one_appimage()
    test_apply_with_no_emulator_flag_covers_everything_installed()
    test_apply_emulator_flag_restricts_to_a_subset()
    test_apply_rejects_unknown_emulator_name()
    test_apply_covering_melonds_prints_the_ds_manual_step_reminder()
    test_apply_covering_only_cemu_does_not_print_the_ds_reminder()
    test_dry_run_makes_no_changes()
    test_status_reports_every_emulator_without_changing_anything()
    test_restore_removes_every_symlink_via_full_reset()

    if failures:
        print(f"\n{len(failures)} failure(s)")
        sys.exit(1)
    print("\nAll checks passed")
