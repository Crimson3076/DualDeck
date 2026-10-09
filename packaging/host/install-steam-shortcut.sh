#!/usr/bin/env bash
# Compatibility shim for releases before the host/internal/
# restructuring -- see the comment above this heredoc in
# scripts/build-release.sh for why this exists. Just forwards to the
# real script's current location.
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/internal/install-steam-shortcut.sh" "$@"
