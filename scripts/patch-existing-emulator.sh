#!/usr/bin/env bash
# Applies the DualDeck remote-server integration patch to an existing
# melonDS, Azahar, or Cemu source checkout you already have, instead of
# downloading/building a separate DualDeck-managed copy via
# scripts/build-release.sh -- for anyone who already has one of these
# emulators set up the way they like it and doesn't want a duplicate
# install alongside it.
#
# Usage:
#   scripts/patch-existing-emulator.sh --system ds   --source /path/to/your/melonDS/checkout [--build]
#   scripts/patch-existing-emulator.sh --system 3ds  --source /path/to/your/azahar/checkout  [--build]
#   scripts/patch-existing-emulator.sh --system wiiu --source /path/to/your/Cemu/checkout    [--build]
#
# Without --build, only applies the patch (git apply) and prints the
# commands to build it yourself. With --build, also configures and
# builds it -- not a full release package, and, unlike
# scripts/build-release.sh's official release build, tuned for fast
# local iteration: it auto-detects and uses ccache/Ninja/mold-or-lld
# when installed, and (Azahar only) turns off LTO and a few unrelated
# features, since none of that matters while testing a patch. See the
# "Speed-up flags" comment further down for the full rationale. Install
# ccache/ninja-build/mold beforehand (e.g. `sudo dnf install ccache
# ninja-build mold` on Fedora) for the biggest wins.
#
# Once built, point host/dualdeck-host.sh's "Launch..." -> "Custom"
# menu choice at the resulting binary -- see
# host/internal/launch-custom-emulator.sh in a packaged release, or
# just launch it directly with the right environment variables (see
# that script, or run-host.sh/run-host-azahar.sh, for exactly which
# ones each system needs).
#
# The patch is generated against a specific pinned upstream commit (see
# MELONDS_COMMIT/AZAHAR_COMMIT below, kept in sync with
# scripts/build-release.sh's own pinned commits) -- applying it to a
# checkout at a different commit may fail outright (git apply refuses a
# hunk that doesn't match cleanly) or, worse, apply with unintended
# differences if the surrounding code happens to have drifted just
# enough to still match. This script warns (does not block) on a commit
# mismatch; git apply's own hunk-matching is the real safety net either
# way.
set -euo pipefail

MELONDS_COMMIT="10a173b5536fc75cd93f8a3868349dad963542ef"
AZAHAR_COMMIT="75134fca82eab4e1a86dca0aaa4a188cefff5469"
CEMU_COMMIT="50b9e4ba1d4d7cf9821a9cd416378bb94e1ba0ca"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

system=""
source_dir=""
do_build=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --system)
            system="$2"
            shift 2
            ;;
        --source)
            source_dir="$2"
            shift 2
            ;;
        --build)
            do_build=1
            shift
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

if [[ -z "${system}" || -z "${source_dir}" ]]; then
    echo "usage: $0 --system <ds|3ds|wiiu> --source /path/to/checkout [--build]" >&2
    exit 1
fi

case "${system}" in
    ds)
        patch_file="${repo_root}/host/melonds-patches/0001-remote-server-integration.patch"
        pinned_commit="${MELONDS_COMMIT}"
        emulator_name="melonDS"
        # This one is stable and load-bearing: host/melonds-patches/README.md
        # and scripts/build-release.sh both already document it as the
        # actual build output path.
        binary_hint="build/melonDS"
        ;;
    3ds)
        patch_file="${repo_root}/host/azahar-patches/0001-remote-server-integration.patch"
        pinned_commit="${AZAHAR_COMMIT}"
        emulator_name="Azahar"
        # CMAKE_RUNTIME_OUTPUT_DIRECTORY is ${PROJECT_BINARY_DIR}/bin/$<CONFIG>
        # (see Azahar's own top-level CMakeLists.txt) and the executable
        # itself is OUTPUT_NAME "azahar" (src/citra_meta/CMakeLists.txt) --
        # confirmed against a real build of the exact pinned commit above,
        # not guessed. $<CONFIG> resolves to the -DCMAKE_BUILD_TYPE value
        # for the single-config Makefiles/Ninja generators this script uses.
        binary_hint="build/bin/Release/azahar"
        ;;
    wiiu)
        patch_file="${repo_root}/host/cemu-patches/0001-remote-server-integration.patch"
        pinned_commit="${CEMU_COMMIT}"
        emulator_name="Cemu"
        # Cemu's own top-level CMakeLists.txt sets RUNTIME_OUTPUT_DIRECTORY
        # to "${CMAKE_CURRENT_SOURCE_DIR}/../bin" from within src/CMakeLists.txt
        # (i.e. <checkout>/bin, not <checkout>/build/bin) with OUTPUT_NAME
        # "Cemu_$<LOWER_CASE:$<CONFIG>>" -- confirmed by reading that
        # CMakeLists.txt directly, not guessed. $<CONFIG> resolves to the
        # -DCMAKE_BUILD_TYPE value below (lowercased), same as Azahar's hint.
        binary_hint="bin/Cemu_release"
        ;;
    *)
        echo "error: --system must be 'ds', '3ds', or 'wiiu', got '${system}'" >&2
        exit 1
        ;;
esac

if [[ ! -f "${patch_file}" ]]; then
    echo "error: ${patch_file} not found -- is this the full DualDeck repo checkout?" >&2
    exit 1
fi
if [[ ! -d "${source_dir}" ]]; then
    echo "error: ${source_dir} does not exist." >&2
    exit 1
fi
if [[ ! -e "${source_dir}/.git" ]]; then
    echo "error: ${source_dir} doesn't look like a git checkout (.git missing) --" >&2
    echo "the patch is generated as a git diff and needs a real git repo to apply" >&2
    echo "cleanly against." >&2
    exit 1
fi

current_commit="$(git -C "${source_dir}" rev-parse HEAD)"
if [[ "${current_commit}" != "${pinned_commit}" ]]; then
    echo "warning: ${source_dir} is at commit ${current_commit}, not the commit this" >&2
    echo "patch was generated against (${pinned_commit} for ${emulator_name})." >&2
    echo "git apply below may fail outright, or -- more subtly -- appear to succeed" >&2
    echo "while landing slightly wrong changes if the surrounding code happens to" >&2
    echo "still match at the exact lines the patch touches. Recommended:" >&2
    echo "  git -C \"${source_dir}\" checkout ${pinned_commit}" >&2
    echo "and re-run this script, unless you specifically know what you're doing" >&2
    echo "(e.g. you've already rebased this patch onto a newer commit yourself)." >&2
    echo >&2
fi

if git -C "${source_dir}" apply --check "${patch_file}" 2>/dev/null; then
    echo "Applying the DualDeck remote-server integration patch to ${source_dir} ..."
    git -C "${source_dir}" apply "${patch_file}"
elif git -C "${source_dir}" apply --reverse --check "${patch_file}" 2>/dev/null; then
    echo "Patch already applied to ${source_dir} -- nothing to do."
else
    echo "error: the patch does not apply cleanly to ${source_dir} (see git apply's" >&2
    echo "own error above, if any). This usually means the checkout is at a" >&2
    echo "different commit than the patch was generated against -- see the warning" >&2
    echo "above if one was printed." >&2
    exit 1
fi

# Speed-up flags for local dev iteration only (never used by
# scripts/build-release.sh's official release build, which keeps LTO on
# for the best user-facing runtime performance). All of these trade a
# bit of runtime performance or diagnostics you don't need while
# iterating on a patch for a lot of wall-clock build time -- Azahar in
# particular is a large Qt+Vulkan codebase where a from-scratch build
# can otherwise take the better part of an hour:
#   - ccache: caches object files by content hash, so rebuilding after
#     `git clean`/`rm -rf build` (which this repo's own scripts do on
#     every patch iteration) only pays full cost once; later rebuilds of
#     unchanged files are near-instant.
#   - Ninja: schedules the build graph more efficiently than the default
#     Makefiles generator, especially in combination with ccache.
#   - mold, falling back to lld: both link substantially faster than the
#     default bfd/gold linker, which matters a lot for a Qt-heavy target
#     like citra_qt.
#   - ENABLE_LTO=OFF (Azahar only -- melonDS's CMakeLists has no such
#     option): link-time optimization is the single biggest link-time
#     cost in a Release Azahar build and only pays off as a runtime perf
#     win, which you don't need while testing a patch.
#   - ENABLE_WEB_SERVICE/ENABLE_SCRIPTING/ENABLE_GDBSTUB=OFF (Azahar
#     only): telemetry, the scripting RPC server, and the GDB stub are
#     all irrelevant to testing the remote-server patch and reduce the
#     amount of code compiled and linked.
# All auto-detected -- nothing here is required; if none of ccache,
# Ninja, mold, or lld are installed, this just falls back to the plain
# command this script has always printed.
cmake_configure_args=(-DCMAKE_BUILD_TYPE=Release)
speedup_notes=()

if command -v ccache >/dev/null 2>&1; then
    cmake_configure_args+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
    speedup_notes+=("ccache (object cache across rebuilds)")
fi

generator_args=()
if command -v ninja >/dev/null 2>&1; then
    generator_args+=(-G Ninja)
    speedup_notes+=("Ninja generator")
fi

if command -v mold >/dev/null 2>&1; then
    cmake_configure_args+=(-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=mold)
    speedup_notes+=("mold linker")
elif command -v ld.lld >/dev/null 2>&1; then
    cmake_configure_args+=(-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld)
    speedup_notes+=("lld linker")
fi

if [[ "${system}" == "3ds" ]]; then
    cmake_configure_args+=(-DENABLE_LTO=OFF -DENABLE_WEB_SERVICE=OFF -DENABLE_SCRIPTING=OFF -DENABLE_GDBSTUB=OFF)
    speedup_notes+=("LTO/web-service/scripting/GDB-stub disabled")
fi

# Cap parallelism by available RAM as well as core count -- a Qt+Vulkan
# translation unit can use 2-3GB, and letting a job count sized only for
# core count run on a memory-constrained machine causes swapping, which
# is slower than just running fewer jobs at once.
job_count="$(nproc)"
if [[ -r /proc/meminfo ]]; then
    mem_kib="$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)"
    mem_capped_jobs=$(( mem_kib / 1024 / 1024 / 2 ))
    if [[ "${mem_capped_jobs}" -lt 1 ]]; then
        mem_capped_jobs=1
    fi
    if [[ "${mem_capped_jobs}" -lt "${job_count}" ]]; then
        job_count="${mem_capped_jobs}"
        speedup_notes+=("parallelism capped to ${job_count} jobs by available RAM")
    fi
fi

echo
echo "Patch applied to ${source_dir}. To build it yourself:"
echo "  cmake -S \"${source_dir}\" -B \"${source_dir}/build\" ${generator_args[*]} ${cmake_configure_args[*]}"
echo "  cmake --build \"${source_dir}/build\" -j${job_count}"
if [[ "${#speedup_notes[@]}" -gt 0 ]]; then
    notes_joined="${speedup_notes[0]}"
    for note in "${speedup_notes[@]:1}"; do
        notes_joined="${notes_joined}, ${note}"
    done
    echo
    echo "Detected and will use: ${notes_joined}."
    echo "(install ccache/ninja-build/mold via your package manager for more of these --"
    echo "e.g. on Fedora: sudo dnf install ccache ninja-build mold)"
fi
echo
echo "Once built, the binary you want is expected at:"
echo "  ${source_dir}/${binary_hint}"
echo "(confirm with e.g. \`find \"${source_dir}/build\" -name '*${emulator_name,,}*' -executable -type f\`"
echo "if it isn't there -- build layouts occasionally shift between upstream releases.)"

if [[ "${do_build}" -eq 1 ]]; then
    # CMake refuses to reconfigure an existing build dir with a
    # different generator than it was first configured with (e.g. a
    # build dir from a previous run of this script before ninja was
    # installed). Detect that up front and wipe it, rather than letting
    # `cmake -S -B` fail confusingly mid-run.
    desired_generator="Unix Makefiles"
    if [[ " ${generator_args[*]} " == *" Ninja "* ]]; then
        desired_generator="Ninja"
    fi
    cache_file="${source_dir}/build/CMakeCache.txt"
    if [[ -f "${cache_file}" ]]; then
        existing_generator="$(awk -F= '/^CMAKE_GENERATOR:INTERNAL=/ {print $2}' "${cache_file}")"
        if [[ -n "${existing_generator}" && "${existing_generator}" != "${desired_generator}" ]]; then
            echo "Existing build dir was configured with '${existing_generator}', not" >&2
            echo "'${desired_generator}' -- removing ${source_dir}/build to reconfigure cleanly" >&2
            echo "(this only removes CMake's build output, not your source checkout or its" >&2
            echo "applied patch)." >&2
            rm -rf "${source_dir}/build"
        fi
    fi
    echo
    echo "Building now (--build was passed) ..."
    cmake -S "${source_dir}" -B "${source_dir}/build" "${generator_args[@]}" "${cmake_configure_args[@]}"
    cmake --build "${source_dir}/build" -j"${job_count}"
    echo "Build complete."
    if [[ -x "${source_dir}/${binary_hint}" ]]; then
        echo "Binary confirmed at: ${source_dir}/${binary_hint}"
    else
        echo "warning: expected binary not found at ${source_dir}/${binary_hint} --" >&2
        echo "search the build directory for the actual output (see the find command" >&2
        echo "above)." >&2
    fi
fi
