#!/usr/bin/env bash
# Installs and enables the persistent Host Control systemd --user
# service (dualdeck-host-control.service) -- see host-control-
# daemon.sh's own comment for why this exists at all. Idempotent: safe
# to re-run on every install/update (matching install-steam-shortcut.sh's
# own convention). Normally invoked from ../../dualdeck-host.sh's "Host
# Control daemon" menu choice, not directly.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

error_log="${HOME}/.config/dualdeck/install.log"
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") install-host-control-daemon.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
}
trap 'ec=$?; on_error "${ec}" "${LINENO}" "${BASH_COMMAND}"' ERR

# Real Bazzite hardware report, 2026-08-02: this daemon used to refuse
# to install at all on immutable/rpm-ostree systems here, on the same
# now-outdated "needs a Distrobox container" assumption
# install-host-distrobox.sh's own DUALDECK_HOST_CONTROL check used to
# make (see that check's own updated comment) -- dualdeck-host-service
# links no Qt/SDL, only libturbojpeg, now bundled alongside it (see
# build-release.sh's bundle_library_dependencies() call), so it needs no
# container on any system. What this unit actually writes to
# (${unit_dir} below, under $HOME) and execs
# (host-control-daemon.sh -> the now-self-contained dualdeck-host-service)
# both work identically whether the base OS is immutable or not -- the
# only real prerequisite is a working systemd --user manager, checked
# next.
#
# Checks a *working* --user manager is actually reachable, not just that
# the systemctl binary exists -- a machine can have systemd installed
# without a lingering/active --user instance for this account (e.g. no
# graphical login has ever happened yet).
if ! command -v systemctl >/dev/null 2>&1 || ! systemctl --user status >/dev/null 2>&1; then
    echo "systemd --user isn't available on this system -- the persistent Host" >&2
    echo "Control daemon can't be installed here. Use the manual 'Host control" >&2
    echo "only' launch option from the menu instead." >&2
    exit 0
fi

unit_dir="${HOME}/.config/systemd/user"
mkdir -p "${unit_dir}"

# %h is systemd's own home-directory expansion (not a bash variable --
# this heredoc is deliberately quoted so nothing here is bash-expanded),
# so this unit keeps working correctly even if $HOME ever changes.
cat > "${unit_dir}/dualdeck-host-control.service" <<'EOF'
[Unit]
Description=DualDeck Host Control (persistent background service)
After=network.target

[Service]
Type=simple
ExecStart=%h/.config/dualdeck/install/internal/host-control-daemon.sh
Restart=on-failure
RestartSec=2

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload
echo "Host Control daemon unit installed. Enable it with:"
echo "  systemctl --user enable --now dualdeck-host-control.service"
