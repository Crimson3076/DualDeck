#!/usr/bin/env bash
# Runs the DualDeck host inside a Distrobox container, for
# immutable-filesystem systems (Bazzite, other rpm-ostree/Fedora Atomic
# derivatives) where run-host.sh can't install missing runtime libraries
# directly -- see docs/bazzite-host-setup.md. On a regular (non-immutable)
# Linux system, just use run-host.sh instead; this refuses to run there
# rather than needlessly creating a container. Normally launched via
# ../../dualdeck-host.sh's "Launch now"/"Add to Steam" menu
# choices, not directly.
#
# Safe to re-run any time, including from a newer release's extracted
# archive: it always re-syncs the whole host/ directory (this script's
# grandparent -- the entry-point script and binary alongside it, plus
# this internal/ directory) into a fixed location
# (~/.config/dualdeck/install/) and re-installs into the same
# Distrobox container (dnf skips packages already present), so updating
# is just "download the new release, run this again" -- no need to
# recreate the container or redo any setup by hand. After the first run
# you can also launch/update straight from that central directory
# instead of keeping the original download around. See
# uninstall-host-distrobox.sh to remove everything this creates.
#
# Update failures leave the previous install usable (GitHub issue #10):
# the new files are staged in a separate directory first and only
# swapped into place once staging AND the container package install both
# succeed -- if either fails partway, the working install from before
# this run is untouched, not deleted first and then possibly left
# missing. The swapped-out previous install is kept as one backup
# generation (*.previous) rather than deleted outright, in case you need
# to manually go back to it.
#
# Pass --install-only to prepare everything (central directory, Distrobox
# container, packages) without actually launching melonDS -- used by
# install-steam-shortcut.sh so registering the Steam shortcut doesn't
# also immediately start the emulator.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
host_root="$(cd .. && pwd)"
# shellcheck source=scripts/lib/host_firewall.sh
source ./host_firewall.sh

if [[ ! -f /run/ostree-booted ]] && ! command -v rpm-ostree >/dev/null 2>&1; then
    echo "This doesn't look like an immutable (rpm-ostree) system -- just run" >&2
    echo "./run-host.sh directly instead, no container needed." >&2
    exit 1
fi

# Host-control mode (GitHub issue #4) doesn't need a Distrobox container
# at all -- dualdeck-host-service links no Qt/SDL, only libturbojpeg,
# now bundled alongside it (see build-release.sh's
# bundle_library_dependencies() call and its own comment on the real
# Bazzite bug this fixes). This used to fail loudly here instead, on the
# now-outdated assumption that it needed the same GUI-library-heavy
# container melonDS itself does; launch-host.sh no longer even routes
# Host Control launches through this script, but delegate rather than
# error in case something still invokes this directly (e.g. a stale
# doc, or a manual run) -- ./run-host.sh handles it correctly on its
# own, container or not.
if [[ "${DUALDECK_HOST_CONTROL:-0}" == "1" ]]; then
    exec ./run-host.sh "$@"
fi

if ! command -v distrobox >/dev/null 2>&1; then
    echo "error: 'distrobox' not found. Bazzite ships it by default; on other" >&2
    echo "rpm-ostree systems, install it first -- see" >&2
    echo "https://github.com/89luca89/distrobox" >&2
    exit 1
fi

# Real user report, 2026-08-01 (Bazzite HTPC, launched via Steam
# shortcut): the client never saw this host at all -- not "discovered
# but refused," just nothing. Root cause: this script is normally
# launched from a Steam shortcut, so Steam's own LD_PRELOAD (overlay-
# injection libs, e.g. gameoverlayrenderer.so) is present in this
# process's environment and distrobox enter forwards it into the
# container by default. That crashes `env MELONDS_REMOTE_ENABLE=1 ...`
# below outright (exit 127, "error while loading shared libraries:
# libGL.so.1") before melonDS ever starts -- identical root cause,
# same fix, as scripts/emudeck-replace-in-place.sh's
# run_in_distrobox_build_container() (see docs/known-limitations.md's
# 2026-08-01 entry on that one). melonDS silently never launching
# explains the symptom exactly: no host process means no listening
# ports, which means nothing for the client to discover no matter how
# correct the firewall setup is.
unset LD_PRELOAD LD_LIBRARY_PATH

install_only=0
if [[ "${1:-}" == "--install-only" ]]; then
    install_only=1
    shift
fi

# Keep in sync with the same paths in uninstall-host-distrobox.sh,
# install-steam-shortcut.sh, uninstall-steam-shortcut.sh, and
# docs/bazzite-host-setup.md's description of this path.
central_install_dir="${HOME}/.config/dualdeck/install"
staging_dir="${central_install_dir}.new"
previous_dir="${central_install_dir}.previous"
container_name="dualdeck-host"

# One-time melonDS-Remote -> DualDeck rebrand cleanup: remove a leftover
# container from before the rename so this doesn't end up with two
# (the old one orphaned, plus a freshly created "dualdeck-host"). No-op
# if it doesn't exist.
#
# The list is captured into a variable first rather than piped straight
# into `grep -q` -- see run-host-azahar.sh's own comment for the full
# story: `grep -q` exits on its first match, SIGPIPEs the still-writing
# `distrobox list`, and `set -o pipefail` then reports the pipeline as
# failed even on a match. Here that failure mode is quiet rather than
# loud (the stale container just never gets cleaned up), which is
# exactly why it went unnoticed. Not `distrobox enter`, unlike Azahar's
# launch gate: starting a container purely to decide whether to delete
# it would be backwards.
distrobox_containers="$(if command -v distrobox >/dev/null 2>&1; then distrobox list 2>/dev/null || true; fi)"
if grep -qw "melonds-remote-host" <<<"${distrobox_containers}"; then
    echo "Removing old Distrobox container \"melonds-remote-host\" (renamed to \"${container_name}\") ..."
    distrobox rm "melonds-remote-host" --force 2>/dev/null || true
fi

already_central=0
if [[ "${host_root}" == "${central_install_dir}" ]]; then
    # Already running from the central directory itself -- e.g.
    # launch-host.sh re-invoking this script on every Steam-shortcut
    # launch, or a user double-clicking this exact copy a second time.
    # Re-copying the directory into itself on every single launch would
    # just waste time (and disk churn copying the melonDS binary) for no
    # benefit, so skip straight to the container/package step below --
    # there's nothing new to stage or swap in.
    already_central=1
    echo "Already installed at ${central_install_dir} -- skipping the re-copy step."
else
    echo "Staging host files at ${staging_dir} ..."
    rm -rf "${staging_dir}"
    mkdir -p "${staging_dir}"
    cp -a "${host_root}/." "${staging_dir}/"

    # So check-for-updates.sh keeps working via dualdeck-host.sh's
    # "../check-for-updates.sh" reference even when a copy of that menu
    # script is later run from inside the central directory itself
    # (e.g. after the original downloaded archive has been deleted) --
    # these two live one level up from host_root, as siblings of
    # install/, so they survive the install/install.previous swap below
    # untouched. Best-effort: a missing source (e.g. this got invoked
    # some other way) shouldn't fail the whole install over a
    # convenience file.
    cp "$(dirname "${host_root}")/check-for-updates.sh" "$(dirname "${central_install_dir}")/check-for-updates.sh" 2>/dev/null || true
    cp "$(dirname "${host_root}")/VERSION" "$(dirname "${central_install_dir}")/VERSION" 2>/dev/null || true
fi

echo "Creating/reusing Distrobox container \"${container_name}\" (Fedora-based) ..."
distrobox create --name "${container_name}" --image fedora:latest --yes

# Real user report, 2026-08-03: "MelonDS is still broken, not hosting a
# server, toggle still missing from menu" -- running melonDS directly
# showed the actual cause plainly: "error while loading shared
# libraries: libturbojpeg.so.0: cannot open shared object file." melonDS
# runs its own in-process NetServer (its patch vendors a full copy of
# net_server.cpp/h, unlike Azahar/Cemu, which delegate video encoding to
# the separate dualdeck-host-service process -- already fixed to bundle
# this same library, see that binary's own "internal/lib" comments
# elsewhere in this file), so melonDS's own binary links libjpeg-turbo's
# TurboJPEG API directly and needs libturbojpeg.so.0 itself at runtime.
# turbojpeg-devel (matching every other package in this list's -devel
# convention, and the same Fedora package name already verified correct
# elsewhere in this file's own ensure_packages() calls) was simply never
# in this container's package list at all -- not a devel-vs-runtime
# naming mismatch like some of the others here, a plain omission. No
# server ever starting and no window (so no menu, no checkbox) is
# exactly what a binary that can't even pass the dynamic linker before
# main() produces.
echo "Installing runtime libraries inside the container ..."
distrobox enter "${container_name}" -- sudo dnf install -y \
    libcurl-devel libpcap-devel SDL2-devel libarchive-devel enet-devel libzstd-devel faad2-devel \
    qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtmultimedia-devel qt6-qtsvg-devel \
    libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXfixes-devel libXi-devel libXScrnSaver-devel \
    wayland-devel libxkbcommon-devel libdrm-devel mesa-libgbm-devel libdecor-devel turbojpeg-devel

# Firewalld runs at the host OS level, not per-container -- opening
# these ports here (not just in install-steam-shortcut.sh, which
# already calls this script as a sub-step on immutable systems) also
# covers anyone running this script standalone. Best-effort, never
# fatal, and idempotent if install-steam-shortcut.sh's own call already
# did this a moment ago -- see scripts/lib/host_firewall.sh.
ensure_host_firewall_ports || true

# Only reached if everything above succeeded (set -e) -- safe to swap in
# the new install now (nothing to swap if already_central -- the
# directory already IS the install). Keeps just one backup generation,
# not unbounded.
if [[ "${already_central}" -eq 0 ]]; then
    echo "Activating the new install ..."
    rm -rf "${previous_dir}"
    if [[ -d "${central_install_dir}" ]]; then
        mv "${central_install_dir}" "${previous_dir}"
    fi
    mv "${staging_dir}" "${central_install_dir}"
fi

if [[ "${install_only}" -eq 1 ]]; then
    echo "Install complete (not launching melonDS -- run this script again without" \
         "--install-only, or use the Steam shortcut, to launch it)."
    exit 0
fi

echo "Launching the host inside the container ..."
# See run-host.sh's identical MELONDS_REMOTE_VERSION comment -- read from
# the same central-directory sibling copy of VERSION staged above.
host_app_version="$(cat "$(dirname "${central_install_dir}")/VERSION" 2>/dev/null || true)"

# Persistent-Host-Control-daemon handoff, mirroring run-host.sh's own
# probe. Real user report, 2026-08-04: with the daemon running, picking
# DS/melonDS on Bazzite produced "remote server failed to start -- a
# port it needs is already in use" and the session never switched out
# of Host Control mode into Emulation.
#
# Root cause was that this handoff lived *only* in run-host.sh, which
# an immutable system never reaches for melonDS: dualdeck-host.sh's
# "ds" choice execs launch-host.sh, which routes straight here on
# rpm-ostree hosts and launched melonDS with nothing but
# MELONDS_REMOTE_ENABLE=1. melonDS therefore always tried to stand up
# its own in-process remote server, on ports the already-running daemon
# was holding -- so the one configuration where the daemon is most
# useful was the one configuration where melonDS could not coexist with
# it.
#
# The socket path is resolved out here and passed in explicitly rather
# than letting default_adapter_socket_path() re-evaluate inside the
# container: distrobox does forward XDG_RUNTIME_DIR, but pinning the
# value the probe actually succeeded against removes any chance of the
# two disagreeing.
#
# Probe-only, never spawn -- same reasoning as run-host.sh: with no
# daemon running this leaves melonDS's proven in-process default
# completely untouched, so this can only add the better path, never
# take away the working one.
# shellcheck source=scripts/lib/adapter_socket_probe.sh
source ./adapter_socket_probe.sh
melonds_adapter_env=()
default_socket="$(default_adapter_socket_path)"
if is_adapter_socket_live "${default_socket}"; then
    echo "DualDeck: found a running Host Control daemon -- melonDS will connect to" >&2
    echo "it out-of-process instead of running its own in-process remote server." >&2
    melonds_adapter_env=(MELONDS_REMOTE_OUT_OF_PROCESS=1
                         "MELONDS_REMOTE_ADAPTER_SOCKET=${default_socket}")
fi

# Same default-fullscreen fix as run-host.sh (see that script's own
# comment on the real user report this addresses) -- this is the
# separate launch path the main Steam Big Picture shortcut takes on
# immutable (rpm-ostree) hosts, so it needs it too.
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

exec distrobox enter "${container_name}" -- env MELONDS_REMOTE_ENABLE=1 \
    "MELONDS_REMOTE_VERSION=${host_app_version}" "${melonds_adapter_env[@]}" \
    "${central_install_dir}/melonDS" ${melonds_args[@]+"${melonds_args[@]}"}
