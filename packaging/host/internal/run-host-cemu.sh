#!/usr/bin/env bash
# Runs the patched Cemu binary (Nintendo Wii U) -- see
# host/cemu-patches/README.md and
# docs/adr/0001-host-service-and-adapter-architecture.md in the
# DualDeck repository this was built from.
#
# Same out-of-process-only shape as run-host-azahar.sh: Cemu has no
# in-process device-approval dialog either, so this script always
# starts the standalone Host Service in the background first, which
# pops the same zero-typing kdialog Yes/No approval prompt melonDS's
# own in-process dialog gives you (see kdialog_approval_prompt.h),
# unless CEMU_REMOTE_AUTH_TOKEN is set, in which case that static
# shared secret is used instead. Normally launched via
# ../dualdeck-host.sh's "Launch..." menu, not directly.
#
# Pick a game through Cemu's own game list / File > Load once it opens,
# same as any other Cemu launch. The GamePad/DRC screen streams to the
# DualDeck client; the TV output stays on this host's own display, same
# "host shows one screen, client shows the other" split as melonDS/
# Azahar.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
host_root="$(cd .. && pwd)"

# shellcheck source=scripts/lib/ensure-packages.sh
source ./ensure-packages.sh

ensure_packages "cemu runtime" \
    "libvulkan1 freeglut3 libgtk-3-0 libpulse0 libsecret-1-0 libsystemd0 libbluetooth3 libgcrypt20 libusb-1.0-0" \
    "vulkan-loader freeglut gtk3 pulseaudio-libs libsecret systemd-libs bluez-libs libgcrypt libusb1" \
    "vulkan-icd-loader freeglut gtk3 libpulse libsecret systemd bluez-libs libgcrypt libusb" \
    || echo "warning: could not verify/install Cemu runtime libraries automatically; see docs/troubleshooting.md" >&2

if [[ -f /run/ostree-booted ]] || command -v rpm-ostree >/dev/null 2>&1; then
    echo "Note: this looks like an immutable (rpm-ostree) system, e.g. Bazzite -- Cemu's" >&2
    echo "Distrobox launch path isn't built yet (see host/cemu-patches/README.md), so" >&2
    echo "this may fail if a required library isn't already present on the base image." >&2
fi

if [[ ! -x "${host_root}/internal/dualdeck-host-service" ]]; then
    echo "error: host/internal/dualdeck-host-service is missing from this install --" >&2
    echo "re-download the release archive." >&2
    exit 1
fi
if [[ ! -x "${host_root}/cemu" ]]; then
    echo "error: host/cemu is missing from this install -- re-download the" >&2
    echo "release archive." >&2
    exit 1
fi

# shellcheck source=scripts/lib/adapter_socket_probe.sh
source ./adapter_socket_probe.sh

# See run-host.sh's identical MELONDS_REMOTE_VERSION comment -- same
# central VERSION file, read the same way.
export CEMU_REMOTE_VERSION="$(cat "$(dirname "${host_root}")/VERSION" 2>/dev/null || true)"

auth_token_args=()
if [[ -n "${CEMU_REMOTE_AUTH_TOKEN:-}" ]]; then
    auth_token_args=(--auth-token "${CEMU_REMOTE_AUTH_TOKEN}")
fi

probe_or_spawn_adapter_socket "${HOME}/.config/dualdeck/run/cemu-adapter.sock" \
    "${host_root}/internal/dualdeck-host-service" "${HOME}/.config/melonds-remote" \
    "${CEMU_REMOTE_VERSION}" "${auth_token_args[@]}"
adapter_socket="${ADAPTER_SOCKET}"
if [[ -n "${HOST_SERVICE_PID}" ]]; then
    # Not exec'd below, same reason as run-host.sh's host-control branch:
    # this trap needs to still be able to run once Cemu exits -- only
    # meaningful when probe_or_spawn_adapter_socket() actually spawned a
    # private Host Service above; nothing to clean up here if this
    # connected to an already-running persistent daemon instead.
    trap 'kill "${HOST_SERVICE_PID}" 2>/dev/null || true' EXIT
fi

export CEMU_REMOTE_ENABLE=1
export CEMU_REMOTE_ADAPTER_SOCKET="${adapter_socket}"
# Performance tuning (optional): set CEMU_REMOTE_CAPTURE_FPS (1-60,
# default 30) before running this script to change how often the
# GamePad/TV video capture is allowed to run -- no rebuild needed, e.g.
# `CEMU_REMOTE_CAPTURE_FPS=20 ./dualdeck-host.sh` if 30 turns out to
# visibly affect the game's own performance on your hardware (Vulkan's
# capture path is a real GPU sync every time it runs, see
# host/cemu-patches/README.md). Already inherited by the exec below
# with no extra wiring needed.
"${host_root}/cemu" "$@"
