#!/usr/bin/env bash
# Applies the three local, --user-scoped Flatpak permission grants (plus
# one symlink) confirmed on real hardware to be necessary and sufficient
# for RetroDECK to launch DualDeck's patched Cemu -- through its own
# point-and-click game list, the `-e` CLI override, and Tender (a Decky
# plugin that calls into RetroDECK's own launch mechanism) alike, with
# no RetroDECK-side change and no Tender-specific code anywhere. See
# docs/retrodeck-compatibility.md's "Real, confirmed blockers" section
# and docs/known-limitations.md's 2026-08-29 entries for the full
# investigation and root-cause writeup this script is a direct
# distillation of -- read those first if any of this needs adjusting.
#
# What each grant is for (all reversible with --restore):
#   1. --filesystem=xdg-run/dualdeck:create
#      RetroDECK's Flatpak sandbox does NOT see $XDG_RUNTIME_DIR/dualdeck/
#      (where the Host Service's adapter socket lives) despite its broad
#      --filesystem=host grant -- confirmed live, Flatpak's `host`
#      filesystem permission does not extend to /run. Without this, Cemu
#      running inside RetroDECK can never reach a persistent Host Service
#      already running on the same machine (e.g. for EmuDeck).
#   2. --env=APPIMAGE_EXTRACT_AND_RUN=1
#      RetroDECK's sandbox has no fusermount/fusermount3 binary, so a raw
#      .AppImage fails outright ("Cannot mount AppImage"). This env var
#      makes the AppImage runtime skip FUSE and extract-and-run instead --
#      not RetroDECK- or Cemu-specific, any AppImage hits the same wall.
#   3. --env=PATH=<home>/.local/bin:/app/bin:/usr/bin + ~/.local/bin/cemu
#      RetroDECK has two separate implementations that resolve
#      "%EMULATOR_CEMU%" to a real path (ES-DE's own GUI, and a from-
#      scratch bash reimplementation in /app/libexec/run_game.sh -- the
#      one both the CLI override and Tender's own launch calls go
#      through). Both have real bugs/gaps that make them skip a valid
#      `~/Applications/Cemu*.AppImage` staticpath entry -- but both
#      correctly honor the higher-priority `systempath` rule (a plain
#      PATH lookup for a `cemu`/`Cemu` binary) first. RetroDECK's actual
#      launch scripts run with a deliberately narrow
#      PATH=/app/bin:/usr/bin that excludes ~/.local/bin (confirmed by
#      dumping the real environment from inside an actual launch, not an
#      interactive debug shell, which has a different PATH) -- widening
#      it, plus a `cemu` symlink pointing at the patched AppImage, wins
#      the systempath check immediately, before either buggy staticpath
#      path is ever reached.
#
# Usage:
#   ./scripts/retrodeck-setup.sh            Apply all three grants + symlink
#   ./scripts/retrodeck-setup.sh --dry-run  Show what would change, do nothing
#   ./scripts/retrodeck-setup.sh --status   Report current state, change nothing
#   ./scripts/retrodeck-setup.sh --restore  Undo all three grants + remove the symlink
#
# Cemu (Nintendo Wii U) only, matching the current RetroDECK-compatibility
# scope -- melonDS/Azahar are not covered.
set -euo pipefail

app_id="net.retrodeck.retrodeck"
cemu_appimage="${HOME}/Applications/Cemu.AppImage"
local_bin_dir="${HOME}/.local/bin"
local_bin_cemu="${local_bin_dir}/cemu"
widened_path="${local_bin_dir}:/app/bin:/usr/bin"

dry_run=0
mode="apply"

for arg in "$@"; do
    case "$arg" in
        --dry-run) dry_run=1 ;;
        --status) mode="status" ;;
        --restore) mode="restore" ;;
        -h|--help)
            grep '^#' "${BASH_SOURCE[0]}" | sed 's/^#//; s/^ //'
            exit 0
            ;;
        *)
            echo "error: unknown argument: $arg (see --help)" >&2
            exit 1
            ;;
    esac
done

run() {
    if [[ "${dry_run}" -eq 1 ]]; then
        echo "would run: $*"
    else
        echo "+ $*"
        "$@"
    fi
}

require_flatpak_and_retrodeck() {
    if ! command -v flatpak >/dev/null 2>&1; then
        echo "error: 'flatpak' not found on PATH -- this script must run on the host, not inside a Flatpak sandbox." >&2
        exit 1
    fi
    if ! flatpak info "${app_id}" >/dev/null 2>&1; then
        echo "error: RetroDECK (${app_id}) is not installed." >&2
        echo "Install it first (shown here for review, not run automatically):" >&2
        echo "  flatpak install --user flathub ${app_id}" >&2
        exit 1
    fi
}

status() {
    require_flatpak_and_retrodeck
    echo "== Flatpak overrides for ${app_id} =="
    flatpak override --user --show "${app_id}" 2>/dev/null || echo "(no user overrides set)"
    echo
    echo "== ${local_bin_cemu} =="
    if [[ -L "${local_bin_cemu}" ]]; then
        echo "symlink -> $(readlink -f "${local_bin_cemu}")"
    elif [[ -e "${local_bin_cemu}" ]]; then
        echo "exists, but is not a symlink -- unexpected, check it manually"
    else
        echo "(missing)"
    fi
    echo
    echo "== ${cemu_appimage} =="
    if [[ -x "${cemu_appimage}" ]]; then
        echo "present and executable"
    else
        echo "(missing or not executable -- run ./scripts/emudeck-replace-in-place.sh first)"
    fi
}

apply() {
    require_flatpak_and_retrodeck
    if [[ ! -x "${cemu_appimage}" ]]; then
        echo "error: ${cemu_appimage} not found or not executable." >&2
        echo "Run ./scripts/emudeck-replace-in-place.sh first to install the patched Cemu AppImage." >&2
        exit 1
    fi

    echo "Applying RetroDECK Cemu compatibility overrides for ${app_id}..."
    run mkdir -p "${local_bin_dir}"
    run ln -sf "${cemu_appimage}" "${local_bin_cemu}"
    run flatpak override --user --filesystem=xdg-run/dualdeck:create "${app_id}"
    run flatpak override --user --env=APPIMAGE_EXTRACT_AND_RUN=1 "${app_id}"
    run flatpak override --user --env="PATH=${widened_path}" "${app_id}"

    if [[ "${dry_run}" -eq 0 ]]; then
        echo
        echo "Done. If RetroDECK is currently running, restart it for the new"
        echo "environment to take effect. Launching a Wii U game -- through"
        echo "RetroDECK's own game list, the CLI, or Tender -- should now use"
        echo "the DualDeck-patched Cemu (window title: \"Cemu 2.6 - DualDeck\")."
    fi
}

restore() {
    require_flatpak_and_retrodeck
    echo "Removing RetroDECK Cemu compatibility overrides for ${app_id}..."
    # Targeted unset/no- flags rather than `flatpak override --user
    # --reset`, so any unrelated override the user set independently for
    # this app is left untouched.
    run flatpak override --user --unset-env=PATH "${app_id}"
    run flatpak override --user --unset-env=APPIMAGE_EXTRACT_AND_RUN "${app_id}"
    run flatpak override --user --nofilesystem=xdg-run/dualdeck "${app_id}"
    if [[ -L "${local_bin_cemu}" ]]; then
        run rm -f "${local_bin_cemu}"
    fi

    if [[ "${dry_run}" -eq 0 ]]; then
        echo
        echo "Done. RetroDECK will go back to launching its own bundled Cemu"
        echo "(restart RetroDECK if it's currently running)."
    fi
}

case "${mode}" in
    status) status ;;
    restore) restore ;;
    apply) apply ;;
esac
