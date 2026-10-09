#!/usr/bin/env bash
# Removes the DualDeck Steam non-Steam-game shortcut added by
# install-steam-shortcut.sh, and deletes the central install directory
# that script copies everything into. See ./steam_shortcut.py for
# exactly what the shortcut-removal part does and why it's careful
# about it (backs up shortcuts.vdf first, refuses to run while Steam is
# open unless --force). Normally launched via
# ../../dualdeck-client.sh's "Remove from Steam" menu choice, not
# directly.
#
# Always targets the fixed central install directory below for the
# actual --exe match, regardless of where this script itself is run
# from: this exact file is also the one copied into that central
# directory by install-steam-shortcut.sh, and must keep removing the
# same shortcut from there indefinitely, even after this archive has
# been deleted. It does fall back to this archive's own sibling copy of
# steam_shortcut.py (see below) purely so a freshly downloaded archive
# can still clean up a shortcut from an older version of this project
# that was never migrated to the central directory -- steam_shortcut.py's
# own AppName fallback (see its module docstring) is what actually finds
# and removes that kind of stale entry.
set -euo pipefail

# Surfaces failures visibly instead of just closing silently when
# double-clicked with no visible terminal attached (GitHub issue #11) --
# logs to a persistent file and, when available (SteamOS Desktop Mode/
# Bazzite are both KDE Plasma), pops up a graphical error dialog via
# kdialog.
error_log="${HOME}/.config/dualdeck-client/install.log"
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") uninstall-steam-shortcut.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "DualDeck" \
            --error "Removing the Steam shortcut failed: ${failing_cmd}
(exit code ${exit_code})

Details logged to:
${error_log}" 2>/dev/null || true
    fi
}
trap 'ec=$?; on_error "${ec}" "${LINENO}" "${BASH_COMMAND}"' ERR

# Keep in sync with the same constant in install-steam-shortcut.sh and
# apply-update.sh.
central_install_dir="${HOME}/.config/dualdeck-client/install"
self_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

steam_shortcut_py=""
for candidate in \
    "${central_install_dir}/internal/steam_shortcut.py" \
    "${self_dir}/steam_shortcut.py"
do
    if [[ -f "${candidate}" ]]; then
        steam_shortcut_py="${candidate}"
        break
    fi
done

if [[ -z "${steam_shortcut_py}" ]]; then
    echo "Nothing installed at ${central_install_dir}, and no local copy of" \
         "steam_shortcut.py found either -- nothing to remove."
    exit 0
fi

# Same two-candidate resolution as steam_shortcut_py above -- this
# script runs either from an extracted archive or from the central
# install copy, and steam_restart_helper.sh is bundled alongside
# steam_shortcut.py in both places (scripts/build-release.sh's
# packaging step).
steam_restart_helper=""
for candidate in \
    "${central_install_dir}/internal/steam_restart_helper.sh" \
    "${self_dir}/steam_restart_helper.sh"
do
    if [[ -f "${candidate}" ]]; then
        steam_restart_helper="${candidate}"
        break
    fi
done
if [[ -n "${steam_restart_helper}" ]]; then
    # shellcheck source=scripts/lib/steam_restart_helper.sh
    source "${steam_restart_helper}"
fi

# One-time melonDS-Remote -> DualDeck rebrand cleanup: also try removing
# under the old identity (old central dir, old Exe, old AppName), since
# the Exe-OR-AppName fallback alone can't bridge a compound change of
# both fields at once. No-op if it was never installed there or was
# already migrated.
old_central_install_dir="${HOME}/.config/melonds-remote-client/install"
python3 "${steam_shortcut_py}" \
    --exe "${old_central_install_dir}/internal/run-client.sh" \
    --name "melonDS Remote" \
    --remove "$@" >/dev/null 2>&1 || true

if [[ -n "${steam_restart_helper}" ]]; then
    run_steam_shortcut_with_restart "${steam_shortcut_py}" "${error_log}" \
        --exe "${central_install_dir}/internal/run-client.sh" \
        --name "DualDeck" \
        --remove \
        "$@"
else
    python3 "${steam_shortcut_py}" \
        --exe "${central_install_dir}/internal/run-client.sh" \
        --name "DualDeck" \
        --remove \
        "$@"
fi

dry_run=0
for arg in "$@"; do
    [[ "${arg}" == "--dry-run" ]] && dry_run=1
done

if [[ "${dry_run}" -eq 0 ]]; then
    # cd out first -- this script may itself be running from inside
    # central_install_dir (it's the copy install-steam-shortcut.sh made
    # there), so deleting the shell's own current directory out from
    # under it is avoided by leaving before removing anything.
    cd /
    for dir in "${central_install_dir}" "${central_install_dir}.new" "${central_install_dir}.previous"; do
        if [[ -d "${dir}" ]]; then
            echo "Removing ${dir}"
            rm -rf -- "${dir}"
        fi
    done

    # check-for-updates.sh/VERSION are staged as siblings of install/
    # (see install-steam-shortcut.sh), not inside any of the three
    # directories just removed above.
    rm -f -- "$(dirname "${central_install_dir}")/check-for-updates.sh" \
             "$(dirname "${central_install_dir}")/VERSION"

    # Also clear install-steam-shortcut.sh's one-time auto-configure
    # marker, so a later reinstall applies the DualDeck control scheme
    # fresh again instead of silently staying hands-off forever because
    # of a marker left over from this now-deleted install.
    rm -f -- "$(dirname "${central_install_dir}")/.steam-input-auto-configured"
fi
