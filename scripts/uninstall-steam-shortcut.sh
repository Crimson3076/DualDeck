#!/usr/bin/env bash
# Removes the DualDeck Steam non-Steam-game shortcut added by
# install-steam-shortcut.sh, and deletes the central install directory
# install-steam-shortcut.sh copies everything into. See
# scripts/lib/steam_shortcut.py for exactly what the shortcut-removal
# part does and why it's careful about it (backs up shortcuts.vdf first,
# refuses to run while Steam is open unless --force).
#
# Always targets the fixed central install directory below for the
# actual --exe match, regardless of where this script itself is run
# from: this exact file is also the one copied into that central
# directory by install-steam-shortcut.sh, and must keep removing the
# same shortcut from there indefinitely, even after this checkout (or
# whatever release archive it came from) has been deleted. It does fall
# back to a locally-sibling copy of steam_shortcut.py (see below) purely
# so a freshly downloaded copy of this script can still clean up a
# shortcut from an older version of this project that was never
# migrated to the central directory -- steam_shortcut.py's own AppName
# fallback (see its module docstring) is what actually finds and removes
# that kind of stale entry.
#
# Usage:
#   ./scripts/uninstall-steam-shortcut.sh            # remove for all local Steam users
#   ./scripts/uninstall-steam-shortcut.sh --dry-run   # preview only, writes nothing
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

# Keep in sync with the same constant in scripts/install-steam-shortcut.sh
# and the packaged client/install-steam-shortcut.sh / uninstall-steam-shortcut.sh
# heredocs in scripts/build-release.sh.
central_install_dir="${HOME}/.config/dualdeck-client/install"
self_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

steam_shortcut_py=""
for candidate in \
    "${central_install_dir}/scripts/lib/steam_shortcut.py" \
    "${self_dir}/lib/steam_shortcut.py" \
    "${self_dir}/../scripts/lib/steam_shortcut.py"
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

dry_run=0
for arg in "$@"; do
    [[ "${arg}" == "--dry-run" ]] && dry_run=1
done

# One-time melonDS-Remote -> DualDeck rebrand cleanup: an old install's
# central dir/Exe/AppName all differ from the current ones (see
# install-steam-shortcut.sh's matching comment for why the Exe-OR-AppName
# fallback alone can't bridge that compound a change), so also try
# removing under the old identity, best-effort. No-op if it was never
# installed there or was already migrated.
old_central_install_dir="${HOME}/.config/melonds-remote-client/install"
python3 "${steam_shortcut_py}" \
    --exe "${old_central_install_dir}/client/melonds-remote-client" \
    --name "melonDS Remote" \
    --remove "$@" >/dev/null 2>&1 || true

python3 "${steam_shortcut_py}" \
    --exe "${central_install_dir}/client/dualdeck-client" \
    --name "DualDeck" \
    --remove \
    "$@"

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
fi
