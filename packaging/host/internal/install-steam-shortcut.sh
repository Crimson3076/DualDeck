#!/usr/bin/env bash
# Registers the DualDeck host as a Steam non-Steam-game shortcut,
# so it can be launched from Steam Big Picture/Gaming Mode with only a
# controller (GitHub issue #10). Mirrors
# ../../client/internal/install-steam-shortcut.sh's approach, reusing
# the same layout-agnostic steam_shortcut.py -- bundled flat alongside
# this script rather than shared from a top-level scripts/ directory,
# since host/ is fully self-contained (same reasoning as client/lib/
# bundling SDL3). Normally launched via ../../dualdeck-host.sh's
# "Add to Steam" menu choice, not directly.
#
# Copies the whole host/ directory (this script's grandparent -- the
# entry-point script and binary, plus this internal/ directory) into a
# fixed central location (~/.config/dualdeck/install/ -- the same
# directory install-host-distrobox.sh already uses on immutable
# systems) and points the shortcut at dualdeck-host.sh there --
# the same single entry point a double-click normally runs, showing its
# "Which system?" picker (DS/melonDS, 3DS/Azahar, host-control-only, or
# a custom emulator) -- not at melonDS, run-host.sh, or
# internal/launch-host.sh directly, so a Steam-launched session gets
# the same choice a manually-launched one does instead of always
# booting straight into melonDS. Re-running this from a newer release's
# extracted archive updates the same shortcut in place instead of
# leaving a stale duplicate pointing at a since-deleted download folder
# (same reasoning as the client's cross-release-directory fix).
#
# A failed update can't break a working install: on an immutable system,
# staging the files AND installing the container's packages is entirely
# delegated to install-host-distrobox.sh --install-only, whose own
# stage-then-swap logic only activates the new files once the package
# install actually succeeds (see that script) -- duplicating a separate
# file copy/swap here, ahead of the package-install step, would let a
# dnf failure leave new, not-yet-verified files active anyway, which is
# exactly the bug this is meant to prevent. On a regular (non-immutable)
# system there's no separate package-install step to gate on, so a
# plain stage-then-swap file copy here is already safe on its own.
#
# Real user report, 2026-08-03 (Bazzite): the persistent Host Control
# daemon's own unattended self-update (net_server.cpp's
# runSelfUpdateCommand(), backgrounded via `nohup ... &` with no TTY
# attached) was routing through this same immutable-system branch and
# hanging on install-host-distrobox.sh --install-only's `sudo dnf
# install` step -- no TTY means no way to authenticate sudo, so that
# step silently failed and the activation swap never ran, leaving
# install/ (including internal/lib/libturbojpeg.so.0) stuck on whatever
# was staged before the update. dualdeck-host-control.service never
# launches melonDS/Azahar/Cemu, so it never needs the Distrobox
# container's packages in the first place (see
# install-host-distrobox.sh's own DUALDECK_HOST_CONTROL check) --
# runSelfUpdateCommand() now sets DUALDECK_HOST_CONTROL=1 for exactly
# this reason, so a Host-Control-only self-update goes straight to the
# same lightweight, always-succeeds file swap the non-immutable branch
# below already uses instead of waiting on a dnf install nothing can
# authenticate. A regular interactive "Check for updates" click
# (DUALDECK_HOST_CONTROL unset) still goes through the full
# Distrobox-provisioning path as before, since that session might also
# launch melonDS/Azahar/Cemu and does need the container kept in sync.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
host_root="$(cd .. && pwd)"
# shellcheck source=scripts/lib/steam_restart_helper.sh
source ./steam_restart_helper.sh
# shellcheck source=scripts/lib/host_firewall.sh
source ./host_firewall.sh

# Surfaces failures visibly instead of just closing silently when
# double-clicked with no visible terminal attached -- logs to a
# persistent file and, when available (SteamOS Desktop Mode/Bazzite are
# both KDE Plasma), pops up a graphical error dialog via kdialog. Same
# pattern as client/internal/install-steam-shortcut.sh.
error_log="${HOME}/.config/dualdeck/install.log"
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") install-steam-shortcut.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "DualDeck Host" \
            --error "Installing the Steam shortcut failed: ${failing_cmd}
(exit code ${exit_code})

Details logged to:
${error_log}" 2>/dev/null || true
    fi
}
trap 'on_error "$?" "${LINENO}" "${BASH_COMMAND}"' ERR

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

# Keep in sync with the same constant in install-host-distrobox.sh,
# uninstall-host-distrobox.sh, and uninstall-steam-shortcut.sh.
central_install_dir="${HOME}/.config/dualdeck/install"
staging_dir="${central_install_dir}.new"
previous_dir="${central_install_dir}.previous"

# One-time melonDS-Remote -> DualDeck rebrand migration: this central
# install dir moved (old: ~/.config/melonds-remote/install) and the
# Steam AppName changed ("melonDS Remote Host" -> "DualDeck Host")
# simultaneously, so steam_shortcut.py's Exe-OR-AppName matching can't
# reliably find-and-update the old entry on its own. Explicitly remove
# the old entry by its old identity first, best-effort.
old_central_install_dir="${HOME}/.config/melonds-remote/install"
if [[ -d "${old_central_install_dir}" && "${dry_run}" -eq 0 ]]; then
    python3 ./steam_shortcut.py \
        --exe "${old_central_install_dir}/melonds-remote-host.sh" \
        --name "melonDS Remote Host" \
        --remove "${extra_args[@]}" >/dev/null 2>&1 || true
fi

if [[ "${dry_run}" -eq 0 ]]; then
    if { [[ -f /run/ostree-booted ]] || command -v rpm-ostree >/dev/null 2>&1; } && \
       [[ "${DUALDECK_HOST_CONTROL:-0}" != "1" ]]; then
        echo "Preparing the Distrobox container (this can take a few minutes the first time) ..."
        ./install-host-distrobox.sh --install-only
    else
        rm -rf "${staging_dir}"
        mkdir -p "${staging_dir}"
        cp -a "${host_root}/." "${staging_dir}/"
        find "${staging_dir}" -name '*.sh' -exec chmod +x {} +

        rm -rf "${previous_dir}"
        if [[ -d "${central_install_dir}" ]]; then
            mv "${central_install_dir}" "${previous_dir}"
        fi
        mv "${staging_dir}" "${central_install_dir}"

        # Same reasoning as install-host-distrobox.sh's equivalent copy:
        # keeps "../check-for-updates.sh" resolvable from a copy of
        # dualdeck-host.sh later run from inside the central
        # directory itself.
        cp "$(dirname "${host_root}")/check-for-updates.sh" "$(dirname "${central_install_dir}")/check-for-updates.sh" 2>/dev/null || true
        cp "$(dirname "${host_root}")/VERSION" "$(dirname "${central_install_dir}")/VERSION" 2>/dev/null || true
    fi

    # Best-effort, never fatal to the install itself -- see
    # scripts/lib/host_firewall.sh's own header for exactly what this
    # opens and why it's safe to just log-and-continue on failure
    # (real user report: the client couldn't reach the host at all,
    # even via a direct DualDeck Host GUI launch, and firewalld -- the
    # Bazzite/Fedora default -- blocking these ports was the likely
    # cause; this used to be a manual docs/bazzite-host-setup.md step).
    ensure_host_firewall_ports || true
fi

run_steam_shortcut_with_restart ./steam_shortcut.py "${error_log}" \
    --exe "${central_install_dir}/dualdeck-host.sh" \
    --name "DualDeck Host" \
    --launch-options "${launch_options}" \
    "${extra_args[@]}" && shortcut_exit=0 || shortcut_exit=$?
if [[ "${shortcut_exit}" -ne 0 ]]; then
    on_error "${shortcut_exit}" "${LINENO}" "steam_shortcut.py"
    exit "${shortcut_exit}"
fi
