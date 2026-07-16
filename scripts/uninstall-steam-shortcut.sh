#!/usr/bin/env bash
# Removes the melonds-remote-client Steam non-Steam-game shortcut added by
# install-steam-shortcut.sh. See scripts/lib/steam_shortcut.py for exactly
# what this does and why it's careful about it (backs up shortcuts.vdf
# first, refuses to run while Steam is open unless --force). Matches by
# the client binary's expected path, so this still works even if that
# binary has since been deleted or the build/ directory removed.
#
# Usage:
#   ./scripts/uninstall-steam-shortcut.sh            # remove for all local Steam users
#   ./scripts/uninstall-steam-shortcut.sh --dry-run   # preview only, writes nothing
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${repo_root}/build/client/melonds-remote-client"

exec python3 "${repo_root}/scripts/lib/steam_shortcut.py" \
    --exe "${binary}" \
    --remove \
    "$@"
