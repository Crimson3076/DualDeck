#!/usr/bin/env bash
# Persistent, cross-invocation build cache for scripts/emudeck-replace-in-place.sh
# -- keyed on (pinned upstream commit, patch file content), NOT on
# whatever stock version EmuDeck's own updater happens to have grabbed.
# DualDeck always builds against its own pinned commit
# (scripts/lib/pinned_commits.sh) regardless of which AppImage EmuDeck
# last downloaded, so a DualDeck-built binary stays valid across EmuDeck's
# own updates as long as neither the pinned commit nor the patch file
# changed -- meaning a drift-detected re-patch
# (scripts/emudeck-check-drift.sh --fix, which just re-invokes
# emudeck-replace-in-place.sh) after EmuDeck silently replaced an
# AppImage with a newer stock build usually needs no recompilation at
# all, just re-installing the same cached DualDeck binary.
#
# Distinct from build_azahar()/build_cemu()'s own existing cache-hit
# check (scripts/lib/build_emulator.sh) -- that one skips re-cloning
# within a *single* run's own scratch work_dir (deleted on exit by
# emudeck-replace-in-place.sh's own EXIT trap); this one persists
# *across* separate invocations of this tool, in $HOME/.cache, and is
# what actually makes a later "EmuDeck updated, re-patch" cheap.
#
# Deliberately does not cache the packaged AppImage itself, only the raw
# compiled emulator binary build_melonds()/build_azahar()/build_cemu()
# produce -- packaging (AppRun generation, bundling dualdeck-host-service)
# is cheap and always re-run fresh, so a change to that logic alone
# (independent of the emulator patch/commit) is never masked by a stale
# cache hit here.

cache_root() {
    echo "${HOME}/.cache/dualdeck/emudeck-builds"
}

# build_fingerprint <patch_file> <commit>
#
# sha256(patch file content):commit -- the two inputs that fully
# determine build_melonds()/build_azahar()/build_cemu()'s output (see
# those functions in build_emulator.sh: clone at <commit>, apply
# <patch_file>, build). Changes whenever either one changes.
build_fingerprint() {
    local patch_file="$1" commit="$2"
    local patch_sha256
    patch_sha256="$(sha256sum "${patch_file}" | cut -d' ' -f1)"
    echo "${patch_sha256}:${commit}"
}

# try_cached_build <out_var> <emulator_name> <patch_file> <commit>
#
# If a cached build matching the current fingerprint exists, copies it
# to a fresh temp path and writes that path into <out_var> (nameref,
# same convention as build_melonds()/build_azahar()/build_cemu() in
# build_emulator.sh), returning 0. Prints nothing and returns 1 on a
# cache miss -- caller must build normally in that case.
try_cached_build() {
    local -n __try_cached_build_out="$1"
    local emulator_name="$2" patch_file="$3" commit="$4"

    local cache_dir
    cache_dir="$(cache_root)/${emulator_name}"
    local fingerprint_file="${cache_dir}/fingerprint.txt"
    local cached_bin="${cache_dir}/bin"

    [[ -f "${fingerprint_file}" && -f "${cached_bin}" ]] || return 1

    local current_fp cached_fp
    current_fp="$(build_fingerprint "${patch_file}" "${commit}")"
    cached_fp="$(cat "${fingerprint_file}")"
    [[ "${current_fp}" == "${cached_fp}" ]] || return 1

    echo "${emulator_name}: reusing cached DualDeck build (unchanged since last build -- no recompile needed)" >&2
    # Copied (not referenced in place) so the caller can freely move/
    # rename it (e.g. into a per-run staging directory matching the
    # exec name AppRun expects) without touching the persistent cache
    # entry itself.
    local out_copy
    out_copy="$(mktemp -u)"
    cp "${cached_bin}" "${out_copy}"
    __try_cached_build_out="${out_copy}"
    return 0
}

# save_build_cache <emulator_name> <patch_file> <commit> <bin_path>
#
# Persists a freshly-built binary as this emulator's new cache entry, so
# the next invocation (e.g. after EmuDeck's own updater causes drift) can
# skip rebuilding via try_cached_build() above. Overwrites any previous
# entry for this emulator unconditionally -- there is only ever one
# cached build per emulator (the most recent), not a history.
save_build_cache() {
    local emulator_name="$1" patch_file="$2" commit="$3" bin_path="$4"

    local cache_dir
    cache_dir="$(cache_root)/${emulator_name}"
    mkdir -p "${cache_dir}"
    cp "${bin_path}" "${cache_dir}/bin"
    build_fingerprint "${patch_file}" "${commit}" > "${cache_dir}/fingerprint.txt"
}
