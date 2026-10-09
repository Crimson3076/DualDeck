#!/usr/bin/env bash
# Compatibility shim: every release before this host/internal/
# restructuring had install-steam-shortcut.sh directly at host/, and
# those releases' own host/apply-update.sh hardcodes exactly that path
# when it downloads and invokes a newer release's copy (see
# docs/history.md). That already-installed
# old script can't be changed retroactively, so a real user on one of
# those versions hit exactly this: "Check for updates" downloads this
# new, restructured release fine, then fails with exit 127 trying to
# run a file that no longer exists at the old flat path. This shim
# forwards to the real (current) location so updating *from* one of
# those older releases keeps working. New installs/updates never
# reach this file directly -- dualdeck-host.sh and apply-update.sh
# both already call internal/install-steam-shortcut.sh -- so this exists
# purely for that one-time upgrade path and is safe to delete once no
# supported release still depends on it.
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/internal/install-steam-shortcut.sh" "$@"
