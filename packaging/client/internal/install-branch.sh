#!/usr/bin/env bash
# Advanced -> Installation branch -> "Install selected branch". Resolves
# the currently-selected branch (dualdeck_branch_get_selected) to the
# newest published release built exactly at that branch's current tip
# commit, downloads and checksum-verifies *that specific release* (not
# "latest" -- see download_base below), and only then hands off to the
# same already-atomic client/internal/install-steam-shortcut.sh --force
# every other install/update path uses (stage in a .new sibling, swap
# into place only once staging succeeds, one generation kept as
# .previous). Nothing is recorded as installed (dualdeck_branch_
# record_installed) unless that whole sequence actually completes --
# a failure at any earlier step leaves the previous install running
# untouched and reports exactly what went wrong, never a fabricated
# fallback to another branch.
#
# Host and client are separate machines: each side runs this
# independently against its own selected branch. What guarantees "same
# branch, same resolved commit" for both is that they each resolve the
# identical branch name against the same GitHub repository and download
# from the identical release archive that name resolves to -- not a
# live cross-machine transaction (see dualdeck_branch.sh's own header
# comment).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

error_log="${HOME}/.config/dualdeck-client/install.log"
# shellcheck source=scripts/lib/release_install.sh
source ./release_install.sh
dualdeck_trap_errors "DualDeck" "Installing the selected branch failed"

if ! command -v curl >/dev/null 2>&1; then
    echo "error: curl is required -- install it and try again." >&2
    exit 1
fi

export DUALDECK_BRANCH_CONFIG_DIR="${HOME}/.config/dualdeck-client"
# shellcheck source=scripts/lib/dualdeck_branch.sh
source ./dualdeck_branch.sh

dualdeck_resolve_selected_branch

repo="Crimson3076/DualDeck"
download_base="https://github.com/${repo}/releases/download/${resolved_tag}"

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

echo "Downloading ${resolved_tag} (branch ${branch}, commit ${resolved_sha:0:7})..."
dualdeck_fetch_release "${download_base}" "${work_dir}"

echo "Installing..."
"${extracted_dir}/client/internal/install-steam-shortcut.sh" --force

# Only recorded once the staged swap above has actually completed.
dualdeck_branch_record_installed "${branch}" "${resolved_sha}" "${resolved_tag}"
echo "Installed ${resolved_tag} (branch ${branch}, commit ${resolved_sha:0:7})."
