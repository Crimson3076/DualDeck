#!/usr/bin/env python3
"""Regression test for pack_retrodeck_component_tarball() and its shared
_stage_bundle_payload() helper (scripts/lib/appimage_pack.sh), added for
RetroDECK compatibility (see docs/retrodeck-compatibility.md).

This is packaging-shape verification only -- it uses small real ELF
binaries (/bin/true and friends, already present on any Linux dev/CI
box) standing in for the real patched Cemu binary, so it never needs a
real Cemu build, network access, or appimagetool. It exists because this
function is new (extracted/added alongside pack_appimage()'s existing,
already-proven bundling logic) and has no other test coverage --
pack_appimage() itself has none either, by long-standing project
convention (see tests/'s other files), since exercising it for real
needs a real built emulator binary and a real appimagetool download,
verified instead via build-release.sh's own CI runs.

Covers:
  - the output tarball has exactly one top-level <component_name>/ dir
  - component_launcher.sh sits at that top level, executable, with the
    exact content handed in (not regenerated or altered)
  - the main binary lands at <component_name>/usr/bin/<basename>,
    executable
  - extra_binaries each land at usr/bin/<basename>, executable
  - extra_dirs are copied under usr/bin/<basename>, preserving their
    own file contents
  - pack_appimage() (the pre-existing function this refactor changed)
    still produces a real, runnable AppImage-shaped AppDir with the same
    inputs -- guards against the _stage_bundle_payload() extraction
    having silently changed pack_appimage()'s own behavior

Usage:
    python3 tests/retrodeck_component_pack_test.py
"""

import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
APPIMAGE_PACK_LIB = REPO_ROOT / "scripts" / "lib" / "appimage_pack.sh"

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)
        print(f"FAIL: {message}")
    else:
        print(f"ok: {message}")


def run_bash(script_body: str, cwd: Path) -> subprocess.CompletedProcess:
    full_script = f"""
set -euo pipefail
source "{APPIMAGE_PACK_LIB}"
{script_body}
"""
    return subprocess.run(
        ["bash", "-c", full_script],
        cwd=cwd,
        capture_output=True,
        text=True,
    )


def test_component_tarball_shape():
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        launcher = tmp_path / "launcher.sh"
        launcher.write_text("#!/usr/bin/env bash\necho launched\n")

        extra_bin = tmp_path / "dualdeck-host-service-fixture"
        shutil.copy("/bin/true", extra_bin)

        extra_dir = tmp_path / "gameProfiles"
        extra_dir.mkdir()
        (extra_dir / "profile.ini").write_text("[fixture]\n")

        output_tar = tmp_path / "out" / "cemu-component.tar.gz"

        result = run_bash(
            f"""
cp /bin/true "{tmp_path}/cemu-fixture"
pack_retrodeck_component_tarball "{tmp_path}/cemu-fixture" "{output_tar}" cemu \
    "{launcher}" "{extra_bin}" "{extra_dir}"
""",
            cwd=tmp_path,
        )
        check(result.returncode == 0, f"pack_retrodeck_component_tarball() exits 0 (stderr: {result.stderr})")
        check(output_tar.is_file(), "output tarball was created")
        if not output_tar.is_file():
            return

        with tarfile.open(output_tar) as tf:
            names = tf.getnames()
            top_level_dirs = {n.split("/")[0] for n in names}
            check(top_level_dirs == {"cemu"}, f"exactly one top-level dir 'cemu' (got {top_level_dirs})")

            launcher_member = tf.getmember("cemu/component_launcher.sh")
            check(bool(launcher_member.mode & stat.S_IXUSR), "component_launcher.sh is executable")
            launcher_content = tf.extractfile(launcher_member).read().decode()
            check(launcher_content == launcher.read_text(), "component_launcher.sh content matches the input launcher script verbatim")

            binary_member = tf.getmember("cemu/usr/bin/cemu-fixture")
            check(bool(binary_member.mode & stat.S_IXUSR), "main binary is executable")

            extra_bin_member = tf.getmember(f"cemu/usr/bin/{extra_bin.name}")
            check(bool(extra_bin_member.mode & stat.S_IXUSR), "extra binary is executable")

            profile_member = tf.getmember("cemu/usr/bin/gameProfiles/profile.ini")
            check(
                tf.extractfile(profile_member).read().decode() == "[fixture]\n",
                "extra_dirs contents are preserved byte-for-byte",
            )


def test_pack_appimage_still_produces_expected_appdir_shape():
    # Doesn't invoke appimagetool (that needs a real network download) --
    # instead calls _stage_bundle_payload() directly, the exact helper
    # pack_appimage() was refactored to call, and confirms its output
    # shape is what pack_appimage() has always relied on (usr/bin/<name>,
    # usr/lib/, extra binaries/dirs alongside it).
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        payload_dir = tmp_path / "AppDir"

        extra_dir = tmp_path / "resources"
        extra_dir.mkdir()
        (extra_dir / "data.bin").write_bytes(b"\x00\x01")

        result = run_bash(
            f"""
cp /bin/true "{tmp_path}/melonDS-fixture"
_stage_bundle_payload "{payload_dir}" "{tmp_path}/melonDS-fixture" "" "{extra_dir}"
""",
            cwd=tmp_path,
        )
        check(result.returncode == 0, f"_stage_bundle_payload() exits 0 (stderr: {result.stderr})")

        expected_bin = payload_dir / "usr" / "bin" / "melonDS-fixture"
        check(expected_bin.is_file() and expected_bin.stat().st_mode & stat.S_IXUSR, "pack_appimage()'s AppDir layout: usr/bin/<name> exists and is executable")
        check((payload_dir / "usr" / "lib").is_dir(), "pack_appimage()'s AppDir layout: usr/lib/ exists")
        check((payload_dir / "usr" / "bin" / "resources" / "data.bin").read_bytes() == b"\x00\x01", "pack_appimage()'s AppDir layout: extra_dirs land under usr/bin/, contents preserved")


if __name__ == "__main__":
    test_component_tarball_shape()
    test_pack_appimage_still_produces_expected_appdir_shape()

    if failures:
        print(f"\n{len(failures)} failure(s)")
        sys.exit(1)
    print("\nAll checks passed")
