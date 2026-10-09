#!/usr/bin/env python3
"""Runs the host and client "Check for updates" installers
(packaging/{host,client}/internal/apply-update.sh) against a fake GitHub:
a real, checksummed fixture release served by the fake `curl` from
dualdeck_branch_selector_test.py.

For each side:
  1. A release whose archive matches SHA256SUMS is installed (the
     release's own install-steam-shortcut.sh stub runs with --force).
  2. A release whose SHA256SUMS doesn't match is refused before anything
     from it runs.

Usage:
    python3 tests/apply_update_test.py
"""

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from dualdeck_branch_selector_test import (  # noqa: E402
    PACKAGING,
    REPO,
    REPO_ROOT,
    build_fake_curl,
    build_fixture_release,
    write_exec,
)

LATEST = f"https://github.com/{REPO}/releases/latest/download"


def run_update(tmp: Path, side: str, corrupt_sums: bool) -> subprocess.CompletedProcess:
    fixture_dir = tmp / "fixture"
    fixture_dir.mkdir()
    build_fixture_release(fixture_dir, "v0.1.60")
    if corrupt_sums:
        (fixture_dir / "SHA256SUMS").write_text("0" * 64 + "  melonds-remote-linux-x86_64.tar.gz\n")

    internal = tmp / "pkg" / side / "internal"
    internal.mkdir(parents=True)
    write_exec(internal / "apply-update.sh", (PACKAGING / side / "internal" / "apply-update.sh").read_text())
    shutil.copy(REPO_ROOT / "scripts" / "lib" / "release_install.sh", internal / "release_install.sh")

    bin_dir = tmp / "bin"
    bin_dir.mkdir()
    build_fake_curl(bin_dir, {
        f"{LATEST}/melonds-remote-linux-x86_64.tar.gz": fixture_dir / "melonds-remote-linux-x86_64.tar.gz",
        f"{LATEST}/SHA256SUMS": fixture_dir / "SHA256SUMS",
    })
    home = tmp / "home"
    home.mkdir()
    return subprocess.run([str(internal / "apply-update.sh")], capture_output=True, text=True, timeout=30,
                          env={"PATH": f"{bin_dir}:/usr/bin:/bin", "HOME": str(home)})


def run() -> int:
    failures = []

    def check(condition: bool, message: str, proc: subprocess.CompletedProcess):
        if condition:
            print(f"[ok] {message}")
        else:
            print(f"[FAIL] {message}\n--- stdout ---\n{proc.stdout}\n--- stderr ---\n{proc.stderr}")
            failures.append(message)

    for side in ("host", "client"):
        with tempfile.TemporaryDirectory() as tmp:
            proc = run_update(Path(tmp), side, corrupt_sums=False)
            check(proc.returncode == 0 and "fixture install-steam-shortcut.sh --force: --force" in proc.stderr,
                  f"{side}: a release matching SHA256SUMS is installed", proc)

        with tempfile.TemporaryDirectory() as tmp:
            proc = run_update(Path(tmp), side, corrupt_sums=True)
            check(proc.returncode != 0 and "checksum verification failed" in proc.stderr
                  and "fixture install-steam-shortcut.sh" not in proc.stderr,
                  f"{side}: a release that fails SHA256SUMS is refused before anything runs", proc)

    if failures:
        print(f"\n{len(failures)} check(s) failed")
        return 1
    print("\nAPPLY-UPDATE TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(run())
