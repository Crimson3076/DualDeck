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
# A failed update can't break a working install (GitHub issue #11): the
# new files are staged in a separate directory first and only swapped
# into place once staging succeeds, keeping the replaced version as a
# one-generation backup (*.previous) rather than deleting it outright.
#
# Usage:
#   ./scripts/install-steam-shortcut.sh                    # discovery mode, no fixed host
#   ./scripts/install-steam-shortcut.sh --host 192.168.1.50 # skip discovery, fixed host
#   ./scripts/install-steam-shortcut.sh --dry-run           # preview only, writes nothing
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_root}/build"
binary="${build_dir}/client/melonds-remote-client"

# Surfaces failures visibly instead of just closing silently when
# double-clicked with no visible terminal attached (GitHub issue #11) --
# logs to a persistent file and, when available (SteamOS Desktop Mode/
# Bazzite are both KDE Plasma), pops up a graphical error dialog via
# kdialog.
error_log="${HOME}/.config/melonds-remote-client/install.log"
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") install-steam-shortcut.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "melonDS Remote" \
            --error "Installing the Steam shortcut failed: ${failing_cmd}
(exit code ${exit_code})

Details logged to:
${error_log}" 2>/dev/null || true
    fi
}
trap 'ec=$?; on_error "${ec}" "${LINENO}" "${BASH_COMMAND}"' ERR

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
staging_dir="${central_install_dir}.new"
previous_dir="${central_install_dir}.previous"
exe="${central_install_dir}/client/melonds-remote-client"

if [[ "${dry_run}" -eq 0 ]]; then
    rm -rf "${staging_dir}"
    mkdir -p "${staging_dir}/client" "${staging_dir}/scripts/lib"

    cp "${binary}" "${staging_dir}/client/melonds-remote-client"
    chmod +x "${staging_dir}/client/melonds-remote-client"

    cp "${repo_root}/scripts/lib/steam_shortcut.py" "${staging_dir}/scripts/lib/steam_shortcut.py"

    cp "${repo_root}/scripts/uninstall-steam-shortcut.sh" "${staging_dir}/client/uninstall-steam-shortcut.sh"
    chmod +x "${staging_dir}/client/uninstall-steam-shortcut.sh"

    # Only reached if staging succeeded -- safe to activate now. Keeps
    # just one backup generation, not unbounded.
    rm -rf "${previous_dir}"
    if [[ -d "${central_install_dir}" ]]; then
        mv "${central_install_dir}" "${previous_dir}"
    fi
    mv "${staging_dir}" "${central_install_dir}"
fi

python3 "${repo_root}/scripts/lib/steam_shortcut.py" \
    --exe "${exe}" \
    --launch-options "${launch_options}" \
    "${extra_args[@]}" && shortcut_exit=0 || shortcut_exit=$?
if [[ "${shortcut_exit}" -ne 0 ]]; then
    on_error "${shortcut_exit}" "${LINENO}" "steam_shortcut.py"
    exit "${shortcut_exit}"
fi
