#!/usr/bin/env bash
# Shared "Installation branch" discovery/caching/resolution logic for
# both DualDeck Host and DualDeck Client (Advanced -> Installation
# branch). Sourced, not executed -- the caller must already have
# `set -euo pipefail` (every function here is written to be safe under
# that: no bare command whose non-zero exit would trigger errexit
# outside of an explicit `if`/`||` guard) and must set
# DUALDECK_BRANCH_CONFIG_DIR before sourcing this file, e.g.
# "${HOME}/.config/dualdeck" for the host, "${HOME}/.config/dualdeck-client"
# for the client.
#
# This is deliberately the *one* implementation both sides use, rather
# than two hand-written copies that could drift (real requirement: "do
# not add separate host and client selectors"). Host and client are
# always separate machines, so there is no single file the two can
# literally share -- what "shared" means here is that the exact same
# branch name, resolved independently by each machine against the same
# GitHub repository, is guaranteed to resolve to the same published
# release/commit (branches move rarely enough in practice for this to
# hold; each side also displays its own resolved commit so a mismatch is
# visible rather than silent).
#
# No jq dependency: like check-for-updates.sh, JSON responses are
# scraped with grep/sed rather than adding a new required package. This
# is fragile in the general case, but relies on the same stable GitHub
# API field ordering check-for-updates.sh already depends on for
# "tag_name", so it's an established, accepted tradeoff in this
# codebase, not a new one.

DUALDECK_BRANCH_REPO="${DUALDECK_BRANCH_REPO:-Crimson3076/DualDeck}"
DUALDECK_BRANCH_API="${DUALDECK_BRANCH_API:-https://api.github.com}"
DUALDECK_BRANCH_CACHE_TTL_SECS="${DUALDECK_BRANCH_CACHE_TTL_SECS:-3600}"

_dualdeck_branch_config_dir() {
    if [[ -z "${DUALDECK_BRANCH_CONFIG_DIR:-}" ]]; then
        echo "dualdeck_branch: DUALDECK_BRANCH_CONFIG_DIR is not set" >&2
        return 1
    fi
    mkdir -p "${DUALDECK_BRANCH_CONFIG_DIR}"
    echo "${DUALDECK_BRANCH_CONFIG_DIR}"
}

# Rejects anything that is not a plain, safe git branch name. Branch
# names are untrusted input by the time they reach this code: they come
# either from GitHub's own API response (anyone with push access to any
# fork could name a branch almost anything) or from a user picking one
# out of that list -- and this project uses names straight in shell
# command lines (curl URLs) and filesystem paths, so both need
# validating before use, not just at the point they were typed.
# Deliberately conservative (alphanumeric plus . _ / -, must start/end
# alphanumeric, no "..", no doubled slash, capped length) rather than
# trying to enumerate every git-illegal ref pattern.
dualdeck_branch_validate() {
    local name="${1:-}"
    [[ -n "${name}" ]] || return 1
    [[ "${#name}" -le 200 ]] || return 1
    if [[ "${#name}" -eq 1 ]]; then
        [[ "${name}" =~ ^[A-Za-z0-9]$ ]] || return 1
        return 0
    fi
    [[ "${name}" =~ ^[A-Za-z0-9][A-Za-z0-9._/-]*[A-Za-z0-9]$ ]] || return 1
    [[ "${name}" != *..* ]] || return 1
    [[ "${name}" != *//* ]] || return 1
    [[ "${name}" != *.lock ]] || return 1
    return 0
}

# GET a GitHub API path, writing the raw response body to stdout.
# Return codes: 0 ok (HTTP 200), 1 offline/timeout/unexpected-status/
# empty response, 2 rate-limited (403 with X-RateLimit-Remaining: 0),
# 3 not found (404, e.g. a deleted branch or bad repo path).
_dualdeck_branch_api_get() {
    local path="$1" headers body status rc
    headers="$(mktemp)"
    body="$(mktemp)"
    status=""
    status="$(curl -sS --max-time 8 \
        -H "Accept: application/vnd.github+json" \
        -D "${headers}" -o "${body}" -w '%{http_code}' \
        "${DUALDECK_BRANCH_API}${path}" 2>/dev/null)" || status=""
    rc=1
    if [[ -z "${status}" ]]; then
        rc=1
    elif [[ "${status}" == "404" ]]; then
        rc=3
    elif [[ "${status}" == "403" ]] && grep -qi '^x-ratelimit-remaining: *0' "${headers}"; then
        rc=2
    elif [[ "${status}" == "200" ]]; then
        rc=0
    else
        rc=1
    fi
    if [[ "${rc}" -eq 0 ]]; then
        cat "${body}"
    fi
    rm -f "${headers}" "${body}"
    return "${rc}"
}

# Extracts every value of a top-level-ish `"field": "value"` JSON pair
# from stdin, one per line, in document order.
_dualdeck_branch_extract_field() {
    local field="$1"
    grep -o "\"${field}\" *: *\"[^\"]*\"" | sed -E "s/.*\"${field}\" *: *\"([^\"]*)\"/\\1/"
}

# Paginates GET /repos/{repo}/branches (100 per page) into
# ${config_dir}/branch-cache.list, one branch name per line, and records
# the fetch time. Returns the same codes as _dualdeck_branch_api_get; on
# any non-zero return, an existing cache file is left completely
# untouched (dualdeck_branch_list below falls back to it, marked stale,
# rather than losing the last known branch list to a transient failure).
dualdeck_branch_refresh() {
    local dir tmp page body page_names page_count rc
    dir="$(_dualdeck_branch_config_dir)" || return 1
    tmp="$(mktemp)"
    page=1
    while :; do
        rc=0
        body="$(_dualdeck_branch_api_get "/repos/${DUALDECK_BRANCH_REPO}/branches?per_page=100&page=${page}")" || rc=$?
        if [[ "${rc}" -ne 0 ]]; then
            rm -f "${tmp}"
            return "${rc}"
        fi
        page_names="$(printf '%s' "${body}" | _dualdeck_branch_extract_field "name")"
        if [[ -z "${page_names}" ]]; then
            break
        fi
        printf '%s\n' "${page_names}" >> "${tmp}"
        page_count="$(printf '%s\n' "${page_names}" | grep -c .)"
        if [[ "${page_count}" -lt 100 ]]; then
            break
        fi
        page=$((page + 1))
        if [[ "${page}" -gt 50 ]]; then
            break
        fi
    done
    if [[ ! -s "${tmp}" ]]; then
        rm -f "${tmp}"
        return 1
    fi
    sort -u -o "${tmp}" "${tmp}"
    mv "${tmp}" "${dir}/branch-cache.list"
    date -u +%s > "${dir}/branch-cache.fetched-at"
    return 0
}

# Prints cached branch names (one per line). Refreshes first if there is
# no cache yet or it's older than DUALDECK_BRANCH_CACHE_TTL_SECS. A
# refresh failure with an existing (merely stale) cache still succeeds,
# printing the stale list -- callers should pair this with
# dualdeck_branch_cache_is_stale to label it. Only returns non-zero when
# there is truly nothing to show.
dualdeck_branch_list() {
    local dir names_file fetched_at now rc
    dir="$(_dualdeck_branch_config_dir)" || return 1
    names_file="${dir}/branch-cache.list"
    if [[ -f "${names_file}" ]]; then
        fetched_at="$(cat "${dir}/branch-cache.fetched-at" 2>/dev/null || echo 0)"
        now="$(date -u +%s)"
        if [[ "$((now - fetched_at))" -lt "${DUALDECK_BRANCH_CACHE_TTL_SECS}" ]]; then
            cat "${names_file}"
            return 0
        fi
    fi
    rc=0
    dualdeck_branch_refresh || rc=$?
    if [[ "${rc}" -eq 0 ]]; then
        cat "${names_file}"
        return 0
    fi
    if [[ -f "${names_file}" ]]; then
        cat "${names_file}"
        return 0
    fi
    return "${rc}"
}

dualdeck_branch_cache_is_stale() {
    local dir fetched_at now
    dir="$(_dualdeck_branch_config_dir)" || return 0
    if [[ ! -f "${dir}/branch-cache.fetched-at" ]]; then
        return 0
    fi
    fetched_at="$(cat "${dir}/branch-cache.fetched-at" 2>/dev/null || echo 0)"
    now="$(date -u +%s)"
    if [[ "$((now - fetched_at))" -ge "${DUALDECK_BRANCH_CACHE_TTL_SECS}" ]]; then
        return 0
    fi
    return 1
}

# Resolves a branch name to the newest published GitHub Release built
# exactly at that branch's current tip commit. On success prints
# "<sha><TAB><tag>" and returns 0.
#
# Return codes: 1 offline/unexpected error, 2 rate-limited, 3 branch not
# found (deleted, renamed, or never existed), 4 branch exists but no
# release has ever been published from its exact current tip (nothing
# installable yet -- true for most feature branches until someone runs
# the Release workflow against them), 5 invalid branch name.
#
# NEVER substitutes another branch (e.g. main) on any failure -- every
# non-zero return means "install nothing, tell the user exactly why."
dualdeck_branch_resolve() {
    local name="${1:-}" body sha page rc found tags_file targets_file page_count
    dualdeck_branch_validate "${name}" || return 5

    rc=0
    body="$(_dualdeck_branch_api_get "/repos/${DUALDECK_BRANCH_REPO}/branches/${name}")" || rc=$?
    if [[ "${rc}" -ne 0 ]]; then
        return "${rc}"
    fi
    sha="$(printf '%s' "${body}" | _dualdeck_branch_extract_field "sha" | head -1)"
    if [[ -z "${sha}" ]]; then
        return 1
    fi

    found=""
    page=1
    while :; do
        rc=0
        body="$(_dualdeck_branch_api_get "/repos/${DUALDECK_BRANCH_REPO}/releases?per_page=100&page=${page}")" || rc=$?
        if [[ "${rc}" -ne 0 ]]; then
            return "${rc}"
        fi
        if [[ -z "${body}" || "${body}" == "[]" ]]; then
            break
        fi

        tags_file="$(mktemp)"
        targets_file="$(mktemp)"
        printf '%s' "${body}" | _dualdeck_branch_extract_field "tag_name" > "${tags_file}"
        printf '%s' "${body}" | _dualdeck_branch_extract_field "target_commitish" > "${targets_file}"
        page_count="$(wc -l < "${tags_file}" | tr -d ' ')"
        if [[ "${page_count}" -eq 0 ]]; then
            rm -f "${tags_file}" "${targets_file}"
            break
        fi

        found="$(paste "${tags_file}" "${targets_file}" | awk -F'\t' -v s="${sha}" '$2==s {print $1; exit}')"
        rm -f "${tags_file}" "${targets_file}"
        if [[ -n "${found}" ]]; then
            break
        fi
        if [[ "${page_count}" -lt 100 ]]; then
            break
        fi
        page=$((page + 1))
        if [[ "${page}" -gt 20 ]]; then
            break
        fi
    done

    if [[ -z "${found}" ]]; then
        return 4
    fi
    printf '%s\t%s\n' "${sha}" "${found}"
    return 0
}

# Persists only the *selected* branch -- must never trigger an install by
# itself (real requirement: changing the dropdown must not immediately
# reinstall). Kept in a separate file from branch.conf (the actually-
# installed record) so the two can legitimately disagree until the user
# runs "Install selected branch".
dualdeck_branch_set_selected() {
    local dir name="${1:-}"
    dualdeck_branch_validate "${name}" || return 1
    dir="$(_dualdeck_branch_config_dir)" || return 1
    printf 'branch=%s\n' "${name}" > "${dir}/branch.conf.selected"
    return 0
}

# Prints the selected branch: the explicit selection if one was ever
# made, else whatever is currently recorded as installed, else nothing
# (caller falls back to dualdeck_branch_default_branch for display).
dualdeck_branch_get_selected() {
    local dir
    dir="$(_dualdeck_branch_config_dir)" || return 1
    if [[ -f "${dir}/branch.conf.selected" ]]; then
        sed -n 's/^branch=//p' "${dir}/branch.conf.selected" | head -1
        return 0
    fi
    dualdeck_branch_get_installed_name
}

# Records what was *actually installed* -- called only after a verified,
# fully-completed install (see internal/install-branch.sh on each side).
# Never called on a partial/failed install.
dualdeck_branch_record_installed() {
    local dir name="${1:-}" sha="${2:-}" tag="${3:-}"
    dir="$(_dualdeck_branch_config_dir)" || return 1
    {
        printf 'branch=%s\n' "${name}"
        printf 'resolved_commit=%s\n' "${sha}"
        printf 'resolved_release_tag=%s\n' "${tag}"
        printf 'resolved_at=%s\n' "$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
    } > "${dir}/branch.conf"
    # Keeps "selected" from disagreeing with what was just installed.
    printf 'branch=%s\n' "${name}" > "${dir}/branch.conf.selected"
    return 0
}

dualdeck_branch_get_installed_name() {
    local dir
    dir="$(_dualdeck_branch_config_dir)" || return 1
    if [[ -f "${dir}/branch.conf" ]]; then
        sed -n 's/^branch=//p' "${dir}/branch.conf" | head -1
        return 0
    fi
    return 1
}

dualdeck_branch_get_installed_commit() {
    local dir
    dir="$(_dualdeck_branch_config_dir)" || return 1
    [[ -f "${dir}/branch.conf" ]] && sed -n 's/^resolved_commit=//p' "${dir}/branch.conf" | head -1
    return 0
}

dualdeck_branch_get_installed_tag() {
    local dir
    dir="$(_dualdeck_branch_config_dir)" || return 1
    [[ -f "${dir}/branch.conf" ]] && sed -n 's/^resolved_release_tag=//p' "${dir}/branch.conf" | head -1
    return 0
}

# The repository's default branch -- used only to LABEL an existing
# install that predates this feature (no branch.conf yet). Never written
# to disk by this function, so merely opening the Advanced menu on an
# existing install never creates state or implies a reinstall (real
# requirement: "existing users should default to the repository's
# default branch without triggering a reinstall").
dualdeck_branch_default_branch() {
    local body rc
    rc=0
    body="$(_dualdeck_branch_api_get "/repos/${DUALDECK_BRANCH_REPO}")" || rc=$?
    if [[ "${rc}" -ne 0 ]]; then
        return "${rc}"
    fi
    printf '%s' "${body}" | _dualdeck_branch_extract_field "default_branch" | head -1
    return 0
}

# One-line human status for the Advanced submenu and the "Check for
# updates" report.
dualdeck_branch_status_line() {
    local installed_name installed_sha installed_tag selected default_branch
    installed_name="$(dualdeck_branch_get_installed_name 2>/dev/null || true)"
    selected="$(dualdeck_branch_get_selected 2>/dev/null || true)"
    if [[ -z "${installed_name}" ]]; then
        default_branch="$(dualdeck_branch_default_branch 2>/dev/null || true)"
        echo "Installation branch: ${selected:-${default_branch:-main}} (installed: pre-branch-selector build, exact commit unknown)"
        return 0
    fi
    installed_sha="$(dualdeck_branch_get_installed_commit)"
    installed_tag="$(dualdeck_branch_get_installed_tag)"
    if [[ -n "${selected}" && "${selected}" != "${installed_name}" ]]; then
        echo "Installation branch: ${selected} selected, not yet installed (currently installed: ${installed_name} @ ${installed_sha:0:7}, ${installed_tag})"
    else
        echo "Installation branch: ${installed_name} (installed: ${installed_sha:0:7}, ${installed_tag})"
    fi
    return 0
}
