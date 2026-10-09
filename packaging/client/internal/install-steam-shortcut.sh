#!/usr/bin/env bash
# Registers dualdeck-client as a Steam non-Steam-game shortcut --
# see ./steam_shortcut.py for exactly what this does and why it's
# careful about it (backs up shortcuts.vdf first, refuses to run while
# Steam is open unless --force). Any arguments given here are passed
# through as the shortcut's launch options, e.g.:
#   ./install-steam-shortcut.sh --host 192.168.1.50
# Normally launched via ../../dualdeck-client.sh's "Add to Steam"
# menu choice, not directly.
#
# Copies the whole client/ directory (this script's parent) into a
# fixed central location -- see central_install_dir below -- and points
# the Steam shortcut at run-client.sh there (specifically, not the raw
# binary, so the bundled SDL3 library is found via LD_LIBRARY_PATH).
# That way, re-running this from a newer release's extracted archive
# later always updates the same shortcut instead of leaving a stale
# duplicate around pointing at a now-deleted download folder, and the
# extracted archive can be deleted once this has run --
# uninstall-steam-shortcut.sh only ever needs the central copy.
#
# A failed update can't break a working install (GitHub issue #11): the
# new files are staged in a separate directory first and only swapped
# into place once staging succeeds, keeping the replaced version as a
# one-generation backup (*.previous) rather than deleting it outright.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
client_root="$(cd .. && pwd)"
# shellcheck source=scripts/lib/steam_restart_helper.sh
source ./steam_restart_helper.sh

# Surfaces failures visibly instead of just closing silently when
# double-clicked with no visible terminal attached (GitHub issue #11) --
# logs to a persistent file and, when available (SteamOS Desktop Mode/
# Bazzite are both KDE Plasma), pops up a graphical error dialog via
# kdialog.
error_log="${HOME}/.config/dualdeck-client/install.log"
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") install-steam-shortcut.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "DualDeck" \
            --error "Installing the Steam shortcut failed: ${failing_cmd}
(exit code ${exit_code})

Details logged to:
${error_log}" 2>/dev/null || true
    fi
}
trap 'ec=$?; on_error "${ec}" "${LINENO}" "${BASH_COMMAND}"' ERR

launch_options=""
extra_args=()
dry_run=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) extra_args+=("$1"); dry_run=1; shift ;;
        --force) extra_args+=("$1"); shift ;;
        --user) extra_args+=("$1" "$2"); shift 2 ;;
        --user=*) extra_args+=("$1"); shift ;;
        *) launch_options+="${launch_options:+ }$1"; shift ;;
    esac
done

# Keep in sync with the same constant in uninstall-steam-shortcut.sh and
# apply-update.sh.
central_install_dir="${HOME}/.config/dualdeck-client/install"
staging_dir="${central_install_dir}.new"
previous_dir="${central_install_dir}.previous"

# One-time melonDS-Remote -> DualDeck rebrand migration: this central
# install dir moved (old: ~/.config/melonds-remote-client/install) and
# the Steam AppName changed ("melonDS Remote" -> "DualDeck")
# simultaneously, so steam_shortcut.py's Exe-OR-AppName matching can't
# reliably find-and-update the old entry on its own. Explicitly remove
# the old entry by its old identity first, best-effort.
old_central_install_dir="${HOME}/.config/melonds-remote-client/install"
if [[ -d "${old_central_install_dir}" && "${dry_run}" -eq 0 ]]; then
    python3 ./steam_shortcut.py \
        --exe "${old_central_install_dir}/internal/run-client.sh" \
        --name "melonDS Remote" \
        --remove "${extra_args[@]}" >/dev/null 2>&1 || true
fi

if [[ "${dry_run}" -eq 0 ]]; then
    rm -rf "${staging_dir}"
    mkdir -p "${staging_dir}"
    cp -a "${client_root}/." "${staging_dir}/"
    find "${staging_dir}" -name '*.sh' -exec chmod +x {} +

    # Only reached if staging succeeded -- safe to activate now. Keeps
    # just one backup generation, not unbounded.
    rm -rf "${previous_dir}"
    if [[ -d "${central_install_dir}" ]]; then
        mv "${central_install_dir}" "${previous_dir}"
    fi
    mv "${staging_dir}" "${central_install_dir}"

    # Same reasoning as the host equivalent in install-host-distrobox.sh:
    # keeps "../check-for-updates.sh" (the menu's "Check for updates")
    # and run-client.sh's own auto-update check both resolvable from a
    # copy of this install later run from inside the central directory
    # itself, after the original downloaded archive is gone.
    cp "$(dirname "${client_root}")/check-for-updates.sh" "$(dirname "${central_install_dir}")/check-for-updates.sh" 2>/dev/null || true
    cp "$(dirname "${client_root}")/VERSION" "$(dirname "${central_install_dir}")/VERSION" 2>/dev/null || true
fi

run_steam_shortcut_with_restart ./steam_shortcut.py "${error_log}" \
    --exe "${central_install_dir}/internal/run-client.sh" \
    --name "DualDeck" \
    --launch-options "${launch_options}" \
    "${extra_args[@]}" && shortcut_exit=0 || shortcut_exit=$?
if [[ "${shortcut_exit}" -ne 0 ]]; then
    on_error "${shortcut_exit}" "${LINENO}" "steam_shortcut.py"
    exit "${shortcut_exit}"
fi

# Real user request: auto-apply the "DualDeck control scheme" (Steam
# Input disabled for this shortcut -- see
# configure-trackpad-experiment.sh's own header for what that actually
# means and why it's the real fix, not a custom Steam Input layout) the
# first time it's ever available, instead of leaving it undiscoverable
# behind a menu/Settings toggle. This runs on both a fresh "Add to
# Steam" AND every update (apply-update.sh calls this script with
# --force), so it reaches existing installs too, not just new ones.
#
# Gated by a one-time marker rather than a live Steam Input status
# check, which can't tell "never touched" apart from "the user
# deliberately turned it back on later" -- so this applies at most
# once, ever, per install, and never re-fights a later manual choice.
# Only written once configure-trackpad-experiment.sh actually succeeds,
# so a failed attempt (e.g. it's genuinely missing from this install)
# retries on the next update instead of silently giving up forever.
#
# --no-restart: writes with --force (safe, it's just a text-file edit)
# and never kills Steam out from under whatever launched this -- a
# routine "check for updates" can run from inside a Steam-launched
# client in Gaming Mode (see configure-trackpad-experiment.sh's own
# comment on exactly this). The change lands next time Steam itself
# restarts, same as every other setting this wrapper touches. Silent
# by design (no dialog) -- best-effort and logged, not user-facing.
control_scheme_marker="${HOME}/.config/dualdeck-client/.steam-input-auto-configured"
trackpad_experiment_script="${central_install_dir}/internal/configure-trackpad-experiment.sh"
if [[ "${dry_run}" -eq 0 && ! -f "${control_scheme_marker}" && -x "${trackpad_experiment_script}" ]]; then
    if "${trackpad_experiment_script}" --no-restart >>"${error_log}" 2>&1; then
        mkdir -p "$(dirname "${control_scheme_marker}")"
        touch "${control_scheme_marker}"
    fi
fi
