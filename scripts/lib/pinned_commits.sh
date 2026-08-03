#!/usr/bin/env bash
# Single source of truth for the upstream commit each emulator patch is
# pinned against, meant to be `source`d by every script that clones and
# builds one of these emulators (scripts/build-release.sh,
# scripts/emudeck-replace-in-place.sh, and, as of 2026-08-03,
# scripts/patch-existing-emulator.sh -- see that script's own comment on
# why it used to keep its own separate, driftable copy of these same
# three commits). Factored out so a second script that needs to build
# the exact same patched binary can't silently drift onto a different
# commit than the release pipeline uses -- exactly the class of
# two-places-disagreeing bug that once shipped two releases with stale
# Azahar binaries (see build_azahar()'s own comment in
# scripts/lib/build_emulator.sh), and that patch-existing-emulator.sh's
# own separate CEMU_COMMIT had independently drifted into: real user
# report, 2026-08-03 -- "DualDeck compiles a nightly build" -- traced to
# patch-existing-emulator.sh still pinning a genuinely-untagged,
# months-past-v2.6 Cemu commit while this file (used by the actual
# release pipeline) already correctly pointed at v2.6. Pinning here,
# once, is the fix: every consumer of this file automatically gets
# whatever the latest-verified-stable commit is, with no second copy to
# forget to update.
#
# Real user policy request, 2026-08-03: "make sure DualDeck only uses
# the latest stable version, unless specified by the user during the
# patching." This file is exactly that default -- always a tagged
# stable release, never a nightly/dev commit, chosen deliberately each
# time it's bumped (not just "whatever HEAD happened to be") -- and
# scripts/patch-existing-emulator.sh's new --commit override (see its
# own comment) is the explicit, opt-in escape hatch for a user who
# wants something else on purpose.

MELONDS_COMMIT="10a173b5536fc75cd93f8a3868349dad963542ef"
AZAHAR_COMMIT="75134fca82eab4e1a86dca0aaa4a188cefff5469"
CEMU_COMMIT="a6fb0a48eb437a8a41c13b782ac8ae0433bf8f98" # v2.6, latest stable release

# Cemu (unlike melonDS/Azahar, which bake a fixed version into their own
# CMakeLists.txt) only knows its own version number if the build
# explicitly passes it via -DEMULATOR_VERSION_MAJOR/-DEMULATOR_VERSION_MINOR
# CMake cache variables -- Cemu's own src/Common/version.h falls back to
# showing the raw commit hash instead of a real version string
# ("// no version provided. Only show commit hash") when these default
# to 0. Real bug this fixes, discovered while investigating the
# "nightly build" report above: scripts/lib/build_emulator.sh's
# build_cemu() never passed these at all, so a build of this exact pinned
# v2.6 commit still self-identified in Cemu's own UI as its bare commit
# hash (which is exactly what made a genuinely-stable build look like a
# nightly to begin with) -- and separately, VulkanRenderer.cpp uses
# VK_MAKE_VERSION(EMULATOR_VERSION_MAJOR, ...) for VkApplicationInfo::
# applicationVersion, which some GPU vendor drivers key app-specific
# compatibility profiles off of, making version 0.0.0 vs 2.6.0 a
# plausible (not confirmed) contributor to Vulkan behavior differing
# from EmuDeck's official build -- worth fixing regardless of whether
# it's the actual cause of any specific Vulkan issue. Keep in sync with
# CEMU_COMMIT above every time it's bumped to a newer tag.
CEMU_VERSION_MAJOR="2"
CEMU_VERSION_MINOR="6"
