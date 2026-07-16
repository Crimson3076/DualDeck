#!/usr/bin/env python3
"""Adds melonds-remote-client as a Steam non-Steam-game shortcut, so it
shows up in Big Picture/Gaming Mode without the manual "Add a Non-Steam
Game" steps in docs/steam-deck-setup.md (see melonds-remote's GitHub
issue #5: "Make an easy way for users to launch from steam big picture
mode/game mode").

Edits Steam's binary shortcuts.vdf directly -- a minimal, dependency-free
implementation of the format (no `vdf` pip package needed, so this runs
on a stock SteamOS Desktop Mode Python with nothing extra installed).
The format itself is community-reverse-engineered, not officially
documented by Valve, so this is deliberately conservative:

- Refuses to run while Steam is running (it caches shortcuts.vdf in
  memory and can silently overwrite this script's change on its next
  save) unless --force is passed.
- Always backs up the existing file next to itself before writing.
- Parses its own freshly-written output back immediately as a sanity
  check, and restores the backup if that fails, rather than leaving a
  possibly-corrupt file in place.
- Idempotent: re-running with the same --exe updates that entry in place
  (matched by Exe path, or by --name as a fallback -- see below) instead
  of adding a duplicate.
- Matching falls back to --name (default "melonDS Remote") when Exe
  doesn't match anything, so a shortcut created by an older version of
  this project's install script -- pointing at some now-stale path, e.g.
  a deleted per-release download directory -- gets migrated in place
  instead of left behind as an orphaned duplicate.
- Pass --remove to undo this instead -- removes the entry matching --exe
  or --name (works even if that path no longer exists on disk) and is
  itself idempotent: running it again once nothing matches is a harmless
  no-op, not an error.
- Applies to every local Steam user by default rather than prompting to
  pick one -- a Steam Deck's controller-only input has no reliable way
  to type an answer to that kind of prompt (--user overrides this for
  scripted/single-user use, but is never required).

This only touches the shortcuts list -- it does not touch Steam Input
controller-layout assignments (a separate, per-shortcut config file
keyed by a derived ID), so docs/steam-deck-setup.md's manual "set
Controller Layout to Gamepad" step is still needed after this runs.
"""
import argparse
import glob
import os
import shutil
import struct
import subprocess
import sys
import time
import zlib


TYPE_MAP = 0x00
TYPE_STRING = 0x01
TYPE_INT = 0x02
TYPE_END = 0x08


def _read_cstring(data: bytes, pos: int) -> tuple[str, int]:
    end = data.index(b"\x00", pos)
    return data[pos:end].decode("utf-8", errors="replace"), end + 1


def parse_vdf_map(data: bytes, pos: int) -> tuple[dict, int]:
    """Parses one binary-VDF map starting just after its own TYPE_MAP byte
    (or at the very start, for the implicit root map). Returns an ordered
    dict of key -> value (str, int, or nested dict) and the position just
    past the map's closing TYPE_END byte."""
    result: dict = {}
    while True:
        if pos >= len(data):
            return result, pos
        marker = data[pos]
        pos += 1
        if marker == TYPE_END:
            return result, pos
        key, pos = _read_cstring(data, pos)
        if marker == TYPE_MAP:
            value, pos = parse_vdf_map(data, pos)
        elif marker == TYPE_STRING:
            value, pos = _read_cstring(data, pos)
        elif marker == TYPE_INT:
            value = struct.unpack_from("<i", data, pos)[0]
            pos += 4
        else:
            raise ValueError(f"unrecognized VDF marker 0x{marker:02x} at offset {pos - 1}")
        result[key] = value


def write_vdf_map(entries: dict) -> bytes:
    out = bytearray()
    for key, value in entries.items():
        if isinstance(value, dict):
            out.append(TYPE_MAP)
            out.extend(key.encode("utf-8"))
            out.append(0)
            out.extend(write_vdf_map(value))
        elif isinstance(value, str):
            out.append(TYPE_STRING)
            out.extend(key.encode("utf-8"))
            out.append(0)
            out.extend(value.encode("utf-8"))
            out.append(0)
        elif isinstance(value, int):
            out.append(TYPE_INT)
            out.extend(key.encode("utf-8"))
            out.append(0)
            out.extend(struct.pack("<i", value))
        else:
            raise TypeError(f"unsupported VDF value type for key {key!r}: {type(value)}")
    out.append(TYPE_END)
    return bytes(out)


def legacy_shortcut_appid(exe: str, appname: str) -> int:
    """Matches the convention used by Steam itself and community tools
    (e.g. Boilr) for non-Steam shortcuts: CRC32 of the exe+appname bytes,
    with the high bit set, stored as a signed 32-bit int."""
    data = (exe + appname).encode("utf-8")
    crc = zlib.crc32(data) & 0xFFFFFFFF
    legacy_id = crc | 0x80000000
    return struct.unpack("<i", struct.pack("<I", legacy_id))[0]


def find_userdata_dirs() -> list[str]:
    candidates = []
    for base in (
        os.path.expanduser("~/.local/share/Steam/userdata"),
        os.path.expanduser("~/.steam/steam/userdata"),
        os.path.expanduser("~/.steam/root/userdata"),
    ):
        if os.path.isdir(base):
            candidates.extend(sorted(glob.glob(os.path.join(base, "*"))))
    # De-duplicate (the three bases above are often symlinked to the same place).
    seen = set()
    result = []
    for c in candidates:
        real = os.path.realpath(c)
        if real not in seen and os.path.isdir(c):
            seen.add(real)
            result.append(c)
    return result


def steam_is_running() -> bool:
    try:
        out = subprocess.run(["pgrep", "-x", "steam"], capture_output=True, text=True)
        return out.returncode == 0
    except FileNotFoundError:
        return False  # no pgrep available -- can't check, caller decides what to do


def load_shortcuts(path: str) -> dict:
    if not os.path.isfile(path):
        return {"shortcuts": {}}
    with open(path, "rb") as f:
        data = f.read()
    root, _ = parse_vdf_map(data, 0)
    if "shortcuts" not in root:
        root["shortcuts"] = {}
    return root


def remove_shortcut(root: dict, exe: str, appname: str) -> bool:
    """Removes any shortcut entries whose Exe OR AppName matches, re-indexing
    the remaining entries to contiguous numeric keys (matches the convention
    upsert_shortcut relies on when picking the next free index). The AppName
    fallback catches entries created before this project switched to a fixed
    central install directory -- their Exe points at some old, possibly now-
    deleted per-release download path, so Exe alone would never match again.
    Returns True if anything was removed."""
    shortcuts = root["shortcuts"]

    def matches(entry: object) -> bool:
        return isinstance(entry, dict) and (entry.get("Exe") == exe or entry.get("AppName") == appname)

    remaining = [v for v in shortcuts.values() if not matches(v)]
    if len(remaining) == len(shortcuts):
        return False
    root["shortcuts"] = {str(i): entry for i, entry in enumerate(remaining)}
    return True


def upsert_shortcut(root: dict, appname: str, exe: str, startdir: str, launch_options: str) -> None:
    shortcuts = root["shortcuts"]
    for key, entry in shortcuts.items():
        # Falls back to matching by AppName alone (see remove_shortcut's
        # docstring) so a pre-existing entry from before this project used a
        # fixed central install directory gets migrated in place instead of
        # left behind as an orphaned duplicate -- hence Exe must be
        # overwritten here too, not just the other fields.
        if isinstance(entry, dict) and (entry.get("Exe") == exe or entry.get("AppName") == appname):
            entry["Exe"] = exe
            entry["AppName"] = appname
            entry["StartDir"] = startdir
            entry["LaunchOptions"] = launch_options
            entry["appid"] = legacy_shortcut_appid(exe, appname)
            return

    next_index = 0
    existing_indices = {int(k) for k in shortcuts.keys() if k.isdigit()}
    while next_index in existing_indices:
        next_index += 1

    shortcuts[str(next_index)] = {
        "appid": legacy_shortcut_appid(exe, appname),
        "AppName": appname,
        "Exe": exe,
        "StartDir": startdir,
        "icon": "",
        "ShortcutPath": "",
        "LaunchOptions": launch_options,
        "IsHidden": 0,
        "AllowDesktopConfig": 1,
        "AllowOverlay": 1,
        "OpenVR": 0,
        "Devkit": 0,
        "DevkitGameID": "",
        "DevkitOverrideAppID": 0,
        "LastPlayTime": 0,
        "FlatpakAppID": "",
        "tags": {},
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", required=True, help="Absolute path to the melonds-remote-client binary")
    parser.add_argument("--name", default="melonDS Remote", help="Shortcut display name (default: %(default)s)")
    parser.add_argument("--launch-options", default="", help="Extra launch arguments, e.g. --host 192.168.1.50")
    parser.add_argument("--user", help="Restrict to one Steam user id (userdata subfolder name); "
                                        "applies to every local user by default, so this is optional")
    parser.add_argument("--dry-run", action="store_true", help="Show what would change without writing anything")
    parser.add_argument("--force", action="store_true", help="Proceed even if Steam appears to be running")
    parser.add_argument("--remove", action="store_true",
                         help="Remove the shortcut instead of adding/updating it "
                              "(matched by --exe path, falling back to --name)")
    args = parser.parse_args()

    exe = os.path.abspath(os.path.expanduser(args.exe))
    if not args.remove and not args.dry_run and not os.path.isfile(exe):
        # Removal matches on the stored path string alone, so it must still
        # work even if the binary itself was since deleted or moved. A
        # --dry-run install preview is allowed to name a path that doesn't
        # exist yet either -- callers may pass a not-yet-populated central
        # install directory just to preview what a real run would do.
        print(f"error: --exe path does not exist: {exe}", file=sys.stderr)
        return 1

    if steam_is_running() and not args.force:
        print(
            "error: Steam appears to be running. Quit Steam first (it can silently overwrite "
            "this change when it next saves shortcuts.vdf), or pass --force if you're sure.",
            file=sys.stderr,
        )
        return 1

    userdata_dirs = find_userdata_dirs()
    if args.user:
        # Optional override for scripted/advanced use -- normal usage on
        # a Steam Deck applies to every local Steam user with no typing
        # needed, since the Deck's controller-only input can't type a
        # user id anyway (see below).
        userdata_dirs = [d for d in userdata_dirs if os.path.basename(d) == args.user]
        if not userdata_dirs:
            print(f"error: no userdata directory found for user id {args.user!r}", file=sys.stderr)
            return 1
    elif not userdata_dirs:
        print("error: no Steam userdata directory found (has Steam been run at least once?)", file=sys.stderr)
        return 1

    # Applies to every local Steam user found, rather than erroring out
    # and asking which one to pick -- a Steam Deck's controller-only
    # input has no reliable way to type a user id in response to such a
    # prompt (this project deliberately avoids relying on Steam Input's
    # on-screen keyboard everywhere else too, see docs/protocol.md's
    # "History: the 6-digit pairing code"). Harmless for the common
    # single-user case, and for any additional local users it just means
    # the shortcut is already there if they ever switch to that profile.
    if len(userdata_dirs) > 1:
        print(f"Found {len(userdata_dirs)} Steam users -- applying to all of them:")
        for d in userdata_dirs:
            print(f"  {os.path.basename(d)}")

    any_written = False
    for userdata_dir in userdata_dirs:
        shortcuts_path = os.path.join(userdata_dir, "config", "shortcuts.vdf")

        root = load_shortcuts(shortcuts_path)
        if args.remove:
            if not remove_shortcut(root, exe, args.name):
                print(f"No shortcut for {exe!r} found in {shortcuts_path} -- nothing to remove")
                continue
        else:
            upsert_shortcut(root, args.name, exe, os.path.dirname(exe), args.launch_options)
        new_data = write_vdf_map(root)

        # Round-trip check: make sure what we're about to write actually
        # parses back to the same structure before touching the real file.
        reparsed, _ = parse_vdf_map(new_data, 0)
        if reparsed != root:
            print(f"error: internal VDF round-trip check failed for {shortcuts_path} -- "
                  "not writing this one", file=sys.stderr)
            continue

        if args.dry_run:
            if args.remove:
                print(f"Would remove shortcut for {exe!r} from {shortcuts_path}")
            else:
                print(f"Would write {len(new_data)} bytes to {shortcuts_path}")
                print(f"Shortcut: name={args.name!r} exe={exe!r} launch_options={args.launch_options!r}")
            continue

        os.makedirs(os.path.dirname(shortcuts_path), exist_ok=True)
        if os.path.isfile(shortcuts_path):
            backup_path = f"{shortcuts_path}.bak-{int(time.time())}"
            shutil.copy2(shortcuts_path, backup_path)
            print(f"Backed up existing shortcuts.vdf to {backup_path}")

        with open(shortcuts_path, "wb") as f:
            f.write(new_data)

        if args.remove:
            print(f"Removed shortcut for {exe!r} from {shortcuts_path}")
        else:
            print(f"Added/updated \"{args.name}\" shortcut in {shortcuts_path}")
        any_written = True

    if args.dry_run:
        return 0
    if not any_written:
        if args.remove:
            print("Nothing removed -- shortcut not found for any local Steam user "
                  "(already removed, or never installed).")
            return 0
        print("error: nothing was written -- see errors above", file=sys.stderr)
        return 1

    if args.remove:
        print("Restart Steam (or switch to Gaming Mode) for the removal to take effect.")
    else:
        print("Restart Steam (or switch to Gaming Mode) to see it in your library.")
        print(
            "Reminder: set its Controller Layout to a plain \"Gamepad\" template once it's "
            "there -- see docs/steam-deck-setup.md's Gaming Mode section for why."
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
