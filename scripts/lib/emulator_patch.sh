#!/usr/bin/env bash
# Applies a DualDeck emulator integration (host/<emulator>-patches/) to an
# upstream source tree, meant to be `source`d by scripts/lib/
# build_emulator.sh and scripts/patch-existing-emulator.sh.
#
# An integration is two parts:
#   - 0001-remote-server-integration.patch: the emulator-specific changes
#     (the adapter, the bridge, hooks into upstream files).
#   - shared-files.txt: the shared DualDeck sources the integration also
#     compiles (adapter-sdk/, protocol/, and for melonDS's in-process
#     server part of host/remote-server/), copied from this repository.
#
# The patches used to embed their own vendored copy of those shared
# sources, one per emulator, which drifted out of sync with the live
# ones more than once (see docs/history.md's 2026-08-01 "frozen
# protocol copy" entry). Copying them from the live tree at apply time
# keeps exactly one copy of each.

# _dualdeck_shared_files <patch_dir>
#
# Prints shared-files.txt's "<repo path> <emulator path>" pairs, one per
# line, without comments or blank lines.
_dualdeck_shared_files() {
    local manifest="$1/shared-files.txt"
    if [[ ! -f "${manifest}" ]]; then
        echo "error: ${manifest} not found" >&2
        return 1
    fi
    grep -v -E '^[[:space:]]*(#|$)' "${manifest}"
}

# install_dualdeck_shared_files <patch_dir> <src_dir> <repo_root>
#
# Copies every file shared-files.txt lists from <repo_root> into
# <src_dir>, overwriting any older copy already there.
install_dualdeck_shared_files() {
    local patch_dir="$1" src_dir="$2" repo_root="$3"
    local entries from to
    entries="$(_dualdeck_shared_files "${patch_dir}")" || return 1
    while read -r from to; do
        mkdir -p "$(dirname "${src_dir}/${to}")"
        cp "${repo_root}/${from}" "${src_dir}/${to}"
    done <<< "${entries}"
}

# apply_dualdeck_patch <patch_dir> <src_dir> <repo_root>
#
# `git apply`s the integration patch to <src_dir>, then installs the
# shared files. Fails (without copying anything) if the patch doesn't
# apply.
apply_dualdeck_patch() {
    local patch_dir="$1" src_dir="$2" repo_root="$3"
    git -C "${src_dir}" apply "${patch_dir}/0001-remote-server-integration.patch" || return 1
    install_dualdeck_shared_files "${patch_dir}" "${src_dir}" "${repo_root}"
}

# dualdeck_patch_is_applied <patch_dir> <src_dir> <repo_root>
#
# Succeeds only if <src_dir> has the CURRENT integration applied: the
# patch reverses cleanly AND every shared file matches the live copy
# byte for byte. Used by the cache-hit checks, so a change to a shared
# source invalidates a cached build exactly like a change to the patch.
dualdeck_patch_is_applied() {
    local patch_dir="$1" src_dir="$2" repo_root="$3"
    local entries from to
    git -C "${src_dir}" apply --reverse --check \
        "${patch_dir}/0001-remote-server-integration.patch" 2>/dev/null || return 1
    entries="$(_dualdeck_shared_files "${patch_dir}")" || return 1
    while read -r from to; do
        cmp -s "${repo_root}/${from}" "${src_dir}/${to}" || return 1
    done <<< "${entries}"
}
