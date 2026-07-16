#!/usr/bin/env bash
# Removes the melonDS Remote Steam non-Steam-game shortcut added by
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

# Keep in sync with the same constant in scripts/install-steam-shortcut.sh
# and the packaged client/install-steam-shortcut.sh / uninstall-steam-shortcut.sh
# heredocs in scripts/build-release.sh.
central_install_dir="${HOME}/.config/melonds-remote-client/install"
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

python3 "${steam_shortcut_py}" \
    --exe "${central_install_dir}/client/melonds-remote-client" \
    --remove \
    "$@"

dry_run=0
for arg in "$@"; do
    [[ "${arg}" == "--dry-run" ]] && dry_run=1
done

if [[ "${dry_run}" -eq 0 && -d "${central_install_dir}" ]]; then
    echo "Removing ${central_install_dir}"
    cd /
    rm -rf -- "${central_install_dir}"
fi
