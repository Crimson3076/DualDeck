#!/usr/bin/env bash
# Shared "connect to the persistent Host Control daemon's socket if one's
# already listening, else spawn a private, ephemeral dualdeck-host-
# service" logic -- meant to be `source`d (not executed directly),
# copied into host/internal/ by scripts/build-release.sh alongside
# ensure-packages.sh/host_firewall.sh, same pattern.
#
# Factored out of generate_apprun_out_of_process()'s (scripts/lib/
# apprun_templates.sh) already-proven inline version of this exact
# logic -- that one has to stay a separate, embedded copy (an AppImage's
# AppRun can't reach outside the .AppImage's own bundled contents at
# runtime to source a file from here), kept in sync by hand; see that
# function's own comment for the cross-reference back to this file.
#
# Mirrors adapter-sdk/ipc/src/socket_path.cpp's defaultAdapterSocketPath()
# exactly (kept in sync here manually -- there's no shared machine-
# readable source of truth between bash and C++ for this, same
# "kept in sync here manually" convention host_firewall.sh's own
# port-list comment already uses).

# default_adapter_socket_path
#
# Echoes the shared, well-known adapter socket path a persistent Host
# Control daemon (scripts/build-release.sh's host-control-daemon.sh, if
# installed and running) listens on.
default_adapter_socket_path() {
    if [[ -n "${XDG_RUNTIME_DIR:-}" ]]; then
        echo "${XDG_RUNTIME_DIR}/dualdeck/adapter.sock"
    else
        echo "${HOME}/.cache/dualdeck/adapter.sock"
    fi
}

# is_adapter_socket_live <socket_path>
#
# True if something is actually listening at <socket_path> right now --
# a stale socket *file* left behind by a process that crashed without
# cleaning up doesn't count on its own (a plain [[ -S ... ]] check alone
# would give a false positive there, so this always also attempts a real
# connect). python3 is assumed present, matching this project's existing
# convention (steam_shortcut.py, appimage_manifest.py, smoke_test.py are
# all python3-implemented with no availability check either).
is_adapter_socket_live() {
    local socket_path="$1"
    [[ -S "${socket_path}" ]] && python3 -c '
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(1)
try:
    s.connect(sys.argv[1])
    sys.exit(0)
except OSError:
    sys.exit(1)
' "${socket_path}" 2>/dev/null
}

# probe_or_spawn_adapter_socket <private_socket_path> <host_service_bin> <state_dir> <app_version> [extra_args...]
#
# Real user request, 2026-08-01: "it should get toggled/switch over if
# it detects that a compatible emulator or separate server is trying to
# open" -- probes the shared default socket first, so a persistent Host
# Control daemon (if the user has set one up) is found and connected to
# automatically, letting ModeCoordinator switch that same daemon's
# session straight from HostControl to Emulation mode with no re-launch
# or re-config needed here. Only if nothing answers there does this fall
# back to spawning a private, ephemeral dualdeck-host-service on
# <private_socket_path>, exactly like every one of these launcher
# scripts used to do unconditionally before this existed.
#
# Sets (does not export) two variables in the CALLER's scope: ADAPTER_
# SOCKET (the socket path to actually connect to) and HOST_SERVICE_PID
# (only set to a nonempty pid in the fallback-spawn case). The caller
# owns installing its own `trap 'kill "${HOST_SERVICE_PID}" ...' EXIT`
# -- deliberately not done here, since each existing caller already has
# its own script-specific trap-timing comment (e.g. run-host-azahar.sh's
# "Not exec'd below" note) explaining exactly why its trap lives where
# it does; centralizing the trap itself here would either duplicate or
# disagree with those.
probe_or_spawn_adapter_socket() {
    local private_socket_path="$1" host_service_bin="$2" state_dir="$3" app_version="$4"
    shift 4
    local extra_args=("$@")

    local default_socket
    default_socket="$(default_adapter_socket_path)"

    HOST_SERVICE_PID=""
    if is_adapter_socket_live "${default_socket}"; then
        echo "DualDeck: found a running Host Control daemon at ${default_socket}, connecting to it" >&2
        ADAPTER_SOCKET="${default_socket}"
        return 0
    fi

    echo "DualDeck: no persistent Host Control daemon running -- starting a private one for this session" >&2
    mkdir -p "$(dirname "${private_socket_path}")"
    rm -f "${private_socket_path}"
    "${host_service_bin}" --adapter-ipc --adapter-socket "${private_socket_path}" \
        --state-dir "${state_dir}" --app-version "${app_version}" "${extra_args[@]}" &
    HOST_SERVICE_PID=$!
    sleep 0.5 # let the listener bind before the caller's emulator tries to connect
    ADAPTER_SOCKET="${private_socket_path}"
}
