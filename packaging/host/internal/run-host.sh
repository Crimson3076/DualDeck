#!/usr/bin/env bash
# Runs the patched melonDS binary with the remote server enabled,
# auto-installing any missing runtime system libraries first (Qt6, SDL2,
# libarchive, libenet, libfaad, etc. -- see
# host-shared-library-dependencies.txt for the exact list this binary
# was linked against).
# No arguments needed: this just opens melonDS itself, and you pick a
# ROM through its own File > Open ROM dialog same as any other launch
# (it defaults to EmuDeck's ROM folder the first time, and remembers
# your last one after that). A path can still be passed through if you
# want a specific one to open immediately, e.g.
# ./run-host.sh --boot always /path/to/your/game.nds -- see melonDS's
# own --help for the rest.
# Omit MELONDS_REMOTE_AUTH_TOKEN (the default) to use zero-typing
# device-approval authentication instead of a static shared secret.
# Normally launched via ../../dualdeck-host.sh's "Launch now" menu
# choice, not directly.
#
# Real user report, 2026-08-28: on a fresh install (no melonDS.ini yet
# with a saved window size) melonDS opens small and windowed, title bar
# and all -- neither matches the Deck's display, and every other emulator
# DualDeck patches (Cemu, Azahar) already goes fullscreen on launch. Pass
# melonDS's own -f/--fullscreen (CLI.cpp: `win->toggleFullscreen()`,
# which uses Qt's real fullscreen mode against the current screen, so it
# always matches the host's actual resolution -- no title bar, no
# stretching/letterboxing to guess at) by default here, unless the
# caller already asked for a specific fullscreen state, or opted out with
# DUALDECK_MELONDS_WINDOWED=1 (e.g. for setup/debugging on a desktop).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
host_root="$(cd .. && pwd)"

# shellcheck source=scripts/lib/ensure-packages.sh
source ./ensure-packages.sh

ensure_packages "host runtime" \
    "libcurl4-gnutls-dev libpcap0.8-dev libsdl2-dev libarchive-dev libenet-dev libzstd-dev libfaad-dev qt6-base-dev qt6-base-private-dev qt6-multimedia-dev qt6-svg-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libwayland-dev libxkbcommon-dev libdrm-dev libgbm-dev libdecor-0-dev libturbojpeg0-dev" \
    "libcurl-devel libpcap-devel SDL2-devel libarchive-devel enet-devel libzstd-devel faad2-devel qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtmultimedia-devel qt6-qtsvg-devel libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXfixes-devel libXi-devel libXScrnSaver-devel wayland-devel libxkbcommon-devel libdrm-devel mesa-libgbm-devel libdecor-devel turbojpeg-devel" \
    "curl libpcap sdl2 libarchive enet zstd faad2 qt6-base qt6-multimedia qt6-svg libx11 libxext libxrandr libxcursor libxfixes libxi libxss wayland libxkbcommon libdrm mesa libdecor libjpeg-turbo" \
    || echo "warning: could not verify/install host runtime libraries automatically; see host-shared-library-dependencies.txt and docs/troubleshooting.md" >&2

if [[ -f /run/ostree-booted ]] || command -v rpm-ostree >/dev/null 2>&1; then
    echo "Note: this looks like an immutable (rpm-ostree) system, e.g. Bazzite --" >&2
    echo "if melonDS fails to start below over a missing shared library, use" >&2
    echo "./install-host-distrobox.sh instead, which runs it inside a Distrobox" >&2
    echo "container with everything it needs already installed. See" >&2
    echo "docs/bazzite-host-setup.md." >&2
fi

export MELONDS_REMOTE_ENABLE=1
# Read by the patched melonDS's remote-server integration (see
# host/melonds-patches/0001-remote-server-integration.patch's
# MELONDS_REMOTE_VERSION wiring) so it can reject a connecting client
# running a different, incompatible release -- see
# protocol.h's HelloPayload::appVersion. dirname(host_root) is the
# archive root (or the central install directory's parent), matching
# check-for-updates.sh's own VERSION lookup.
export MELONDS_REMOTE_VERSION="$(cat "$(dirname "${host_root}")/VERSION" 2>/dev/null || true)"

# Host-control mode (GitHub issue #4, experimental): set by
# ../dualdeck-host.sh's "Host control only -- no emulator" menu choice,
# not on by default. Starts the standalone dualdeck-host-service binary
# in --adapter-ipc mode and runs it in the foreground until Ctrl+C --
# see docs/adr/0001-host-service-and-adapter-architecture.md section 10
# for why this is a separate process from any emulator: a client needs
# somewhere to connect and navigate *before* an emulator has even
# started. Uses the same zero-typing device-approval flow as melonDS's
# own in-process dialog -- a kdialog Yes/No popup on the host's own
# desktop (see kdialog_approval_prompt.h) -- unless
# MELONDS_REMOTE_AUTH_TOKEN is set, in which case that static shared
# secret is used instead. --state-dir points at the same directory
# melonDS's own device-approval uses, so a device approved once is
# approved for every emulator.
#
# Real user report, 2026-08-01: this used to *also* launch melonDS
# immediately afterward (as an out-of-process adapter, so it would be
# "ready" the moment a ROM was picked). That defeated the entire point:
# ModeCoordinator switches to Emulation mode the instant *any* adapter
# connects, with no concept of "connected but idle, no ROM loaded yet"
# -- so melonDS's out-of-process bridge connecting within a fraction of
# a second of starting flipped the session out of Host Control mode
# before the user could ever interact with it, every single time,
# regardless of whether a ROM was ever actually loaded. The client sat
# silently in Emulation mode waiting for video frames that would never
# arrive (no ROM = nothing for melonDS to render), which looked
# identical to "the touchpad/buttons just don't work."
#
# This manual menu choice is a *temporary, private* Host Control session
# on its own private socket, separate from the real fix for "genuinely
# emulator-agnostic hand-off": a persistent Host Control daemon
# (host/internal/dualdeck-host-control.service, installed via
# install-host-control-daemon.sh -- see ../dualdeck-host.sh's own menu)
# that stays up independent of any single Steam shortcut/session and
# listens on the shared default socket every emulator's own launch path
# now probes first (scripts/lib/adapter_socket_probe.sh). If that
# persistent daemon is already running, this manual choice is almost
# never what you actually want -- it doesn't replace the daemon's own
# session, it just starts a second, separate, throwaway one on a
# different, private socket -- so this only ever prints an informational
# note about that rather than refusing to run (deliberately non-blocking:
# a user who genuinely wants a second, temporary session for some reason
# should still be able to get one).
if [[ "${DUALDECK_HOST_CONTROL:-0}" == "1" ]]; then
    if [[ ! -x "${host_root}/internal/dualdeck-host-service" ]]; then
        echo "error: host/internal/dualdeck-host-service is missing from this" >&2
        echo "install -- re-download the release archive." >&2
        exit 1
    fi

    if command -v systemctl >/dev/null 2>&1 && \
       systemctl --user is-active --quiet dualdeck-host-control.service 2>/dev/null; then
        echo "Note: a persistent Host Control daemon already appears to be running" >&2
        echo "(dualdeck-host-control.service) -- you probably don't need this manual" >&2
        echo "session too. Starting one anyway, on its own private socket ..." >&2
    fi

    run_dir="${HOME}/.config/dualdeck/run"
    mkdir -p "${run_dir}"
    adapter_socket="${run_dir}/adapter.sock"
    rm -f "${adapter_socket}"

    auth_token_args=()
    if [[ -n "${MELONDS_REMOTE_AUTH_TOKEN:-}" ]]; then
        auth_token_args=(--auth-token "${MELONDS_REMOTE_AUTH_TOKEN}")
    fi

    echo "Starting the standalone Host Service (host-control mode, experimental) --" >&2
    echo "no emulator will be launched. Use its own Steam shortcut/launcher" >&2
    echo "whenever you're ready to play something -- see this script's own" >&2
    echo "header comment for why that starts a separate session rather than" >&2
    echo "taking over this one (yet)." >&2
    # See host-control-daemon.sh's identical fix (pipewire_env.sh's own
    # comment has the full story) -- this manual Host-Control session can
    # also try to mirror the screen via PipeWire, so it needs the same fix.
    # shellcheck source=scripts/lib/pipewire_env.sh
    source ./pipewire_env.sh
    find_and_export_pipewire_dirs
    # See build-release.sh's bundle_library_dependencies() call for this
    # binary -- host/internal/lib ships its runtime deps (libturbojpeg)
    # so it runs on any host regardless of whether that library happens
    # to be installed system-wide (the real Bazzite bug this fixes).
    exec env LD_LIBRARY_PATH="${host_root}/internal/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
        "${host_root}/internal/dualdeck-host-service" --adapter-ipc --adapter-socket "${adapter_socket}" \
        --state-dir "${HOME}/.config/melonds-remote" "${auth_token_args[@]}" \
        --app-version "${MELONDS_REMOTE_VERSION}"
fi

# Real user request, 2026-08-01: auto-switch mode "if it detects that a
# compatible emulator or separate server is trying to open" -- if a
# persistent Host Control daemon is already listening on the shared
# default socket, connect melonDS to it out-of-process (letting
# ModeCoordinator switch that session straight to Emulation mode)
# instead of always defaulting to melonDS's own in-process remote
# server. Deliberately just a probe, not a spawn-if-missing (unlike
# probe_or_spawn_adapter_socket(), used by Azahar/Cemu, which have no
# in-process fallback at all): melonDS's in-process default is already
# proven and correct for the common case of no daemon running, so this
# only ever adds a better path on top of it, never removes the existing
# one.
# shellcheck source=scripts/lib/adapter_socket_probe.sh
source ./adapter_socket_probe.sh
# 2026-10-09: no longer only when the daemon happens to be running --
# melonDS now always goes out-of-process through dualdeck-host-service
# (the daemon if it's up, otherwise a private one for this session, the
# same probe_or_spawn_adapter_socket() Azahar/Cemu use), because
# melonDS's in-process server is a frozen, vendored NetServer copy that
# never gained H.264/PyroWave. Falls back to that in-process server if
# the host service is missing or dies on startup;
# DUALDECK_MELONDS_IN_PROCESS=1 forces the fallback outright. See
# scripts/lib/apprun_templates.sh's generate_apprun_melonds() for the
# AppImage launch path's identical logic.
HOST_SERVICE_PID=""
if [[ "${DUALDECK_MELONDS_IN_PROCESS:-0}" != "1" && -x "${host_root}/internal/dualdeck-host-service" ]]; then
    auth_token_args=()
    if [[ -n "${MELONDS_REMOTE_AUTH_TOKEN:-}" ]]; then
        auth_token_args=(--auth-token "${MELONDS_REMOTE_AUTH_TOKEN}")
    fi
    probe_or_spawn_adapter_socket "${HOME}/.config/dualdeck/run/melonds-adapter.sock" \
        "${host_root}/internal/dualdeck-host-service" "${HOME}/.config/melonds-remote" \
        "${MELONDS_REMOTE_VERSION}" ${auth_token_args[@]+"${auth_token_args[@]}"}
    if [[ -n "${HOST_SERVICE_PID}" ]]; then
        for _ in $(seq 1 30); do
            [[ -S "${ADAPTER_SOCKET}" ]] && break
            kill -0 "${HOST_SERVICE_PID}" 2>/dev/null || break
            sleep 0.1
        done
        if ! kill -0 "${HOST_SERVICE_PID}" 2>/dev/null || [[ ! -S "${ADAPTER_SOCKET}" ]]; then
            echo "DualDeck: host service failed to start -- using melonDS's built-in server instead" >&2
            kill "${HOST_SERVICE_PID}" 2>/dev/null || true
            HOST_SERVICE_PID=""
            ADAPTER_SOCKET=""
        else
            trap 'kill "${HOST_SERVICE_PID}" 2>/dev/null || true' EXIT
        fi
    fi
    if [[ -n "${ADAPTER_SOCKET:-}" ]]; then
        export MELONDS_REMOTE_OUT_OF_PROCESS=1
        export MELONDS_REMOTE_ADAPTER_SOCKET="${ADAPTER_SOCKET}"
    fi
fi

melonds_args=("$@")
if [[ "${DUALDECK_MELONDS_WINDOWED:-0}" != "1" ]]; then
    fullscreen_requested=0
    for melonds_arg in ${melonds_args[@]+"${melonds_args[@]}"}; do
        case "${melonds_arg}" in
            -f|--fullscreen) fullscreen_requested=1 ;;
        esac
    done
    if [[ "${fullscreen_requested}" -eq 0 ]]; then
        melonds_args+=(--fullscreen)
    fi
fi
if [[ -n "${HOST_SERVICE_PID}" ]]; then
    # Not exec'd, so the EXIT trap above still stops the private host
    # service once melonDS exits.
    "${host_root}/melonDS" ${melonds_args[@]+"${melonds_args[@]}"}
else
    exec "${host_root}/melonDS" ${melonds_args[@]+"${melonds_args[@]}"}
fi
