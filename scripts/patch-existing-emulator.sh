#!/usr/bin/env bash
# Applies the DualDeck remote-server integration patch to an existing
# melonDS or Azahar source checkout you already have, instead of
# downloading/building a separate DualDeck-managed copy via
# scripts/build-release.sh -- for anyone who already has one of these
# emulators set up the way they like it and doesn't want a duplicate
# install alongside it.
#
# Usage:
#   scripts/patch-existing-emulator.sh --system ds   --source /path/to/your/melonDS/checkout [--build]
#   scripts/patch-existing-emulator.sh --system 3ds  --source /path/to/your/azahar/checkout  [--build]
#
# Without --build, only applies the patch (git apply) and prints the
# commands to build it yourself. With --build, also configures and
# builds it the same way scripts/build-release.sh does for its own
# bundled copies (plain `cmake --build`, not a full release package).
#
# Once built, point host/melonds-remote-host.sh's "Launch..." -> "Custom"
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
    echo "usage: $0 --system <ds|3ds> --source /path/to/checkout [--build]" >&2
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
    *)
        echo "error: --system must be 'ds' or '3ds', got '${system}'" >&2
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

echo
echo "Patch applied to ${source_dir}. To build it yourself:"
echo "  cmake -S \"${source_dir}\" -B \"${source_dir}/build\" -DCMAKE_BUILD_TYPE=Release"
echo "  cmake --build \"${source_dir}/build\" -j\$(nproc)"
echo
echo "Once built, the binary you want is expected at:"
echo "  ${source_dir}/${binary_hint}"
echo "(confirm with e.g. \`find \"${source_dir}/build\" -name '*${emulator_name,,}*' -executable -type f\`"
echo "if it isn't there -- build layouts occasionally shift between upstream releases.)"

if [[ "${do_build}" -eq 1 ]]; then
    echo
    echo "Building now (--build was passed) ..."
    cmake -S "${source_dir}" -B "${source_dir}/build" -DCMAKE_BUILD_TYPE=Release
    cmake --build "${source_dir}/build" -j"$(nproc)"
    echo "Build complete."
    if [[ -x "${source_dir}/${binary_hint}" ]]; then
        echo "Binary confirmed at: ${source_dir}/${binary_hint}"
    else
        echo "warning: expected binary not found at ${source_dir}/${binary_hint} --" >&2
        echo "search the build directory for the actual output (see the find command" >&2
        echo "above)." >&2
    fi
fi
