#!/usr/bin/env bash
# Applies the three local, --user-scoped Flatpak permission grants (plus
# one ~/.local/bin symlink per emulator) confirmed on real hardware to be
# necessary and sufficient for RetroDECK to launch DualDeck's patched
# Cemu, melonDS, and Azahar -- through RetroDECK's own point-and-click
# game list, the `-e` CLI override, and Tender (a Decky plugin that calls
# into RetroDECK's own launch mechanism) alike, with no RetroDECK-side
# change and no Tender-specific code anywhere. See
# docs/retrodeck-compatibility.md's "Real, confirmed blockers" section
# and docs/known-limitations.md's 2026-08-29 entries for the full
# investigation and root-cause writeup this script is a direct
# distillation of -- read those first if any of this needs adjusting.
#
# What each grant is for (--restore reverts all of them at once via a
# full `flatpak override --user --reset` -- see that function's own
# comment for why a full reset, not a per-grant undo):
#   1. --filesystem=xdg-run/dualdeck:create
#      RetroDECK's Flatpak sandbox does NOT see $XDG_RUNTIME_DIR/dualdeck/
#      (where the Host Service's adapter socket lives) despite its broad
#      --filesystem=host grant -- confirmed live, Flatpak's `host`
#      filesystem permission does not extend to /run. Without this, Cemu/
#      Azahar running inside RetroDECK can never reach a persistent Host
#      Service already running on the same machine (e.g. for EmuDeck).
#      melonDS's own patch handles the remote connection in-process (see
#      apprun_templates.sh's generate_apprun_melonds()) rather than
#      connecting out to a separate Host Service over this socket, so
#      this specific grant is mainly for Cemu/Azahar -- harmless to apply
#      unconditionally regardless of which emulators are in use.
#   2. --env=APPIMAGE_EXTRACT_AND_RUN=1
#      RetroDECK's sandbox has no fusermount/fusermount3 binary, so a raw
#      .AppImage fails outright ("Cannot mount AppImage"). This env var
#      makes the AppImage runtime skip FUSE and extract-and-run instead --
#      not RetroDECK- or emulator-specific, any AppImage hits the same
#      wall.
#   3. --env=PATH=<home>/.local/bin:/app/bin:/usr/bin + a symlink per
#      emulator (~/.local/bin/cemu, ~/.local/bin/melonds,
#      ~/.local/bin/azahar)
#      RetroDECK has two separate implementations that resolve
#      "%EMULATOR_X%" to a real path (ES-DE's own GUI, and a from-scratch
#      bash reimplementation in /app/libexec/run_game.sh -- the one both
#      the CLI override and Tender's own launch calls go through). Both
#      have real bugs/gaps that make them skip a valid
#      `~/Applications/<Name>*.AppImage` staticpath entry -- but both
#      correctly honor the higher-priority `systempath` rule (a plain
#      PATH lookup for a bare command name) first. RetroDECK's actual
#      launch scripts run with a deliberately narrow
#      PATH=/app/bin:/usr/bin that excludes ~/.local/bin (confirmed by
#      dumping the real environment from inside an actual launch, not an
#      interactive debug shell, which has a different PATH) -- widening
#      it, plus a symlink per emulator pointing at its patched AppImage,
#      wins the systempath check immediately, before either buggy
#      staticpath path is ever reached. The exact symlink name matters --
#      it must match one of es_find_rules.xml's own systempath <entry>
#      values for that emulator (confirmed against a real installed
#      RetroDECK, not assumed): "cemu", "melonds", and "azahar"
#      respectively (all lowercase).
#
# melonDS/Nintendo DS has one more real requirement this script can't
# do anything about: RetroDECK's own "nds" system lists four RetroArch
# cores *before* "melonDS (Standalone)" in its es_systems.xml, so
# RetroDECK will keep using a libretro core regardless of anything fixed
# here until you manually switch the DS system's selected emulator to
# "melonDS (Standalone)" in RetroDECK's own UI (its Configurator, or
# ES-DE's per-system/per-game "Select alternative emulator" screen).
# Nintendo Wii U (Cemu) and 3DS (Azahar) both already default to their
# "(Standalone)" command with no such manual step needed -- confirmed
# against a real installed RetroDECK's es_systems.xml.
#
# Usage:
#   ./scripts/retrodeck-setup.sh                 Apply for every emulator with a patched AppImage installed
#   ./scripts/retrodeck-setup.sh --emulator cemu Apply for just one (repeatable: --emulator cemu --emulator azahar)
#   ./scripts/retrodeck-setup.sh --dry-run       Show what would change, do nothing
#   ./scripts/retrodeck-setup.sh --status        Report current state, change nothing
#   ./scripts/retrodeck-setup.sh --restore       Undo all grants + remove every emulator's symlink
set -euo pipefail

app_id="net.retrodeck.retrodeck"
local_bin_dir="${HOME}/.local/bin"
widened_path="${local_bin_dir}:/app/bin:/usr/bin"

# Every emulator this covers. Order is display order only.
all_emulators=(cemu melonds azahar)
declare -A appimage_path=(
    [cemu]="${HOME}/Applications/Cemu.AppImage"
    [melonds]="${HOME}/Applications/melonDS.AppImage"
    [azahar]="${HOME}/Applications/azahar.AppImage"
)
# Must exactly match a real systempath <entry> in RetroDECK's own
# es_find_rules.xml for that emulator (case-sensitive) -- see this
# script's own header comment.
declare -A systempath_name=(
    [cemu]="cemu"
    [melonds]="melonds"
    [azahar]="azahar"
)
declare -A display_name=(
    [cemu]="Cemu (Nintendo Wii U)"
    [melonds]="melonDS (Nintendo DS)"
    [azahar]="Azahar (Nintendo 3DS)"
)

dry_run=0
mode="apply"
selected_emulators=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) dry_run=1; shift ;;
        --status) mode="status"; shift ;;
        --restore) mode="restore"; shift ;;
        --emulator)
            selected_emulators+=("$2")
            shift 2
            ;;
        -h|--help)
            grep '^#' "${BASH_SOURCE[0]}" | sed 's/^#//; s/^ //'
            exit 0
            ;;
        *)
            echo "error: unknown argument: $1 (see --help)" >&2
            exit 1
            ;;
    esac
done

for e in "${selected_emulators[@]}"; do
    if [[ -z "${appimage_path[$e]+set}" ]]; then
        echo "error: --emulator must be one of: ${all_emulators[*]} (got: ${e})" >&2
        exit 1
    fi
done

# With no --emulator given, cover every emulator this script knows about
# -- apply()/restore() skip (not fail on) one with no patched AppImage
# installed; status() reports on all of them regardless.
emulators=("${all_emulators[@]}")
if [[ "${#selected_emulators[@]}" -gt 0 ]]; then
    emulators=("${selected_emulators[@]}")
fi

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

    for e in "${emulators[@]}"; do
        local symlink="${local_bin_dir}/${systempath_name[$e]}"
        echo
        echo "== ${display_name[$e]} =="
        echo "AppImage (${appimage_path[$e]}):"
        if [[ -x "${appimage_path[$e]}" ]]; then
            echo "  present and executable"
        else
            echo "  missing or not executable -- run ./scripts/emudeck-replace-in-place.sh first"
        fi
        echo "Symlink (${symlink}):"
        if [[ -L "${symlink}" ]]; then
            echo "  -> $(readlink -f "${symlink}")"
        elif [[ -e "${symlink}" ]]; then
            echo "  exists, but is not a symlink -- unexpected, check it manually"
        else
            echo "  (missing)"
        fi
    done
    echo
    echo "Note: RetroDECK's \"Nintendo DS\" system also needs its active"
    echo "emulator manually switched to \"melonDS (Standalone)\" in"
    echo "RetroDECK's own settings -- see this script's --help for why."
}

apply() {
    require_flatpak_and_retrodeck
    local applied_any=0

    echo "Applying RetroDECK compatibility overrides for ${app_id}..."
    run mkdir -p "${local_bin_dir}"
    for e in "${emulators[@]}"; do
        if [[ ! -x "${appimage_path[$e]}" ]]; then
            echo "skipping ${display_name[$e]}: ${appimage_path[$e]} not found or not executable" \
                "(run ./scripts/emudeck-replace-in-place.sh first)"
            continue
        fi
        run ln -sf "${appimage_path[$e]}" "${local_bin_dir}/${systempath_name[$e]}"
        applied_any=1
    done

    if [[ "${applied_any}" -eq 0 ]]; then
        echo "error: none of the requested emulators have a patched AppImage installed yet." >&2
        echo "Run ./scripts/emudeck-replace-in-place.sh first, then try again." >&2
        exit 1
    fi

    run flatpak override --user --filesystem=xdg-run/dualdeck:create "${app_id}"
    run flatpak override --user --env=APPIMAGE_EXTRACT_AND_RUN=1 "${app_id}"
    run flatpak override --user --env="PATH=${widened_path}" "${app_id}"

    if [[ "${dry_run}" -eq 0 ]]; then
        echo
        echo "Done. If RetroDECK is currently running, restart it for the new"
        echo "environment to take effect. Launching a game -- through RetroDECK's"
        echo "own game list, the CLI, or Tender -- should now use the"
        echo "DualDeck-patched build for whichever emulators above weren't skipped."
        for e in "${emulators[@]}"; do
            if [[ "$e" == "melonds" && -x "${appimage_path[$e]}" ]]; then
                echo
                echo "Nintendo DS also needs one manual step in RetroDECK itself:"
                echo "switch its active emulator to \"melonDS (Standalone)\" (RetroDECK's"
                echo "Configurator, or ES-DE's per-system/per-game \"Select alternative"
                echo "emulator\" screen) -- RetroDECK defaults DS to a RetroArch core, and"
                echo "nothing this script does can change that from outside RetroDECK."
            fi
        done
    fi
}

restore() {
    require_flatpak_and_retrodeck
    echo "Removing RetroDECK compatibility overrides for ${app_id}..."
    # A full `flatpak override --user --reset` rather than targeted
    # `--unset-env=X`/`--nofilesystem=X` flags. An earlier version of this
    # script used the targeted approach specifically to avoid clearing any
    # unrelated override the user might have set independently -- real
    # hardware testing (2026-08-29) found that combination of
    # `--unset-env=PATH` and `--nofilesystem=xdg-run/dualdeck` left
    # RetroDECK unable to launch at all, rather than cleanly reverting to
    # its shipped defaults (Flatpak's own override-removal semantics
    # here are apparently not as simple as "undo the earlier --env/
    # --filesystem call"). A full reset is blunter -- it clears every
    # override for this app, not just these three -- but is the one
    # approach actually confirmed to leave RetroDECK working afterward.
    run flatpak override --user --reset "${app_id}"
    for e in "${emulators[@]}"; do
        local symlink="${local_bin_dir}/${systempath_name[$e]}"
        if [[ -L "${symlink}" ]]; then
            run rm -f "${symlink}"
        fi
    done

    if [[ "${dry_run}" -eq 0 ]]; then
        echo
        echo "Done. RetroDECK will go back to launching its own bundled emulators"
        echo "(restart RetroDECK if it's currently running)."
    fi
}

case "${mode}" in
    status) status ;;
    restore) restore ;;
    apply) apply ;;
esac
