#!/usr/bin/env python3
"""Opt-in experiment (real user request, 2026-08-02): "the controller
layout should function exactly like a steam controller/steam deck" --
specifically, the client's touchpads only forwarded raw touch
(SDL_EVENT_GAMEPAD_TOUCHPAD_*, what client/src/main.cpp actually reads)
while the STEAM button was held, because Steam Input's own action-set
layer was the only place a Trackpad-type binding existed for the
DualDeck Client's auto-picked, unconfigured Controller Layout (see
docs/known-limitations.md's 2026-08-02 touchpad entry).

Writing a *custom* Steam Input Controller Layout (a reverse-engineered,
undocumented per-controller-type binary/text VDF schema) was the first
approach considered, but real research turned up a simpler, better-
precedented fix instead: Steam Deck's own controller reports raw
touchpad data to SDL's HIDAPI driver *unconditionally*, independent of
any Steam Input binding, whenever Steam Input itself isn't intercepting
the device at all -- i.e. when Steam Input is fully disabled for that
one shortcut (Properties -> Controller -> "Disable Steam Input"), not
when some specific binding is configured. This is exactly what
RPCS3 (github.com/RPCS3/rpcs3, PR #18427, "steam: disable steam input
for shortcuts") already does for its own generated non-Steam shortcut --
this script's core logic (the localconfig.vdf path, the "apps" ->
"<appid>" -> "UseSteamControllerConfig" "0" key, and computing that
appid the exact same legacy CRC32 way) mirrors that real, shipped
precedent, not a guess.

Unlike steam_shortcut.py's shortcuts.vdf (Valve's own *binary* VDF
format, parsed elsewhere in this project), localconfig.vdf is Steam's
plain-*text* VDF/KeyValues format -- a different, much simpler grammar,
so this file has its own minimal parser/serializer rather than reusing
steam_shortcut.py's binary one.

Deliberately narrow and additive:
- Only ever touches ONE key ("UseSteamControllerConfig") under
  "UserLocalConfigStore" -> "Software" -> "Valve" -> "Steam" -> "apps" ->
  "<appid>" -- every other key anywhere in localconfig.vdf (playtime,
  last-played timestamps, every other app's own entry, etc.) is parsed,
  kept, and re-serialized completely unchanged.
- Only ever runs when explicitly invoked (a menu choice, not part of the
  normal install flow) -- this is an opt-in experiment, not a default,
  per the user's own explicit choice.
- Same defensive discipline as steam_shortcut.py: refuses to write while
  Steam is running and a write is actually needed (unless --force,
  matching run_steam_shortcut_with_restart's exact "Steam appears to be
  running" refusal-detection string so it plugs into the same automatic-
  restart helper), backs up the existing file first, and parses its own
  freshly-written output back as a sanity check before trusting it.

Honest, not hidden: disabling Steam Input for the whole DualDeck Client
shortcut also disables any Steam Input remapping/dead-zone/gyro tuning
for it -- believed to be a non-issue here specifically, since the client
reads the Deck's controller directly via SDL's own gamepad API expecting
a fixed layout it maps itself (see docs/steam-deck-setup.md's controller
mapping table), not anything Steam Input would otherwise be providing --
but this has not been confirmed against real Steam Deck hardware, only
against real, independent research (SDL's own Steam Controller touchpad
PR discussion; RPCS3's shipped implementation). The 2026-08-02
known-limitations.md entry for this experiment states this plainly.
"""
import argparse
import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib


# ---- Minimal text VDF (KeyValues) parser/serializer ----
# Steam's localconfig.vdf is Valve's plain-text KeyValues format:
# quoted "key" "value" pairs, or "key" { ...nested map... }, whitespace-
# insignificant otherwise. Deliberately separate from steam_shortcut.py's
# parse_vdf_map()/write_vdf_map(), which parse the unrelated *binary* VDF
# format shortcuts.vdf uses -- same "kept as a separate, purpose-built
# copy" reasoning this project already uses elsewhere for two genuinely
# different wire/file formats that happen to share a family name (see
# scripts/lib/apprun_templates.sh's own cross-reference comment for the
# same kind of justified duplication).
_TOKEN_RE = re.compile(r'"((?:\\.|[^"\\])*)"|([{}])')


def _unescape(s: str) -> str:
    return s.replace('\\"', '"').replace("\\\\", "\\")


def _escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def _tokenize(text: str) -> list[str]:
    tokens = []
    for m in _TOKEN_RE.finditer(text):
        if m.group(2) is not None:
            tokens.append(m.group(2))
        else:
            tokens.append(_unescape(m.group(1)))
    return tokens


def parse_text_vdf(text: str) -> "dict[str, object]":
    """Parses a whole text-VDF document into a nested, ORDERED dict
    (insertion order preserved, so re-serializing an untouched document
    round-trips key order exactly -- matters for a file Steam itself
    also reads/writes, where an unnecessary diff invites suspicion of
    what actually changed). Values are either str (leaf) or dict
    (nested map)."""
    tokens = _tokenize(text)
    pos = 0

    def parse_map() -> "tuple[dict, int]":
        nonlocal pos
        result: dict = {}
        while pos < len(tokens):
            if tokens[pos] == "}":
                pos += 1
                return result, pos
            key = tokens[pos]
            pos += 1
            if pos < len(tokens) and tokens[pos] == "{":
                pos += 1
                value, _ = parse_map()
            else:
                value = tokens[pos]
                pos += 1
            result[key] = value
        return result, pos

    root, _ = parse_map()
    return root


def write_text_vdf(entries: dict, indent: int = 0) -> str:
    pad = "\t" * indent
    out = []
    for key, value in entries.items():
        if isinstance(value, dict):
            out.append(f'{pad}"{_escape(key)}"\n{pad}{{\n')
            out.append(write_text_vdf(value, indent + 1))
            out.append(f"{pad}}}\n")
        else:
            out.append(f'{pad}"{_escape(key)}"\t\t"{_escape(str(value))}"\n')
    return "".join(out)


# ---- Shortcut appid (mirrors steam_shortcut.py's legacy_shortcut_appid()
# exactly -- MUST compute the identical value for the identical --exe/
# --name, since the whole point is targeting the same shortcut
# steam_shortcut.py already created. Kept as a separate copy rather than
# importing that module: both scripts are meant to be run standalone
# from wherever build-release.sh copies them (client/internal/ and
# host/internal/), and this project already has a "kept in sync here
# manually" precedent for exactly this situation, e.g.
# adapter_socket_probe.sh's own cross-reference to
# apprun_templates.sh's embedded copy) ----
def legacy_shortcut_appid(exe: str, appname: str) -> int:
    """Byte-for-byte identical to steam_shortcut.py's function of the
    same name -- MUST stay that way, since the whole point is landing on
    the exact same appid steam_shortcut.py already used for this
    shortcut in shortcuts.vdf. RPCS3's own real localconfig.vdf writer
    (see this file's module docstring) stores this same signed 32-bit
    form under "apps", not the raw unsigned CRC32 -- matched here."""
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
        return False


def _get_apps_map(root: dict) -> dict:
    """Walks/creates "UserLocalConfigStore" -> "Software" -> "Valve" ->
    "Steam" -> "apps", creating any missing intermediate map so this
    also works against a nearly-empty file (unlikely for a real Steam
    install, but keeps this from crashing on an unusual one)."""
    node = root
    for key in ("UserLocalConfigStore", "Software", "Valve", "Steam", "apps"):
        if key not in node or not isinstance(node[key], dict):
            node[key] = {}
        node = node[key]
    return node


def set_steam_input_disabled(localconfig_path: str, appid: int, disabled: bool) -> bool:
    """Sets (disabled=True) or clears (disabled=False)
    "UseSteamControllerConfig" for the given appid in localconfig_path.
    Returns True if the file's content actually changed (nothing to do
    if it already matched, matching steam_shortcut.py's own
    shortcut_up_to_date()-style idempotency -- avoids writing/backing-up
    a byte-for-byte-identical file on a routine re-run)."""
    if os.path.isfile(localconfig_path):
        with open(localconfig_path, "r", encoding="utf-8", errors="replace") as f:
            original_text = f.read()
        root = parse_text_vdf(original_text)
    else:
        original_text = ""
        root = {}

    apps = _get_apps_map(root)
    key = str(appid)

    if disabled:
        entry = apps.get(key)
        if not isinstance(entry, dict):
            entry = {}
        if entry.get("UseSteamControllerConfig") == "0":
            return False  # already set -- nothing to do
        entry["UseSteamControllerConfig"] = "0"
        apps[key] = entry
    else:
        entry = apps.get(key)
        if not isinstance(entry, dict) or "UseSteamControllerConfig" not in entry:
            return False  # nothing to remove
        del entry["UseSteamControllerConfig"]
        if entry:
            apps[key] = entry
        else:
            del apps[key]  # don't leave a bare, empty "<appid>" {} behind

    new_text = write_text_vdf(root)

    if original_text:
        backup_path = f"{localconfig_path}.bak-{int(time.time())}"
        shutil.copy2(localconfig_path, backup_path)
        print(f"Backed up existing localconfig.vdf to {backup_path}")

    with open(localconfig_path, "w", encoding="utf-8") as f:
        f.write(new_text)

    # Same "parse our own freshly-written output back" sanity check
    # steam_shortcut.py's binary-VDF path already does -- restores the
    # backup rather than leaving a possibly-corrupt file in place if
    # something about this write doesn't round-trip cleanly.
    try:
        with open(localconfig_path, "r", encoding="utf-8") as f:
            reparsed = parse_text_vdf(f.read())
        reparsed_entry = reparsed.get("UserLocalConfigStore", {}).get("Software", {}).get("Valve", {}).get(
            "Steam", {}
        ).get("apps", {}).get(key, {})
        expected = "0" if disabled else None
        actual = reparsed_entry.get("UseSteamControllerConfig") if isinstance(reparsed_entry, dict) else None
        if actual != expected:
            raise ValueError(f"round-trip check failed: expected {expected!r}, got {actual!r}")
    except Exception as exc:
        if original_text:
            shutil.move(backup_path, localconfig_path)
            print(f"error: writing localconfig.vdf did not round-trip cleanly ({exc}) -- restored the backup.",
                  file=sys.stderr)
        else:
            os.remove(localconfig_path)
            print(f"error: writing localconfig.vdf did not round-trip cleanly ({exc}) -- removed the new file.",
                  file=sys.stderr)
        raise

    return True


def is_steam_input_disabled(localconfig_path: str, appid: int) -> bool:
    """Read-only status check -- doesn't write anything, so it's safe to
    call regardless of whether Steam is running (used by the menu's
    dynamic Enable/Disable label)."""
    if not os.path.isfile(localconfig_path):
        return False
    with open(localconfig_path, "r", encoding="utf-8", errors="replace") as f:
        root = parse_text_vdf(f.read())
    entry = (
        root.get("UserLocalConfigStore", {})
        .get("Software", {})
        .get("Valve", {})
        .get("Steam", {})
        .get("apps", {})
        .get(str(appid), {})
    )
    return isinstance(entry, dict) and entry.get("UseSteamControllerConfig") == "0"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, help="Same --exe steam_shortcut.py was given for this shortcut.")
    parser.add_argument("--name", required=True, help="Same --name steam_shortcut.py was given for this shortcut.")
    parser.add_argument("--remove", action="store_true",
                         help="Re-enable Steam Input for this shortcut instead of disabling it.")
    parser.add_argument("--status", action="store_true",
                         help="Print 'disabled' or 'enabled' and exit -- doesn't write anything, doesn't need "
                              "Steam closed. Checks every local user's config and reports 'disabled' if ANY of "
                              "them have it set (matching how --exe/--name themselves apply to every local user "
                              "by default).")
    parser.add_argument("--user", help="Only touch this Steam userdata dir (default: every local user).")
    parser.add_argument("--force", action="store_true",
                         help="Write even if Steam appears to be running (risks Steam clobbering the change on "
                              "its next save -- see steam_shortcut.py's own --force for the identical tradeoff).")
    args = parser.parse_args()

    if args.status:
        signed_appid = legacy_shortcut_appid(args.exe, args.name)
        userdata_dirs = [args.user] if args.user else find_userdata_dirs()
        disabled = any(
            is_steam_input_disabled(os.path.join(d, "config", "localconfig.vdf"), signed_appid)
            for d in userdata_dirs
        )
        print("disabled" if disabled else "enabled")
        return 0

    if steam_is_running() and not args.force:
        print("Steam appears to be running -- refusing to edit localconfig.vdf (it caches this file in memory "
              "and can silently overwrite this change when it next saves), or pass --force if you're sure.",
              file=sys.stderr)
        return 1

    signed_appid = legacy_shortcut_appid(args.exe, args.name)

    userdata_dirs = [args.user] if args.user else find_userdata_dirs()
    if not userdata_dirs:
        print("error: no Steam userdata directory found.", file=sys.stderr)
        return 1

    any_changed = False
    for userdata_dir in userdata_dirs:
        localconfig_path = os.path.join(userdata_dir, "config", "localconfig.vdf")
        if not os.path.isfile(localconfig_path):
            continue  # this Steam user has never actually run Steam -- nothing to edit
        changed = set_steam_input_disabled(localconfig_path, signed_appid, disabled=not args.remove)
        any_changed = any_changed or changed
        if changed:
            action = "Re-enabled" if args.remove else "Disabled"
            print(f"{action} Steam Input for appid {signed_appid} in {localconfig_path}")

    if not any_changed:
        print("Nothing to change (already in the requested state, or no localconfig.vdf found for any local user).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
