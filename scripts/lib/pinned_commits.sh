#!/usr/bin/env bash
# Single source of truth for the upstream commit each emulator patch is
# pinned against, meant to be `source`d by both scripts/build-release.sh
# and scripts/emudeck-replace-in-place.sh. Previously these were defined
# directly in build-release.sh alone; factored out so a second script that
# needs to build the exact same patched binary (emudeck-replace-in-place.sh)
# can't silently drift onto a different commit than the release pipeline
# uses -- exactly the class of two-places-disagreeing bug that once shipped
# two releases with stale Azahar binaries (see build_azahar()'s own comment
# in scripts/lib/build_emulator.sh).

MELONDS_COMMIT="10a173b5536fc75cd93f8a3868349dad963542ef"
AZAHAR_COMMIT="75134fca82eab4e1a86dca0aaa4a188cefff5469"
CEMU_COMMIT="a6fb0a48eb437a8a41c13b782ac8ae0433bf8f98" # v2.6, latest stable release
