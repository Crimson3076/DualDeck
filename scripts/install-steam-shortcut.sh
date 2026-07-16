#!/usr/bin/env bash
# Registers melonds-remote-client as a Steam non-Steam-game shortcut, so
# it shows up in your Steam library (and Big Picture/Gaming Mode) without
# manually going through Games -> Add a Non-Steam Game -> Browse... every
# time you rebuild it. See scripts/lib/steam_shortcut.py for exactly what
# this does and why it's careful about it (backs up shortcuts.vdf first,
# refuses to run while Steam is open unless --force).
#
# Usage:
#   ./scripts/install-steam-shortcut.sh                    # discovery mode, no fixed host
#   ./scripts/install-steam-shortcut.sh --host 192.168.1.50 # skip discovery, fixed host
#   ./scripts/install-steam-shortcut.sh --dry-run           # preview only, writes nothing
#
# After running this, restart Steam (or just switch to Gaming Mode) and
# the shortcut appears in your library. You still need to set its
# Controller Layout to a plain "Gamepad" template by hand once --
# see docs/steam-deck-setup.md's Gaming Mode section for why.
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
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run|--force)
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

exec python3 "${repo_root}/scripts/lib/steam_shortcut.py" \
    --exe "${binary}" \
    --launch-options "${launch_options}" \
    "${extra_args[@]}"
