#!/usr/bin/env bash
# DualDeck's Azahar launcher for EmuDeck / Steam ROM Manager shortcuts.
# Companion to launch-emudeck-melonds.sh -- see that file's header for
# why these shims exist and why EmuDeck's all.sh is not sourced.
#
# Much thinner than the melonDS one, because run-host-azahar.sh already
# does the hard parts itself: it detects an immutable system, enters the
# dualdeck-host container, starts/finds the Host Service, and sets the
# adapter environment. Real user report, 2026-08-04: simply pointing
# EmuDeck's azahar.sh at run-host-azahar.sh made existing Steam ROM
# Manager 3DS shortcuts work, with no shortcut regeneration.
#
# What it replaces is EmuDeck's launcher running ~/Applications/
# azahar.AppImage directly. That AppImage is correctly patched, but on
# Bazzite it dies in Vulkan init with
#   vk::createInstanceUnique: ErrorIncompatibleDriver
# because it runs against the base image with its own bundled library
# path, rather than inside the container where the same build and the
# same ROM work.
#
# Arguments are passed through untouched, unlike the melonDS shim: Azahar
# accepts a bare ROM path, and the reporter's fix needed no argument
# translation. If Steam ROM Manager is ever configured to append flags
# Azahar rejects, this is where that would be handled.
set -euo pipefail

install_root="${HOME}/.config/dualdeck/install"
wrapper="${install_root}/internal/run-host-azahar.sh"

log="${HOME}/.cache/dualdeck/azahar-steam-launch.log"
mkdir -p "$(dirname "${log}")"
{
    echo "=== $(date --iso-8601=seconds) pid=$$ ==="
    printf 'arg: %q\n' "$@"
} >> "${log}" 2>&1

if [[ ! -x "${wrapper}" ]]; then
    echo "error: DualDeck's Azahar wrapper is missing: ${wrapper}" >&2
    echo "Re-run the DualDeck Host menu's \"Patch my EmuDeck-installed emulators\"." >&2
    echo "error: missing wrapper ${wrapper}" >> "${log}"
    exit 1
fi

if [[ "${DUALDECK_EMUDECK_LAUNCH_DRY_RUN:-0}" == "1" ]]; then
    printf '%q ' "${wrapper}" "$@"; echo
    exit 0
fi

exec "${wrapper}" "$@"
