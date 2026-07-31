#!/usr/bin/env bash
# Shared "clone + pin + patch + build" logic for melonDS/Azahar/Cemu, meant
# to be `source`d (not executed directly) by scripts/build-release.sh and by
# scripts/emudeck-replace-in-place.sh -- factored out so the EmuDeck
# replace-in-place installer (which only ever needs to build ONE emulator at
# a time, against a user's already-detected EmuDeck install) can reuse the
# exact same cache-hit/patch-correctness logic build-release.sh's release
# pipeline depends on, instead of a second, silently-drifting copy of it.
#
# Contract: each build_<emulator>() function takes (out_var_name, work_dir,
# repo_root, commit) as positional args and expects a `cmake_launcher_args`
# array variable already declared in the CALLER's scope (bash's dynamic
# scoping means these functions see it without it being passed explicitly --
# both build-release.sh and emudeck-replace-in-place.sh declare it once, the
# same way ensure-packages.sh's callers declare `sudo_cmd`/`pkgs` for it).
#
# The built binary's path is written into the CALLER's out_var_name (via a
# `local -n` nameref), not printed on stdout for a `$(...)` caller to
# capture -- capturing the whole function via command substitution would
# swallow every line of git clone/cmake/ninja progress output into that
# variable instead of streaming it to the terminal, which matters a lot for
# builds that can run 20+ minutes (Cemu especially). Callers do:
#   build_melonds melonds_bin "${work_dir}" "${repo_root}" "${MELONDS_COMMIT}"
#   echo "built: ${melonds_bin}"

# build_melonds <out_var> <work_dir> <repo_root> <commit>
#
# No cache-hit check (unlike Azahar/Cemu below) -- melonDS is still a cheap
# enough build not to bother, matching build-release.sh's original comment.
build_melonds() {
    local -n __build_melonds_out="$1"
    local work_dir="$2" repo_root="$3" commit="$4"
    local melonds_src="${work_dir}/melonds-src"

    rm -rf "${melonds_src}"
    git clone https://github.com/melonDS-emu/melonDS.git "${melonds_src}"
    (cd "${melonds_src}" && git checkout "${commit}")
    (cd "${melonds_src}" && git apply "${repo_root}/host/melonds-patches/0001-remote-server-integration.patch")
    cmake -S "${melonds_src}" -B "${melonds_src}/build" -DCMAKE_BUILD_TYPE=Release \
        "${cmake_launcher_args[@]}"
    cmake --build "${melonds_src}/build" -j"$(nproc)"

    __build_melonds_out="${melonds_src}/build/melonDS"
}

# build_azahar <out_var> <work_dir> <repo_root> <commit>
#
# Cached across runs by commit + patch hash (a much heavier build than
# melonDS's -- 36 git submodules for real 3D emulation). The cache-hit check
# verifies the CURRENT patch file is actually what's applied in the cached
# tree (via `git apply --reverse --check`, the same idempotency technique
# patch-existing-emulator.sh uses), not just that a binary happens to exist
# at this path -- a real bug shipped two releases (v0.1.39, v0.1.40) with
# byte-for-byte identical azahar binaries despite the patch changing in
# between, because the old check only tested file existence.
build_azahar() {
    local -n __build_azahar_out="$1"
    local work_dir="$2" repo_root="$3" commit="$4"
    local azahar_src="${work_dir}/azahar-src"
    local azahar_patch_file="${repo_root}/host/azahar-patches/0001-remote-server-integration.patch"
    local azahar_cache_hit=0

    if [[ -f "${azahar_src}/build/bin/Release/azahar" ]] && \
       (cd "${azahar_src}" && git apply --reverse --check "${azahar_patch_file}" 2>/dev/null); then
        echo "already built at ${azahar_src} with the current patch applied, skipping (cache hit)"
        azahar_cache_hit=1
    fi
    if [[ "${azahar_cache_hit}" -eq 0 ]]; then
        rm -rf "${azahar_src}"
        git clone https://github.com/azahar-emu/azahar.git "${azahar_src}"
        (cd "${azahar_src}" && git checkout "${commit}")
        (cd "${azahar_src}" && git submodule update --init --recursive --depth 1)
        (cd "${azahar_src}" && git apply "${azahar_patch_file}")
        cmake -S "${azahar_src}" -B "${azahar_src}/build" -DCMAKE_BUILD_TYPE=Release \
            -DENABLE_QT_TRANSLATION=OFF \
            "${cmake_launcher_args[@]}"
        cmake --build "${azahar_src}/build" -j"$(nproc)"
    fi

    __build_azahar_out="${azahar_src}/build/bin/Release/azahar"
}

# build_cemu <out_var> <work_dir> <repo_root> <commit>
#
# Same cache-across-runs technique and rationale as build_azahar() above
# (an even heavier build: Cemu's own vcpkg submodule resolves to roughly
# 108 packages). vcpkg's own binary archive cache at
# $HOME/.cache/vcpkg/archives lives outside cemu_src, so it survives the
# `rm -rf cemu_src` below on a patch-hash cache miss.
build_cemu() {
    local -n __build_cemu_out="$1"
    local work_dir="$2" repo_root="$3" commit="$4"
    local cemu_src="${work_dir}/cemu-src"
    local cemu_patch_file="${repo_root}/host/cemu-patches/0001-remote-server-integration.patch"
    local cemu_cache_hit=0

    if [[ -f "${cemu_src}/bin/Cemu_release" ]] && \
       (cd "${cemu_src}" && git apply --reverse --check "${cemu_patch_file}" 2>/dev/null); then
        echo "already built at ${cemu_src} with the current patch applied, skipping (cache hit)"
        cemu_cache_hit=1
    fi
    if [[ "${cemu_cache_hit}" -eq 0 ]]; then
        rm -rf "${cemu_src}"
        git clone --recurse-submodules https://github.com/cemu-project/Cemu.git "${cemu_src}"
        (cd "${cemu_src}" && git checkout "${commit}" && git submodule update --init --recursive)
        (cd "${cemu_src}" && git apply "${cemu_patch_file}")
        cmake -S "${cemu_src}" -B "${cemu_src}/build" -DCMAKE_BUILD_TYPE=release -G Ninja \
            "${cmake_launcher_args[@]}"
        cmake --build "${cemu_src}/build" -j"$(nproc)"
    fi

    __build_cemu_out="${cemu_src}/bin/Cemu_release"
}
