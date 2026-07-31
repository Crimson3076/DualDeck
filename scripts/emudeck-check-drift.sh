#!/usr/bin/env bash
# Checks whether any EmuDeck AppImage scripts/emudeck-replace-in-place.sh
# previously installed has been silently replaced since (most likely by
# EmuDeck's own "Manage Emulators" updater doing a straight file swap) --
# see docs/known-limitations.md's Phase A entry for why this can only ever
# detect drift after the fact, not prevent the window where a replaced
# file is transiently back to stock (no remote-server capability) between
# the swap and the next time this runs.
#
# Report-only by default (no side effects -- safe to run from a cron job
# or, once it exists, a persistent host-service startup hook per Phase B).
# Pass --fix to actually re-patch any drifted emulator by invoking
# emudeck-replace-in-place.sh --yes for it.
#
# Usage:
#   ./scripts/emudeck-check-drift.sh [--emulator melonds|azahar|cemu]... [--fix]
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/lib/emudeck_paths.sh
source "${repo_root}/scripts/lib/emudeck_paths.sh"

manifest_py="${repo_root}/scripts/lib/appimage_manifest.py"

emulators=()
fix=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --emulator) emulators+=("$2"); shift 2 ;;
        --fix) fix=1; shift ;;
        -h|--help)
            sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "error: unknown argument: $1" >&2; exit 1 ;;
    esac
done
if [[ "${#emulators[@]}" -eq 0 ]]; then
    emulators=(melonds azahar cemu)
fi

drifted_emulators=()

check_one() {
    local emulator="$1"
    local appimage_path=""

    case "${emulator}" in
        melonds) appimage_path="$(find_emudeck_melonds_appimage)" || return 0 ;;
        azahar)  appimage_path="$(find_emudeck_azahar_appimage)" || return 0 ;;
        cemu)    appimage_path="$(find_emudeck_cemu_appimage)" || return 0 ;;
    esac

    local status
    status="$(python3 "${manifest_py}" check "${appimage_path}")"
    case "${status}" in
        no_manifest)
            # DualDeck never installed here -- not this script's concern.
            return 0
            ;;
        matches_patched)
            echo "${emulator}: OK (DualDeck's patched build is installed, unchanged)"
            ;;
        matches_original)
            echo "${emulator}: reverted to the original stock build (no remote-server capability)"
            drifted_emulators+=("${emulator}")
            ;;
        drifted)
            echo "${emulator}: DRIFTED -- installed file matches neither DualDeck's patched build nor"
            echo "  the known original (EmuDeck's updater likely replaced it with a newer stock"
            echo "  build). No remote-server capability until this is re-patched."
            drifted_emulators+=("${emulator}")
            ;;
        missing)
            echo "${emulator}: the AppImage DualDeck last installed at ${appimage_path} is gone"
            ;;
    esac
}

for emulator in "${emulators[@]}"; do
    check_one "${emulator}"
done

if [[ "${#drifted_emulators[@]}" -eq 0 ]]; then
    exit 0
fi

if [[ "${fix}" -eq 1 ]]; then
    echo "== Re-patching: ${drifted_emulators[*]} =="
    args=(--yes)
    for e in "${drifted_emulators[@]}"; do
        args+=(--emulator "${e}")
    done
    exec "${repo_root}/scripts/emudeck-replace-in-place.sh" "${args[@]}"
else
    echo ""
    echo "Run with --fix to re-patch the emulator(s) listed above, or:"
    echo "  ./scripts/emudeck-replace-in-place.sh --emulator <name> --yes"
    exit 1
fi
