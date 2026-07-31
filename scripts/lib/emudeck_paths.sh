#!/usr/bin/env bash
# EmuDeck install-location detection, meant to be `source`d (not executed
# directly) by scripts/emudeck-replace-in-place.sh and
# scripts/emudeck-check-drift.sh.
#
# EmuDeck installs melonDS/Azahar (and its predecessor Citra)/Cemu as
# single-file AppImages under $HOME/Applications/, and its own per-emulator
# launcher scripts pick whichever matching AppImage there is newest by
# mtime rather than one fixed filename (EmuDeck's own tools/launchers/*.sh
# do this, e.g. cemu.sh) -- so detection here matches by a case-insensitive
# name fragment + newest-mtime, the same convention, rather than assuming
# one exact filename. This has NOT been verified against a real EmuDeck
# install from inside this repo (no such install available in this
# project's development sandbox) -- see docs/known-limitations.md's Phase A
# entry. Confirm the glob patterns below actually match a real install
# before relying on this in production.
#
# Deliberately excludes anything not ending in exactly ".AppImage" from
# every glob below (in particular, backup/manifest files this same phase's
# scripts/lib/appimage_manifest.py writes alongside the original, e.g.
# "<name>.AppImage.dualdeck-original" and "<name>.AppImage.dualdeck.json")
# -- so re-running detection after an install never mistakes DualDeck's own
# backup copy for "the newest AppImage" and loops back on itself.

# emudeck_applications_dir
#
# The one directory EmuDeck installs all AppImages into. Not configurable
# today (EmuDeck itself hardcodes this), but kept as its own function
# rather than inlined everywhere in case that ever needs to change.
emudeck_applications_dir() {
    echo "${HOME}/Applications"
}

# _find_newest_appimage <case-insensitive glob fragment>
#
# Internal helper: finds the most-recently-modified *.AppImage file under
# emudeck_applications_dir() whose name matches the given fragment
# (case-insensitively, substring match). Prints its path on success;
# prints nothing and returns 1 if no match exists (directory missing or
# genuinely no matching AppImage -- both are "EmuDeck doesn't have this
# emulator installed," not an error the caller should treat specially).
_find_newest_appimage() {
    local fragment="$1"
    local apps_dir
    apps_dir="$(emudeck_applications_dir)"

    [[ -d "${apps_dir}" ]] || return 1

    local newest="" newest_mtime=-1
    local f mtime
    shopt -s nullglob nocaseglob
    for f in "${apps_dir}"/*"${fragment}"*.AppImage; do
        # Exact suffix check even with nocaseglob on -- nocaseglob would
        # otherwise also match e.g. "*.appimage" or "*.APPIMAGE", which
        # EmuDeck itself never produces but a manually-renamed file might.
        [[ "${f}" == *.AppImage ]] || continue
        mtime="$(stat -c '%Y' "${f}" 2>/dev/null || echo -1)"
        if (( mtime > newest_mtime )); then
            newest="${f}"
            newest_mtime="${mtime}"
        fi
    done
    shopt -u nullglob nocaseglob

    [[ -n "${newest}" ]] || return 1
    echo "${newest}"
}

# find_emudeck_melonds_appimage
find_emudeck_melonds_appimage() {
    _find_newest_appimage "melonds"
}

# find_emudeck_azahar_appimage
#
# Matches both "azahar" and EmuDeck's older "citra" naming (Azahar is
# Citra's community continuation; EmuDeck installs referenced in the wild
# as recently as this project's own research this session still call the
# AppImage citra-qt.AppImage) -- tried in this order, first match wins.
find_emudeck_azahar_appimage() {
    _find_newest_appimage "azahar" || _find_newest_appimage "citra"
}

# find_emudeck_cemu_appimage
#
# EmuDeck's own cemu.sh launcher prefers an AppImage, falling back to
# Flatpak and then a Proton/Windows build if no AppImage is present (in
# that priority order) -- this function only ever looks for the AppImage
# case, which is what Phase A's replace-in-place installer supports today.
# See find_emudeck_cemu_flatpak_id() below for detecting (and cleanly
# refusing) the Flatpak case rather than silently reporting "not found."
find_emudeck_cemu_appimage() {
    _find_newest_appimage "cemu"
}

# find_emudeck_cemu_flatpak_id
#
# Prints Cemu's Flatpak application ID ("info.cemu.Cemu") if it's
# installed via Flatpak, so callers can detect this case and refuse
# cleanly ("Flatpak replace-in-place isn't supported yet") instead of
# silently doing nothing when find_emudeck_cemu_appimage() finds no
# AppImage. Flatpak apps are sandboxed and launched via `flatpak run
# <app-id>`, not a plain file path DualDeck could drop a replacement
# binary at -- a materially different install shape this phase doesn't
# attempt to handle.
find_emudeck_cemu_flatpak_id() {
    command -v flatpak >/dev/null 2>&1 || return 1
    flatpak info info.cemu.Cemu >/dev/null 2>&1 && echo "info.cemu.Cemu"
}
