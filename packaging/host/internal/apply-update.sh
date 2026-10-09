#!/usr/bin/env bash
# Downloads the latest DualDeck release and installs it, by
# handing off to that release's own
# host/internal/install-steam-shortcut.sh --force -- reusing its
# already-verified stage-then-swap file safety and Distrobox/dnf-gated
# activation on immutable systems, rather than duplicating any of that
# logic here. This script's only job is fetching and extracting the
# new release archive. Normally invoked from
# ../../dualdeck-host.sh's "Check for updates" menu choice after
# the user confirms; also runnable standalone.
#
# Passes --force through to install-steam-shortcut.sh so an update
# doesn't silently do nothing just because Steam happens to be open --
# the confirmation prompt in the menu is the actual "are you sure" gate
# here, not that check. In practice --force rarely matters: since GitHub
# issue "atomic updates -- Steam shouldn't need to be closed to update",
# install-steam-shortcut.sh only writes shortcuts.vdf when the
# shortcut's Exe/AppName/StartDir/LaunchOptions actually differ from
# what's already there, which never happens for a routine version bump
# (see steam_shortcut.py's shortcut_up_to_date()) -- so a normal update
# never touches shortcuts.vdf at all and Steam never needs to be closed
# or restarted for it. If Steam genuinely was never set up on this
# machine, installing the Steam shortcut part fails visibly (its own
# error-trap logs and shows a dialog) while the files themselves are
# still updated either way, since that copy step doesn't depend on
# Steam at all.
#
# Only ever downloads from this exact, hardcoded GitHub Releases URL
# over HTTPS -- never anything derived from user input, an environment
# variable, or a config file.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

error_log="${HOME}/.config/dualdeck/install.log"
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") apply-update.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "DualDeck Host" --error "Updating failed: ${failing_cmd}
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
curl -fsSL --max-time 180 -o "${work_dir}/release.tar.gz" "${download_url}"

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

# Real user request: "it should also reset on updates, so toggle off and
# back on if it was already running" -- checked *before* the install
# step below, since that step only replaces files on disk, it doesn't
# touch whatever copy of dualdeck-host-service the persistent daemon
# (if running) already has open. `systemctl --user restart` after the
# install is a single atomic "toggle off and back on," matching the
# request exactly. Guarded with `|| true`/an explicit warning rather
# than left to the ERR trap above: a failed restart here is real but
# distinct from an update itself failing (the update has already fully
# succeeded by this point), so it shouldn't produce the same "Updating
# failed" kdialog error as an actual download/install failure would.
daemon_was_active=0
if command -v systemctl >/dev/null 2>&1 && \
   systemctl --user is-active --quiet dualdeck-host-control.service 2>/dev/null; then
    daemon_was_active=1
fi

echo "Installing..."
# Not exec'd: the work_dir EXIT trap above must still fire to clean up
# the download afterward, which exec'ing over this process would skip.
"${extracted_dir}/host/internal/install-steam-shortcut.sh" --force

# Each DualDeck-patched emulator AppImage carries its own copy of
# dualdeck-host-service (see apprun_templates.sh), which the install
# step above never touches -- real report, 2026-10-09: after updating,
# Cemu kept running the old host service and PyroWave silently fell back
# to JPEG. Re-patch only the emulators DualDeck already patched (see
# emudeck-replace-in-place.sh's --refresh-installed). Non-fatal:
# the DualDeck update itself already succeeded, and a stale emulator is
# recoverable later from the host menu's EmuDeck integration entry.
refresh_tool="${extracted_dir}/host/emudeck-integration/scripts/emudeck-replace-in-place.sh"
if [[ -x "${refresh_tool}" ]]; then
    echo "Refreshing DualDeck-patched emulators..."
    "${refresh_tool}" --refresh-installed ||
        echo "warning: couldn't refresh patched emulator AppImages -- re-run the EmuDeck integration from the host menu" >&2
fi

if [[ "${daemon_was_active}" -eq 1 ]]; then
    echo "Restarting the Host Control daemon to pick up the update..."
    if ! systemctl --user restart dualdeck-host-control.service 2>/dev/null; then
        echo "warning: could not restart dualdeck-host-control.service -- restart it manually" >&2
        echo "(systemctl --user restart dualdeck-host-control.service)" >&2
    fi
fi
