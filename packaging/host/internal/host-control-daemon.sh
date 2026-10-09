#!/usr/bin/env bash
# The actual command the persistent Host Control systemd --user service
# (dualdeck-host-control.service, installed by
# install-host-control-daemon.sh) runs. A thin wrapper, not the unit's
# ExecStart directly, because dualdeck-host-service takes CLI flags
# (--auth-token, --app-version, --state-dir), not environment variables,
# and the unit file itself stays static/trivial.
#
# Deliberately never passes --adapter-socket: dualdeck-host-service then
# binds adapter-sdk/ipc/src/socket_path.cpp's defaultAdapterSocketPath()
# (the shared, well-known path) -- the exact socket every emulator
# launch path (run-host.sh, run-host-azahar.sh, run-host-cemu.sh, and
# the prebuilt AppImages' own AppRun) now probes first via
# scripts/lib/adapter_socket_probe.sh, so any of them connects to this
# daemon automatically and ModeCoordinator switches this same session
# from HostControl to Emulation mode with no re-launch/re-config needed
# anywhere. This is the one detail tying the persistent daemon to
# genuinely emulator-agnostic mode-switching.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
host_root="$(cd .. && pwd)"

DUALDECK_HOST_CONTROL_VERSION="$(cat "$(dirname "${host_root}")/VERSION" 2>/dev/null || true)"

export DUALDECK_HOST_CONTROL_VERSION

# Real user report, 2026-08-03: "Host Control Screen mirroring still does
# not work, as falls back to a grey screen" -- even after the X11/Wayland
# portal+PipeWire capture backends were built and unit-verified,
# DUALDECK_HOSTCONTROL_MIRROR_SCREEN (the env var HostControlAdapter's
# constructor actually gates capture on -- see host_control_adapter.cpp)
# was never exported by any real install/launch path, including this
# persistent daemon -- grep confirms it appears nowhere else in this
# repo's scripts. The feature was fully built but structurally
# unreachable without hand-editing a systemd unit override yourself.
# Defaulted on here (not in HostControlAdapter's own opt-in design,
# which stays unchanged for other launch paths like run-host.sh's manual
# Host-Control-only branch) since this is the actual, user-facing,
# always-running Host Control entry point, and the client already has
# its own Settings toggle (clientSettings.mirrorHostScreen) to opt out
# of *displaying* whatever the host sends -- still overridable via a
# systemd unit Environment= override for anyone who wants the host to
# never even attempt capture.
export DUALDECK_HOSTCONTROL_MIRROR_SCREEN="${DUALDECK_HOSTCONTROL_MIRROR_SCREEN:-1}"

auth_token_args=()
if [[ -n "${DUALDECK_HOST_CONTROL_AUTH_TOKEN:-}" ]]; then
    auth_token_args=(--auth-token "${DUALDECK_HOST_CONTROL_AUTH_TOKEN}")
fi

# Real user request, 2026-08-03: "if the client connects to the host and
# the host is on an older version, it should try to trigger an update if
# possible." This process is exactly the case that feature was scoped
# to -- a standalone daemon that can cleanly restart itself once
# apply-update.sh finishes (see that script's own "restart the daemon if
# it was active" logic, added for this exact purpose) -- unlike melonDS's
# in-process integration, which has no way to "restart itself" without
# losing the current game. NetServer only ever acts on this for a device
# identity already in the approved set (see net_server.cpp's own
# comment), never an unapproved one, so passing this unconditionally
# here is safe regardless of whether device-approval or static-token
# mode ends up active.
self_update_args=(--self-update "${host_root}/internal/apply-update.sh")

# Real user report, 2026-08-03 (Bazzite, PIPEWIRE_DEBUG=3): "can't make
# support.system handle: No such file or directory" -- dualdeck-host-
# service's linked libpipewire (built on an Ubuntu CI runner) has its own
# SPA plugin search path baked in at PipeWire's own build time, pointing
# at Ubuntu's layout, which doesn't exist on Fedora-based hosts like
# Bazzite -- so WaylandScreenCapture's PipeWire client never even got as
# far as this project's own format/buffer negotiation code. See
# pipewire_env.sh's own comment for why this points at the host's own
# installed PipeWire instead of bundling DualDeck's own copies.
# shellcheck source=scripts/lib/pipewire_env.sh
source ./pipewire_env.sh
find_and_export_pipewire_dirs

# See build-release.sh's bundle_library_dependencies() call for this
# binary -- host/internal/lib ships its runtime deps (libturbojpeg) so
# it runs under systemd --user the same way on any host, immutable or
# not (the real Bazzite bug this fixes: this daemon previously couldn't
# even be installed on rpm-ostree systems because of it).
exec env LD_LIBRARY_PATH="${host_root}/internal/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
    "${host_root}/internal/dualdeck-host-service" --adapter-ipc \
    --state-dir "${HOME}/.config/melonds-remote" "${auth_token_args[@]}" \
    --app-version "${DUALDECK_HOST_CONTROL_VERSION}" "${self_update_args[@]}"
