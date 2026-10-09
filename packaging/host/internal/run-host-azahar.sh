#!/usr/bin/env bash
# Runs the patched Azahar binary (Nintendo 3DS) -- see
# docs/azahar-integration-analysis.md and
# docs/adr/0001-host-service-and-adapter-architecture.md (section 11)
# in the DualDeck repository this was built from.
#
# Unlike run-host.sh's melonDS, Azahar's integration has only ever been
# built as an out-of-process adapter -- Azahar itself has no in-process
# approval dialog. This script always starts the standalone Host Service
# in the background first, the same way run-host.sh's opt-in
# host-control mode does; it pops the same zero-typing kdialog Yes/No
# approval prompt melonDS's in-process dialog gives you (see
# kdialog_approval_prompt.h), unless AZAHAR_REMOTE_AUTH_TOKEN is set, in
# which case that static shared secret is used instead. Normally
# launched via ../dualdeck-host.sh's "Launch..." menu, not
# directly.
#
# Pick a ROM through Azahar's own game list / File > Load File once it
# opens, same as any other Azahar launch.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
host_root="$(cd .. && pwd)"

# shellcheck source=scripts/lib/ensure-packages.sh
source ./ensure-packages.sh

ensure_packages "azahar runtime" \
    "qt6-base-dev libvulkan1 libsdl2-2.0-0 libopenal1 libboost-iostreams1.83.0 libboost-thread1.83.0" \
    "qt6-qtbase vulkan-loader SDL2 openal-soft boost-iostreams boost-thread" \
    "qt6-base vulkan-icd-loader sdl2 openal-soft boost-libs" \
    || echo "warning: could not verify/install Azahar runtime libraries automatically; see docs/troubleshooting.md" >&2

# Real user report, 2026-08-03: "Azahar now does not open fully, it
# opens as a background process, but does not render a window." Root
# cause: this script used to run azahar directly against Bazzite's own
# minimal base image (see this comment's own former text, now removed --
# "Azahar's Distrobox launch path isn't built yet"), which doesn't ship
# Qt6 -- unlike Cemu's GTK3/Vulkan/PulseAudio deps, which a gaming-
# focused base image is far more likely to already have. Qt6 silently
# falling back to a non-visible platform when it can't find a real
# xcb/wayland platform plugin (the exact same failure mode already fixed
# for the AppImage path via bundled Qt platform plugins, see
# apprun_templates.sh) is exactly "process alive, no window." Fixed by
# routing through the same "dualdeck-host" Distrobox container
# install-host-distrobox.sh already creates/provisions for melonDS
# (confirmed to already carry qt6-qtbase-devel and friends) -- see
# on_immutable_system below.
on_immutable_system=0
if [[ -f /run/ostree-booted ]] || command -v rpm-ostree >/dev/null 2>&1; then
    on_immutable_system=1
fi

if [[ ! -x "${host_root}/internal/dualdeck-host-service" ]]; then
    echo "error: host/internal/dualdeck-host-service is missing from this install --" >&2
    echo "re-download the release archive." >&2
    exit 1
fi
if [[ ! -x "${host_root}/azahar" ]]; then
    echo "error: host/azahar is missing from this install -- re-download the" >&2
    echo "release archive." >&2
    exit 1
fi

# shellcheck source=scripts/lib/adapter_socket_probe.sh
source ./adapter_socket_probe.sh

# See run-host.sh's identical MELONDS_REMOTE_VERSION comment -- same
# central VERSION file, read the same way.
export AZAHAR_REMOTE_VERSION="$(cat "$(dirname "${host_root}")/VERSION" 2>/dev/null || true)"

auth_token_args=()
if [[ -n "${AZAHAR_REMOTE_AUTH_TOKEN:-}" ]]; then
    auth_token_args=(--auth-token "${AZAHAR_REMOTE_AUTH_TOKEN}")
fi

probe_or_spawn_adapter_socket "${HOME}/.config/dualdeck/run/azahar-adapter.sock" \
    "${host_root}/internal/dualdeck-host-service" "${HOME}/.config/melonds-remote" \
    "${AZAHAR_REMOTE_VERSION}" "${auth_token_args[@]}"
adapter_socket="${ADAPTER_SOCKET}"
if [[ -n "${HOST_SERVICE_PID}" ]]; then
    # Not exec'd below, same reason as run-host.sh's host-control branch:
    # this trap needs to still be able to run once Azahar exits -- only
    # meaningful when probe_or_spawn_adapter_socket() actually spawned a
    # private Host Service above; nothing to clean up here if this
    # connected to an already-running persistent daemon instead.
    trap 'kill "${HOST_SERVICE_PID}" 2>/dev/null || true' EXIT
fi

export AZAHAR_REMOTE_ENABLE=1
export AZAHAR_REMOTE_ADAPTER_SOCKET="${adapter_socket}"
# Performance tuning (optional): set AZAHAR_REMOTE_CAPTURE_FPS (1-60,
# default 60) before running this script to change how often the video
# capture loop polls for a new frame -- no rebuild needed, e.g.
# `AZAHAR_REMOTE_CAPTURE_FPS=30 ./dualdeck-host.sh` if 60 turns
# out to visibly affect the game's own performance on your hardware.
# Also AZAHAR_REMOTE_CAPTURE_SCALE (1-10): overrides how many multiples
# of the bottom screen's native 320x240 get streamed, e.g.
# `AZAHAR_REMOTE_CAPTURE_SCALE=2 ./dualdeck-host.sh` for 640x480. Not
# needed just to match Azahar's own "Internal Resolution" graphics
# setting -- the stream already follows that automatically now (see
# docs/known-limitations.md's capture-scale-follows-resolution-factor
# entry); only set this to stream at a different resolution than what's
# configured there, e.g. a sharper local picture than is worth sending
# over a particular link.
# Already inherited by the exec below with no extra wiring needed --
# see docs/known-limitations.md's performance-tuning entry.

# Works around a known class of Qt6-on-Linux crash (reported for many
# Qt6 apps, not specific to Azahar): a GTK3 platform-theme bug in
# window parenting can crash the process the moment a native file
# dialog opens -- exactly the "crashes when browsing files to pick a
# ROM" symptom this project's own user hit with File > Load File. An
# empty QT_QPA_PLATFORMTHEME disables that native GTK integration
# entirely, so Qt falls back to its own built-in (non-native) file
# dialog implementation, which isn't affected. The only downside is
# cosmetic: dialogs render in Qt's own style instead of matching the
# desktop's native GTK theme.
export QT_QPA_PLATFORMTHEME=""

if [[ "${on_immutable_system}" -eq 1 ]]; then
    container_name="dualdeck-host"
    # Deliberately an actual `distrobox enter ... -- true`, NOT
    # `distrobox list | grep -q`. Real user report, 2026-08-04, with the
    # root cause diagnosed in the report itself: `grep -q` exits the
    # moment it matches, which SIGPIPEs the still-writing `distrobox
    # list`, and under this script's own `set -o pipefail` that makes
    # the whole pipeline report failure *even though the container was
    # found*. The result was this branch firing on Bazzite hosts where
    # `distrobox list` plainly showed a running, fully provisioned
    # "dualdeck-host" and `distrobox enter` worked fine -- i.e. Azahar
    # refused to start precisely when everything it needed was present.
    # Entering is also the stronger check: it proves the container is
    # usable, not merely listed, which is the thing that actually has to
    # hold before Azahar is launched inside it.
    if ! command -v distrobox >/dev/null 2>&1 || \
       ! distrobox enter "${container_name}" -- true >/dev/null 2>&1; then
        echo "error: this looks like an immutable (rpm-ostree) system, e.g. Bazzite, but the" >&2
        echo "\"${container_name}\" Distrobox container Azahar needs (for Qt6, which Bazzite's" >&2
        echo "own base image doesn't ship) could not be entered -- it either doesn't exist" >&2
        echo "yet or isn't usable. Launch melonDS at least once first (../dualdeck-host.sh" >&2
        echo "-> DS/melonDS), which creates and provisions it -- or run" >&2
        echo "internal/install-host-distrobox.sh --install-only directly to create or" >&2
        echo "repair it." >&2
        exit 1
    fi
    # Distrobox forwards DISPLAY/WAYLAND_DISPLAY/XDG_RUNTIME_DIR and
    # mounts $HOME automatically (its whole point) -- host_root/
    # adapter_socket are both under $HOME, so no path translation is
    # needed. Explicit `env` (not relying on distrobox enter inheriting
    # this script's own exported vars into the container) since that's
    # not guaranteed for arbitrary non-XDG env vars, matching
    # install-host-distrobox.sh's own identical `env` pattern for
    # melonDS. Deliberately NOT exec'd -- same reason as the
    # HOST_SERVICE_PID trap set above: this shell needs to still be
    # alive to run that cleanup trap once Azahar (running inside the
    # container) actually exits.
    distrobox enter "${container_name}" -- env \
        "AZAHAR_REMOTE_ENABLE=1" \
        "AZAHAR_REMOTE_ADAPTER_SOCKET=${adapter_socket}" \
        "AZAHAR_REMOTE_VERSION=${AZAHAR_REMOTE_VERSION}" \
        "QT_QPA_PLATFORMTHEME=" \
        "${host_root}/azahar" "$@"
else
    "${host_root}/azahar" "$@"
fi
