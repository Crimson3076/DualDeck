# shellcheck shell=bash
# Shared by the host and client install/update scripts in
# packaging/*/internal/ and their two menus. build-release.sh bundles a
# copy into both host/internal/ and client/internal/, the same way it
# bundles dualdeck_branch.sh, so each side stays self-contained.
#
# Callers set `error_log` before calling dualdeck_trap_errors.
# shellcheck disable=SC2154  # error_log is set by the caller

# dualdeck_trap_errors <dialog title> <what failed>
#
# Logs any failing command to ${error_log} and, when kdialog exists
# (SteamOS Desktop Mode and Bazzite are KDE), shows it in an error popup,
# so a double-clicked script with no terminal doesn't just close
# silently (GitHub issue #11). An empty title logs without a popup.
dualdeck_trap_errors() {
    _dualdeck_error_title="$1"
    _dualdeck_error_what="$2"
    _dualdeck_error_script="$(basename "${BASH_SOURCE[1]}")"
    trap 'dualdeck_report_error "$?" "${LINENO}" "${BASH_COMMAND}"' ERR
}

dualdeck_report_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") ${_dualdeck_error_script} line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
    if [[ -n "${_dualdeck_error_title}" ]] && command -v kdialog >/dev/null 2>&1; then
        kdialog --title "${_dualdeck_error_title}" --error "${_dualdeck_error_what}: ${failing_cmd}
(exit code ${exit_code})

Details logged to:
${error_log}" 2>/dev/null || true
    fi
}

# dualdeck_resolve_selected_branch
#
# Resolves the branch picked in Advanced -> Installation branch (needs
# dualdeck_branch.sh sourced and DUALDECK_BRANCH_CONFIG_DIR set) to the
# release built from its current tip. Sets `branch`, `resolved_sha` and
# `resolved_tag` for the caller, or prints why not and exits 1.
# shellcheck disable=SC2034  # branch/resolved_* are read by the caller
dualdeck_resolve_selected_branch() {
    branch="$(dualdeck_branch_get_selected)"
    if [[ -z "${branch}" ]]; then
        echo "No branch selected -- open Advanced -> Installation branch and pick one first." >&2
        exit 1
    fi

    local resolved resolve_rc=0
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
}

# dualdeck_fetch_release <download base URL> <work dir>
#
# Downloads the release archive from <download base URL> into <work dir>,
# checks it against the release's SHA256SUMS, and extracts it. Sets
# `extracted_dir` to the extracted release folder, or prints why not and
# exits 1. Nothing outside <work dir> is touched.
dualdeck_fetch_release() {
    local download_base="$1" work_dir="$2"
    local archive_name="melonds-remote-linux-x86_64.tar.gz"

    if ! command -v sha256sum >/dev/null 2>&1; then
        echo "error: sha256sum is required to verify the download -- install coreutils and try again." >&2
        exit 1
    fi
    curl --proto =https -fsSL --max-time 180 -o "${work_dir}/${archive_name}" "${download_base}/${archive_name}"
    curl --proto =https -fsSL --max-time 30 -o "${work_dir}/SHA256SUMS" "${download_base}/SHA256SUMS"
    echo "Verifying download integrity..."
    if ! (cd "${work_dir}" && sha256sum -c --ignore-missing SHA256SUMS) >/dev/null 2>&1; then
        echo "error: checksum verification failed -- refusing to install an unverified download. Nothing changed." >&2
        exit 1
    fi

    echo "Extracting..."
    tar xzf "${work_dir}/${archive_name}" -C "${work_dir}"
    extracted_dir=""
    local candidate
    for candidate in "${work_dir}"/melonds-remote-*; do
        [[ -d "${candidate}" ]] && extracted_dir="${candidate}" && break
    done
    if [[ -z "${extracted_dir}" ]]; then
        echo "error: couldn't find the extracted release directory -- nothing installed." >&2
        exit 1
    fi
}
