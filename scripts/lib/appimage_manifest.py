#!/usr/bin/env python3
"""Sidecar manifest for an EmuDeck AppImage DualDeck has replaced in place.

Written next to the installed AppImage (e.g. next to
~/Applications/citra-qt.AppImage, this writes
~/Applications/citra-qt.AppImage.dualdeck.json) by
scripts/emudeck-replace-in-place.sh after a successful install, and read by
scripts/emudeck-check-drift.sh to detect when EmuDeck's own "Manage
Emulators" updater has silently replaced the file with a newer stock build
out from under DualDeck's patched one.

A sidecar file (not metadata embedded inside the AppImage itself) is the
right shape here: EmuDeck's updater does a straight file replace, so only a
file *outside* the one being replaced can survive that update to notice it
happened. This can only ever detect drift after the fact -- if EmuDeck's
updater runs and the user launches the game before the next drift check,
they transiently get an unpatched emulator with no remote-server
capability. That's an accepted, documented degrade-to-stock behavior, not a
crash -- see docs/known-limitations.md's Phase A entry.

Both scripts/emudeck-replace-in-place.sh and scripts/emudeck-check-drift.sh
(bash) invoke this file's CLI via subprocess rather than reimplementing
sha256/JSON handling in bash, matching this project's existing convention
of doing structured-data work in Python and process/file orchestration in
bash (see scripts/lib/steam_shortcut.py for the same division of labor on
the Steam-shortcut side).
"""
import argparse
import hashlib
import json
import sys
import time
from pathlib import Path


def manifest_path(appimage_path: Path) -> Path:
    return appimage_path.with_name(appimage_path.name + ".dualdeck.json")


def backup_path(appimage_path: Path) -> Path:
    # Deliberately does NOT end in ".AppImage" -- scripts/lib/
    # emudeck_paths.sh's newest-matching-AppImage glob only ever matches
    # files ending in exactly ".AppImage", so a backup named this way can
    # never accidentally be picked back up as "the newest install" by that
    # detection logic.
    return appimage_path.with_name(appimage_path.name + ".dualdeck-original")


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def write_manifest(appimage_path: Path, dualdeck_version: str, original_sha256: str) -> None:
    """Called once, right after a successful replace-in-place install --
    patched_sha256 is computed from the just-installed file itself (it's
    already sitting at appimage_path by the time this runs), so the caller
    only needs to supply the ORIGINAL file's hash (computed before install,
    since by definition it no longer exists at this path afterward)."""
    data = {
        "dualdeck_version": dualdeck_version,
        "patched_sha256": sha256_of(appimage_path),
        "original_sha256": original_sha256,
        "original_backup_path": str(backup_path(appimage_path)),
        "installed_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }
    manifest_path(appimage_path).write_text(json.dumps(data, indent=2) + "\n")


def read_manifest(appimage_path: Path) -> dict | None:
    path = manifest_path(appimage_path)
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text())
    except (json.JSONDecodeError, OSError):
        return None


# Drift-check outcomes, returned by check_drift() as a plain string so the
# calling shell script can branch on it without needing to parse JSON:
#   "matches_patched" -- up to date, exactly the binary DualDeck installed.
#   "matches_original" -- the stock binary is back (e.g. the user restored
#       it, or ran EmuDeck's updater and it happens to have produced a
#       build with the identical hash -- rare but not impossible).
#   "drifted" -- live file matches neither known hash. EmuDeck's updater
#       almost certainly replaced it with a newer stock build DualDeck has
#       never seen; needs re-patching to regain remote-server capability.
#   "no_manifest" -- DualDeck never installed anything at this path (or the
#       manifest was deleted/corrupted); not an error, just "nothing to
#       compare against."
#   "missing" -- the AppImage itself is gone.
def check_drift(appimage_path: Path) -> str:
    if not appimage_path.is_file():
        return "missing"
    manifest = read_manifest(appimage_path)
    if manifest is None:
        return "no_manifest"
    live_sha256 = sha256_of(appimage_path)
    if live_sha256 == manifest.get("patched_sha256"):
        return "matches_patched"
    if live_sha256 == manifest.get("original_sha256"):
        return "matches_original"
    return "drifted"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="action", required=True)

    write_p = sub.add_parser("write", help="Write a manifest after installing a patched AppImage")
    write_p.add_argument("appimage_path", type=Path)
    write_p.add_argument("--dualdeck-version", required=True)
    write_p.add_argument("--original-sha256", required=True,
                          help="sha256 of the original (stock) AppImage, computed before it was replaced")

    check_p = sub.add_parser("check", help="Check an installed AppImage for drift; prints the outcome")
    check_p.add_argument("appimage_path", type=Path)

    args = parser.parse_args()

    if args.action == "write":
        write_manifest(args.appimage_path, args.dualdeck_version, args.original_sha256)
        print(f"wrote {manifest_path(args.appimage_path)}")
        return 0

    if args.action == "check":
        outcome = check_drift(args.appimage_path)
        print(outcome)
        return 0

    return 1  # unreachable, argparse enforces a valid subcommand


if __name__ == "__main__":
    sys.exit(main())
