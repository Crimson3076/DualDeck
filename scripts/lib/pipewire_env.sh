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
#
# Real user report, 2026-08-03 (Bazzite, round 2): the fix above got past
# pw_loop_new() -- SPA_PLUGIN_DIR now resolves correctly -- but
# pw_context_new() then failed: "could not load mandatory module
# libpipewire-module-protocol-native: No such file or directory". Same
# root cause, different PipeWire env var (PIPEWIRE_MODULE_DIR), but a
# real, separate bug in this file's own original fix: the original
# "only detect if the var isn't already set" guard is backwards from how
# PipeWire itself behaves. Confirmed directly from PipeWire's own source
# (pipewire.c/impl-module.c): PipeWire only falls back to its own correct
# compiled-in default when getenv() returns NULL -- if the variable is
# set to anything else, including a wrong path, PipeWire trusts it and
# never falls back. If Bazzite's own ambient environment (systemd
# --user's imported environment, /etc/environment, or similar) already
# exported PIPEWIRE_MODULE_DIR to something that doesn't actually contain
# the module, this file's own original "skip if already set" logic would
# silently never correct it -- exactly matching "SPA_PLUGIN_DIR got
# fixed, PIPEWIRE_MODULE_DIR didn't" despite identical detection logic
# for both. Rewritten below to validate the *real target file*, not just
# "is the directory non-empty" or "is the variable merely set," and to
# override an already-set-but-invalid value rather than trusting it.

# _pw_dir_has_file <dir> <relative-file>
#
# The actual load-bearing check: does the file PipeWire will dlopen()
# really exist under this directory, not just "the directory exists" or
# "something is non-empty there."
_pw_dir_has_file() {
    local dir="$1" rel="$2"
    [[ -n "${dir}" && -f "${dir}/${rel}" ]]
}

# _pw_pkgconfig_dir <pkgconfig-package> <pkgconfig-variable>
#
# Most authoritative source when available: asks the host's own real
# installed PipeWire directly, via the exact variable its own meson build
# generated into its own .pc file, rather than guessing a distro
# convention. Absence of pkg-config itself, or of the relevant -devel
# package's .pc file, is expected and non-fatal on a minimal/atomic image
# like Bazzite -- just falls through to the candidate-path scan below.
_pw_pkgconfig_dir() {
    local pkg="$1" var="$2"
    command -v pkg-config >/dev/null 2>&1 || return 1
    pkg-config --exists "${pkg}" 2>/dev/null || return 1
    pkg-config --variable="${var}" "${pkg}" 2>/dev/null
}

# _pw_scan_candidates <verify-rel-file> <candidate-dir>...
#
# Fallback for hosts without pkg-config (or without PipeWire's -devel
# package installed) -- the same per-distro path guesses as before,
# validated against the real target file rather than a bare directory
# existence check.
_pw_scan_candidates() {
    local rel="$1"
    shift
    local candidate
    for candidate in "$@"; do
        if _pw_dir_has_file "${candidate}" "${rel}"; then
            printf '%s' "${candidate}"
            return 0
        fi
    done
    return 1
}

# _pw_resolve_dir <ENV_VAR_NAME> <pkgconfig-pkg> <pkgconfig-var> <verify-rel-file> <candidate-dir>...
#
# Three-tier resolution for one env var: trust-but-verify an existing
# value, then pkg-config, then the hardcoded candidate scan. Exports the
# resolved directory, or unsets the variable entirely (never exports an
# empty string -- see this file's own header comment for why that would
# be strictly worse than leaving it unset).
_pw_resolve_dir() {
    local var_name="$1" pkg="$2" pkgvar="$3" verify_rel="$4"
    shift 4
    local current="${!var_name:-}"
    local pc_dir resolved=""

    if [[ -n "${current}" ]]; then
        if _pw_dir_has_file "${current}" "${verify_rel}"; then
            return 0 # valid override already in place -- leave it alone
        fi
        echo "pipewire_env.sh: \${${var_name}}='${current}' does not contain" \
             "${verify_rel} -- ignoring it and re-detecting" >&2
    fi

    pc_dir="$(_pw_pkgconfig_dir "${pkg}" "${pkgvar}")"
    if _pw_dir_has_file "${pc_dir}" "${verify_rel}"; then
        resolved="${pc_dir}"
        echo "pipewire_env.sh: resolved ${var_name}='${resolved}' via pkg-config (${pkg})" >&2
    else
        resolved="$(_pw_scan_candidates "${verify_rel}" "$@")" || resolved=""
        if [[ -n "${resolved}" ]]; then
            echo "pipewire_env.sh: resolved ${var_name}='${resolved}' via candidate-path scan" >&2
        fi
    fi

    if [[ -n "${resolved}" ]]; then
        export "${var_name}=${resolved}"
    elif [[ -n "${current}" ]]; then
        unset "${var_name}"
        echo "pipewire_env.sh: no valid ${var_name} found -- unset it so PipeWire falls" \
             "back to its own built-in default" >&2
    else
        echo "pipewire_env.sh: no valid ${var_name} found -- leaving it unset so PipeWire" \
             "falls back to its own built-in default" >&2
    fi
}

# find_and_export_pipewire_dirs
#
# Best-effort: if no valid path is found for a given variable (neither an
# existing override, pkg-config, nor the candidate scan), that variable is
# left unset -- WaylandScreenCapture already degrades gracefully (falls
# back to X11, then to mirroring simply staying disabled) when PipeWire
# itself can't be reached at all, so there's nothing meaningfully worse to
# fail into here.
find_and_export_pipewire_dirs() {
    _pw_resolve_dir SPA_PLUGIN_DIR libspa-0.2 plugindir \
        "support/libspa-support.so" \
        /usr/lib64/spa-0.2 /usr/lib/x86_64-linux-gnu/spa-0.2 /usr/lib/spa-0.2

    _pw_resolve_dir PIPEWIRE_MODULE_DIR libpipewire-0.3 moduledir \
        "libpipewire-module-protocol-native.so" \
        /usr/lib64/pipewire-0.3 /usr/lib/x86_64-linux-gnu/pipewire-0.3 /usr/lib/pipewire-0.3
}
