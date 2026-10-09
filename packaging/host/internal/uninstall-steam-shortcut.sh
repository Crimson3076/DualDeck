#!/usr/bin/env bash
# Removes the "DualDeck Host" Steam non-Steam-game shortcut added
# by install-steam-shortcut.sh, the central install directory
# (~/.config/dualdeck/install) it copies everything into, and the
# Distrobox container if one was created -- i.e. this is the complete
# host uninstall once a Steam shortcut has been set up. (If you only
# ever used install-host-distrobox.sh directly and never installed the
# Steam shortcut, uninstall-host-distrobox.sh alone is equivalent.)
# Normally launched via ../../dualdeck-host.sh's "Remove from
# Steam" menu choice, not directly.
#
# Always targets the fixed central install directory below, regardless
# of where this script itself is run from -- this exact file is also
# the copy install-steam-shortcut.sh placed there, and must keep
# removing the same shortcut from there indefinitely, even after the
# original archive has been deleted (same reasoning as
# client/internal/uninstall-steam-shortcut.sh).
set -euo pipefail

error_log="${HOME}/.config/dualdeck/install.log"
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") uninstall-steam-shortcut.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "DualDeck Host" \
            --error "Removing the Steam shortcut failed: ${failing_cmd}
(exit code ${exit_code})

Details logged to:
${error_log}" 2>/dev/null || true
    fi
}
trap 'on_error "$?" "${LINENO}" "${BASH_COMMAND}"' ERR

# Keep in sync with the same constant in install-steam-shortcut.sh,
# install-host-distrobox.sh, and uninstall-host-distrobox.sh.
central_install_dir="${HOME}/.config/dualdeck/install"
self_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
container_name="dualdeck-host"

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

# Same two-candidate resolution as steam_shortcut_py above -- see
# client/internal/uninstall-steam-shortcut.sh's identical comment.
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
old_central_install_dir="${HOME}/.config/melonds-remote/install"
python3 "${steam_shortcut_py}" \
    --exe "${old_central_install_dir}/melonds-remote-host.sh" \
    --name "melonDS Remote Host" \
    --remove "$@" >/dev/null 2>&1 || true

if [[ -n "${steam_restart_helper}" ]]; then
    run_steam_shortcut_with_restart "${steam_shortcut_py}" "${error_log}" \
        --exe "${central_install_dir}/dualdeck-host.sh" \
        --name "DualDeck Host" \
        --remove \
        "$@"
else
    python3 "${steam_shortcut_py}" \
        --exe "${central_install_dir}/dualdeck-host.sh" \
        --name "DualDeck Host" \
        --remove \
        "$@"
fi

dry_run=0
for arg in "$@"; do
    [[ "${arg}" == "--dry-run" ]] && dry_run=1
done

if [[ "${dry_run}" -eq 0 ]]; then
    # Captured once, not piped into `grep -q` -- see
    # run-host-azahar.sh's comment for why `distrobox list | grep -q`
    # under pipefail can report "not found" for a container that is
    # plainly there, which in an uninstaller means silently leaving it
    # behind.
    distrobox_containers="$(command -v distrobox >/dev/null 2>&1 && distrobox list 2>/dev/null || true)"
    if grep -qw "${container_name}" <<<"${distrobox_containers}"; then
        echo "Removing Distrobox container \"${container_name}\" ..."
        distrobox rm "${container_name}" --force
    fi
    # Same melonDS-Remote -> DualDeck rebrand cleanup as the shortcut
    # removal above, for the container this project itself created
    # under the old name before the rename.
    if grep -qw "melonds-remote-host" <<<"${distrobox_containers}"; then
        echo "Removing old Distrobox container \"melonds-remote-host\" ..."
        distrobox rm "melonds-remote-host" --force
    fi

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
    # (see install-steam-shortcut.sh/install-host-distrobox.sh), not
    # inside any of the three directories just removed above.
    rm -f -- "$(dirname "${central_install_dir}")/check-for-updates.sh" \
             "$(dirname "${central_install_dir}")/VERSION"
fi
