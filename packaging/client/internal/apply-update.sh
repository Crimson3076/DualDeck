#!/usr/bin/env bash
# Downloads the latest DualDeck release and installs the client
# part, by handing off to that release's own
# client/internal/install-steam-shortcut.sh --force -- reusing its
# already-verified stage-then-swap file safety rather than duplicating
# any of that logic here. This script's only job is fetching and
# extracting the new release archive. Normally invoked from
# ../../dualdeck-client.sh's "Check for updates" menu choice
# after the user confirms; also runnable standalone. See
# host/internal/apply-update.sh for the host equivalent (same design,
# minus the Distrobox complexity the client doesn't need).
#
# --force here does not mean "always touch shortcuts.vdf": since
# GitHub issue "atomic updates -- Steam shouldn't need to be closed to
# update", install-steam-shortcut.sh only writes shortcuts.vdf when the
# shortcut's Exe/AppName/StartDir/LaunchOptions actually differ from
# what's already there (see steam_shortcut.py's shortcut_up_to_date()) --
# which never happens for a routine version bump, since those are all
# derived from the fixed central install directory, not the release
# version. --force only matters for the rare case something genuinely
# needs to change; a plain update's file swap (already atomic via
# stage-then-rename) is the only thing that happens, and Steam never
# needs to be closed or restarted for it.
#
# Only ever downloads from this exact, hardcoded GitHub Releases URL
# over HTTPS -- never anything derived from user input, an environment
# variable, or a config file.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

error_log="${HOME}/.config/dualdeck-client/install.log"
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") apply-update.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "DualDeck" --error "Updating failed: ${failing_cmd}
(exit code ${exit_code})

Details logged to:
${error_log}" 2>/dev/null || true
    fi
}
trap 'ec=$?; on_error "${ec}" "${LINENO}" "${BASH_COMMAND}"' ERR

if ! command -v curl >/dev/null 2>&1; then
    echo "error: curl is required to download updates -- install it, or download" >&2
    echo "the latest release manually from https://github.com/Crimson3076/DualDeck/releases/latest" >&2
    exit 1
fi

repo="Crimson3076/DualDeck"
download_url="https://github.com/${repo}/releases/latest/download/melonds-remote-linux-x86_64.tar.gz"

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

echo "Downloading the latest release..."
curl --proto =https -fsSL --max-time 180 -o "${work_dir}/release.tar.gz" "${download_url}"

echo "Extracting..."
tar xzf "${work_dir}/release.tar.gz" -C "${work_dir}"

extracted_dir=""
for candidate in "${work_dir}"/melonds-remote-*; do
    [[ -d "${candidate}" ]] && extracted_dir="${candidate}" && break
done
if [[ -z "${extracted_dir}" ]]; then
    echo "error: couldn't find the extracted release directory" >&2
    exit 1
fi

echo "Installing..."
# Not exec'd: the work_dir EXIT trap above must still fire to clean up
# the download afterward, which exec'ing over this process would skip.
"${extracted_dir}/client/internal/install-steam-shortcut.sh" --force
