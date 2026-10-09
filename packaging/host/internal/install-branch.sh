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
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") install-branch.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "DualDeck Host" --error "Installing the selected branch failed: ${failing_cmd}
(exit code ${exit_code})

Details logged to:
${error_log}" 2>/dev/null || true
    fi
}
trap 'ec=$?; on_error "${ec}" "${LINENO}" "${BASH_COMMAND}"' ERR

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

branch="$(dualdeck_branch_get_selected)"
if [[ -z "${branch}" ]]; then
    echo "No branch selected -- open Advanced -> Installation branch and pick one first." >&2
    exit 1
fi

resolve_rc=0
resolved="$(dualdeck_branch_resolve "${branch}")" || resolve_rc=$?
case "${resolve_rc}" in
    0) : ;;
    2)
        echo "error: GitHub API rate limit hit while resolving '${branch}' -- try again later. Nothing installed." >&2
        exit 1
        ;;
    3)
        echo "error: branch '${branch}' no longer exists on GitHub (deleted or renamed?). Nothing installed." >&2
        exit 1
        ;;
    4)
        echo "error: no DualDeck release has been published from branch '${branch}''s current commit yet -- nothing to install. Nothing changed." >&2
        exit 1
        ;;
    5)
        echo "error: '${branch}' is not a valid branch name. Nothing installed." >&2
        exit 1
        ;;
    *)
        echo "error: couldn't reach GitHub to resolve branch '${branch}' (offline?). Nothing installed." >&2
        exit 1
        ;;
esac
resolved_sha="$(printf '%s' "${resolved}" | cut -f1)"
resolved_tag="$(printf '%s' "${resolved}" | cut -f2)"

repo="Crimson3076/DualDeck"
download_base="https://github.com/${repo}/releases/download/${resolved_tag}"

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

echo "Downloading ${resolved_tag} (branch ${branch}, commit ${resolved_sha:0:7})..."
# Saved under the archive's real basename, not an arbitrary local name --
# sha256sum -c matches SHA256SUMS entries by exact filename, and
# SHA256SUMS (see build-release.sh's own SHA256SUMS-generation comment)
# lists this exact name.
archive_name="melonds-remote-linux-x86_64.tar.gz"
curl --proto =https -fsSL --max-time 180 -o "${work_dir}/${archive_name}" "${download_base}/${archive_name}"
curl --proto =https -fsSL --max-time 30 -o "${work_dir}/SHA256SUMS" "${download_base}/SHA256SUMS"

echo "Verifying download integrity..."
if ! (cd "${work_dir}" && sha256sum -c --ignore-missing SHA256SUMS) >/dev/null 2>&1; then
    echo "error: checksum verification failed for ${resolved_tag} -- refusing to install an unverified download. Nothing changed." >&2
    exit 1
fi

echo "Extracting..."
tar xzf "${work_dir}/${archive_name}" -C "${work_dir}"

extracted_dir=""
for candidate in "${work_dir}"/melonds-remote-*; do
    [[ -d "${candidate}" ]] && extracted_dir="${candidate}" && break
done
if [[ -z "${extracted_dir}" ]]; then
    echo "error: couldn't find the extracted release directory -- nothing installed." >&2
    exit 1
fi

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
