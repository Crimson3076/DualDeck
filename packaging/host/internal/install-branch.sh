#!/usr/bin/env bash
# Advanced -> Installation branch -> "Install selected branch". Resolves
# the currently-selected branch (dualdeck_branch_get_selected) to the
# newest published release built exactly at that branch's current tip
# commit, downloads and checksum-verifies *that specific release* (not
# "latest" -- see download_base below), and only then hands off to the
# same already-atomic host/internal/install-steam-shortcut.sh --force
# every other install/update path uses (stage in a .new sibling, swap
# into place only once staging succeeds, one generation kept as
# .previous). Nothing is recorded as installed (dualdeck_branch_
# record_installed) unless that whole sequence actually completes -- a
# failure at any earlier step leaves the previous install running
# untouched and reports exactly what went wrong, never a fabricated
# fallback to another branch. See client/internal/install-branch.sh for
# the client equivalent (same design) and dualdeck_branch.sh's own
# header comment for why resolving the identical branch name
# independently on both machines is what makes "same commit on both"
# hold, rather than a live cross-machine transaction.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

error_log="${HOME}/.config/dualdeck/install.log"
# shellcheck source=scripts/lib/release_install.sh
source ./release_install.sh
dualdeck_trap_errors "DualDeck Host" "Installing the selected branch failed"

if ! command -v curl >/dev/null 2>&1; then
    echo "error: curl is required -- install it and try again." >&2
    exit 1
fi
if ! command -v sha256sum >/dev/null 2>&1; then
    echo "error: sha256sum is required to verify the download -- install coreutils and try again." >&2
    exit 1
fi

export DUALDECK_BRANCH_CONFIG_DIR="${HOME}/.config/dualdeck"
# shellcheck source=scripts/lib/dualdeck_branch.sh
source ./dualdeck_branch.sh

dualdeck_resolve_selected_branch

repo="Crimson3076/DualDeck"
download_base="https://github.com/${repo}/releases/download/${resolved_tag}"

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

echo "Downloading ${resolved_tag} (branch ${branch}, commit ${resolved_sha:0:7})..."
dualdeck_fetch_release "${download_base}" "${work_dir}" 1

# Same "toggle off and back on" reasoning as apply-update.sh's own
# identical block -- checked before the install step below, since that
# step only replaces files on disk, not whatever copy the persistent
# daemon (if running) already has open.
daemon_was_active=0
if command -v systemctl >/dev/null 2>&1 && \
   systemctl --user is-active --quiet dualdeck-host-control.service 2>/dev/null; then
    daemon_was_active=1
fi

echo "Installing..."
"${extracted_dir}/host/internal/install-steam-shortcut.sh" --force

# Only recorded once the staged swap above has actually completed.
dualdeck_branch_record_installed "${branch}" "${resolved_sha}" "${resolved_tag}"
echo "Installed ${resolved_tag} (branch ${branch}, commit ${resolved_sha:0:7})."

# Each DualDeck-patched emulator AppImage carries its own copy of
# dualdeck-host-service (see apprun_templates.sh), which the install
# step above never touches -- real report, 2026-10-09: after updating,
# Cemu kept running the old host service and PyroWave silently fell back
# to JPEG. Re-patch only the emulators DualDeck already patched (see
# emudeck-replace-in-place.sh's --refresh-installed), from this
# same branch release rather than the latest one. Non-fatal:
# the DualDeck update itself already succeeded, and a stale emulator is
# recoverable later from the host menu's EmuDeck integration entry.
refresh_tool="${extracted_dir}/host/emudeck-integration/scripts/emudeck-replace-in-place.sh"
if [[ -x "${refresh_tool}" ]]; then
    echo "Refreshing DualDeck-patched emulators..."
    DUALDECK_REPLACE_DOWNLOAD_BASE="${download_base}" "${refresh_tool}" --refresh-installed ||
        echo "warning: couldn't refresh patched emulator AppImages -- re-run the EmuDeck integration from the host menu" >&2
fi

if [[ "${daemon_was_active}" -eq 1 ]]; then
    echo "Restarting the Host Control daemon to pick up the update..."
    if ! systemctl --user restart dualdeck-host-control.service 2>/dev/null; then
        echo "warning: could not restart dualdeck-host-control.service -- restart it manually" >&2
        echo "(systemctl --user restart dualdeck-host-control.service)" >&2
    fi
fi
