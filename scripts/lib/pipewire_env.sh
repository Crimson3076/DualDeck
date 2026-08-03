#!/usr/bin/env bash
# Meant to be `source`d (not executed directly) by any generated launcher
# script that might run dualdeck-host-service with Host Control screen
# mirroring enabled (host-control-daemon.sh, run-host.sh's Host-Control
# branch).
#
# Real user report, 2026-08-03 (Bazzite): even after WaylandScreenCapture's
# own format/buffer-negotiation bugs were fixed, screen mirroring still
# never worked -- "PIPEWIRE_DEBUG=3" showed the real cause directly:
#   [E] pw.loop [loop.c:67 pw_loop_new()] can't make support.system
#   handle: No such file or directory
# dualdeck-host-service is built once in CI (an Ubuntu runner) and
# dynamically links the CI machine's libpipewire -- but libpipewire's own
# SPA plugin search path (SPA_PLUGIN_DIR, where "support.system" and every
# other SPA module actually live as separate dlopen()'d .so files) is
# baked in at *PipeWire's own* build time, pointing at wherever Ubuntu's
# PipeWire package puts them. That path doesn't exist on Bazzite (Fedora-
# based, different filesystem layout entirely), so libpipewire can't find
# its own foundational support module and fails before ever reaching this
# project's own format/buffer negotiation code.
#
# Same root shape as find_qt6_plugins_dir()'s Qt-platform-plugin fix
# (scripts/lib/appimage_pack.sh) -- a dlopen()'d component with its own
# separate, non-ldd-visible search path -- but the fix here is
# deliberately different: SPA plugins/PipeWire modules are tightly
# version-coupled to the exact PipeWire *daemon* actually running on this
# host (the same reasoning bundle_library_dependencies() already gives
# for never bundling glibc itself -- these aren't safely portable between
# machines the way an ordinary linked library is), so this points
# SPA_PLUGIN_DIR/PIPEWIRE_MODULE_DIR at the *host's own* installed
# PipeWire, detected at actual launch time on the real machine, rather
# than bundling DualDeck's own copies into the release build.

# find_and_export_pipewire_dirs
#
# Best-effort: if neither SPA_PLUGIN_DIR's nor PIPEWIRE_MODULE_DIR's
# default candidate paths exist on this host, leaves both env vars
# unset -- WaylandScreenCapture already degrades gracefully (falls back
# to X11, then to mirroring simply staying disabled) when PipeWire itself
# can't be reached at all, so there's nothing meaningfully worse to fail
# into here. Skips entirely (does not override) if either var is already
# set in the environment, so an explicit user override always wins.
find_and_export_pipewire_dirs() {
    if [[ -z "${SPA_PLUGIN_DIR:-}" ]]; then
        local candidate
        for candidate in /usr/lib64/spa-0.2 /usr/lib/x86_64-linux-gnu/spa-0.2 /usr/lib/spa-0.2; do
            if [[ -d "${candidate}/support" ]]; then
                export SPA_PLUGIN_DIR="${candidate}"
                break
            fi
        done
    fi

    if [[ -z "${PIPEWIRE_MODULE_DIR:-}" ]]; then
        local candidate
        for candidate in /usr/lib64/pipewire-0.3 /usr/lib/x86_64-linux-gnu/pipewire-0.3 /usr/lib/pipewire-0.3; do
            if [[ -d "${candidate}" ]]; then
                export PIPEWIRE_MODULE_DIR="${candidate}"
                break
            fi
        done
    fi
}
