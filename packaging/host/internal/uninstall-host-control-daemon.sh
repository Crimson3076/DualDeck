#!/usr/bin/env bash
# Undoes install-host-control-daemon.sh: stops, disables, and removes
# the persistent Host Control systemd --user service. Safe to run even
# if it was never installed (every step degrades to a harmless no-op).
# Called both standalone (../../dualdeck-host.sh's "Stop & disable"
# menu choice) and from the same script's full "Remove from Steam /
# uninstall" flow, so a full uninstall doesn't leave this running
# against a now-deleted install.
set -euo pipefail

if command -v systemctl >/dev/null 2>&1; then
    systemctl --user disable --now dualdeck-host-control.service >/dev/null 2>&1 || true
fi
rm -f "${HOME}/.config/systemd/user/dualdeck-host-control.service"
if command -v systemctl >/dev/null 2>&1; then
    systemctl --user daemon-reload >/dev/null 2>&1 || true
fi
