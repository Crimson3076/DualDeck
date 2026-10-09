#!/usr/bin/env bash
# Checks whether a newer DualDeck release is published on GitHub --
# read-only, no download or install of anything happens here. Both
# dualdeck-host.sh and dualdeck-client.sh's "Check for
# updates" menu choice call this first and only offer to actually
# install if it reports one available. Capped at 5s network time and
# never exits non-zero on a reachability/parse failure, specifically so
# nothing that calls this automatically -- run-client.sh does, on every
# launch, applying an update silently if one's found -- ever blocks or
# fails a launch waiting on it. run-host.sh does not call this
# automatically (the host has no equivalent auto-update yet); run this
# yourself (or use the host menu) to check there.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

current_version="$(cat VERSION 2>/dev/null || echo "unknown")"
repo="Crimson3076/DualDeck"

if ! command -v curl >/dev/null 2>&1; then
    echo "DualDeck ${current_version} -- install 'curl' to enable update checks."
    exit 0
fi

api_response="$(curl --proto =https -fsSL --max-time 5 \
    "https://api.github.com/repos/${repo}/releases/latest" 2>/dev/null)"
if [[ -z "${api_response}" ]]; then
    echo "DualDeck ${current_version} -- couldn't reach GitHub to check for updates (offline?)."
    exit 0
fi

latest_version="$(echo "${api_response}" | grep -o '"tag_name" *: *"[^"]*"' | head -1 | sed -E 's/.*"([^"]+)"$/\1/')"
if [[ -z "${latest_version}" ]]; then
    echo "DualDeck ${current_version} -- couldn't parse GitHub's response to check for updates."
    exit 0
fi

if [[ "${latest_version}" == "${current_version}" ]]; then
    echo "DualDeck ${current_version} -- you're on the latest version."
else
    echo "DualDeck ${current_version} -- update available: ${latest_version}"
    echo "  https://github.com/${repo}/releases/tag/${latest_version}"
fi
