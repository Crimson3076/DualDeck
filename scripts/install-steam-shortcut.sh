#!/usr/bin/env bash
# Registers melonds-remote-client as a Steam non-Steam-game shortcut, so
# it shows up in your Steam library (and Big Picture/Gaming Mode) without
# manually going through Games -> Add a Non-Steam Game -> Browse... every
# time you rebuild it. See scripts/lib/steam_shortcut.py for exactly what
# this does and why it's careful about it (backs up shortcuts.vdf first,
# refuses to run while Steam is open unless --force).
#
# Copies the built binary (plus everything needed to uninstall it later)
# into a fixed central directory -- see central_install_dir below -- and
# points the Steam shortcut at that copy rather than at this build tree
# directly. That way, re-running this after a rebuild always updates the
# same shortcut instead of leaving a stale duplicate around, and
# uninstall-steam-shortcut.sh keeps working even if this checkout moves
# or is deleted later.
#
# Usage:
#   ./scripts/install-steam-shortcut.sh                    # discovery mode, no fixed host
#   ./scripts/install-steam-shortcut.sh --host 192.168.1.50 # skip discovery, fixed host
#   ./scripts/install-steam-shortcut.sh --dry-run           # preview only, writes nothing
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_root}/build"
binary="${build_dir}/client/melonds-remote-client"

if [[ ! -x "${binary}" ]]; then
    echo "Client binary not found, building..." >&2
    cmake -S "${repo_root}" -B "${build_dir}" -DMELONDS_REMOTE_BUILD_CLIENT=ON
    cmake --build "${build_dir}" -j"$(nproc)"
fi

launch_options=""
extra_args=()
dry_run=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run)
            extra_args+=("$1")
            dry_run=1
            shift
            ;;
        --force)
            extra_args+=("$1")
            shift
            ;;
        --user)
            extra_args+=("$1" "$2")
            shift 2
            ;;
        --user=*)
            extra_args+=("$1")
            shift
            ;;
        *)
            launch_options+="${launch_options:+ }$1"
            shift
            ;;
    esac
done

# Keep in sync with the same constant in scripts/uninstall-steam-shortcut.sh
# and the packaged client/install-steam-shortcut.sh / uninstall-steam-shortcut.sh
# heredocs in scripts/build-release.sh.
central_install_dir="${HOME}/.config/melonds-remote-client/install"
exe="${central_install_dir}/client/melonds-remote-client"

if [[ "${dry_run}" -eq 0 ]]; then
    rm -rf "${central_install_dir}"
    mkdir -p "${central_install_dir}/client" "${central_install_dir}/scripts/lib"

    cp "${binary}" "${central_install_dir}/client/melonds-remote-client"
    chmod +x "${central_install_dir}/client/melonds-remote-client"

    cp "${repo_root}/scripts/lib/steam_shortcut.py" "${central_install_dir}/scripts/lib/steam_shortcut.py"

    cp "${repo_root}/scripts/uninstall-steam-shortcut.sh" "${central_install_dir}/client/uninstall-steam-shortcut.sh"
    chmod +x "${central_install_dir}/client/uninstall-steam-shortcut.sh"
fi

exec python3 "${repo_root}/scripts/lib/steam_shortcut.py" \
    --exe "${exe}" \
    --launch-options "${launch_options}" \
    "${extra_args[@]}"
