#!/usr/bin/env bash
# Single entry point for the host Steam Big Picture/Gaming Mode shortcut
# (GitHub issue #10: "the installed host can be launched from Steam Big
# Picture or Gaming Mode and accept a client connection"). Picks the
# right launch path depending on whether this is an immutable
# (rpm-ostree, e.g. Bazzite) system or a regular one, so the same
# shortcut works either way without the user needing to know which
# applies to their system. install-steam-shortcut.sh points the Steam
# shortcut's Exe at this script, never at melonDS or run-host.sh
# directly.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

# Real Bazzite hardware report, 2026-08-02: Host Control mode
# (DUALDECK_HOST_CONTROL=1) never worked on immutable systems at all --
# install-host-distrobox.sh flatly refused it (see that script's own
# comment), even though its own actual reason (needing Qt6/SDL2 that
# only a Distrobox container can provide) doesn't apply to it:
# dualdeck-host-service links no Qt/SDL at all, only libturbojpeg, which
# is now bundled alongside it (see build-release.sh's
# bundle_library_dependencies() call) precisely so it runs the same way
# on any host regardless of package-manager mutability. Route Host
# Control mode straight to run-host.sh even here -- it doesn't need a
# container -- and reserve the Distrobox path for an actual melonDS GUI
# launch, which still does.
if [[ "${DUALDECK_HOST_CONTROL:-0}" == "1" ]]; then
    exec ./run-host.sh "$@"
elif [[ -f /run/ostree-booted ]] || command -v rpm-ostree >/dev/null 2>&1; then
    exec ./install-host-distrobox.sh "$@"
else
    exec ./run-host.sh "$@"
fi
