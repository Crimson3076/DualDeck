#!/usr/bin/env bash
# AppRun script generators for the patched melonDS/Azahar/Cemu AppImages,
# meant to be `source`d (not executed directly). Used by build-release.sh
# to produce the prebuilt, patched AppImages published as release assets
# (see docs/known-limitations.md's 2026-08-01 "prebuilt AppImages" entry
# for why these are built once in CI rather than compiled on every user's
# own machine by emudeck-replace-in-place.sh, which used to own these
# functions and now just downloads the result).

# ---- AppRun generation ----
# melonDS's remote server is enabled via its own persisted Qt Config
# checkbox (MelonDSRemote.Enable / the "Enable melonDS Remote" setting) or
# MELONDS_REMOTE_ENABLE, and runs its server IN-PROCESS by default -- no
# out-of-process Host Service is needed for it to work at all (see
# docs/known-limitations.md's "Config/UI toggle" entry). Every OTHER
# melonDS launch path in this codebase (run-host.sh, launch-custom-
# emulator.sh, the Distrobox exec in install-host-distrobox.sh) exports
# MELONDS_REMOTE_ENABLE=1 explicitly -- this one didn't, a real bug
# (real user report: melonDS launched via a patched EmuDeck AppImage
# never streamed at all, while Azahar/Cemu on the same machine worked
# fine) that meant the server only ever started here if the user had
# separately gone into melonDS's own Settings and manually checked
# "Enable melonDS Remote," which nobody does on a fresh EmuDeck install.
# Fixed by exporting the same env var every other path already does,
# so this works out of the box regardless of which frontend/launcher
# (EmuDeck's own shortcuts, SteamRomManager, ES-DE) invokes the patched
# AppImage -- the entire point of "replace in place." Making melonDS
# default to out-of-process instead is separate, larger Phase B work
# (a bigger change to melonDS's own patch), not this fix's scope -- see
# docs/known-limitations.md's Phase A entry.
generate_apprun_melonds() {
    local output_path="$1" dualdeck_version_arg="$2"
    cat > "${output_path}" <<EOF
#!/usr/bin/env bash
set -euo pipefail
HERE="\$(cd "\$(dirname "\$(readlink -f "\${0}")")" && pwd)"
# See bundle_library_dependencies()'s comment in
# scripts/lib/appimage_pack.sh -- this binary was built in a clean CI
# environment that may have newer Qt/etc. than whatever host actually
# launches this AppImage, so its real dependencies are bundled alongside
# it rather than assumed present on the host (glibc itself is
# deliberately excluded from that bundle -- see the same comment). Kept
# as a plain variable, not \`export\`ed, and passed only on the final
# \`exec\` line below rather than for the whole script -- nothing else
# runs between here and that exec in this particular AppRun, but this
# matches the out-of-process AppRun template's identical, load-bearing
# reason for doing the same (see generate_apprun_out_of_process()'s own
# comment): system utilities this script might ever call in the future
# must never inherit a LD_LIBRARY_PATH pointing at bundled libraries.
bundled_lib_path="\${HERE}/usr/lib"
export MELONDS_REMOTE_ENABLE=1
export MELONDS_REMOTE_VERSION="${dualdeck_version_arg}"
exec env LD_LIBRARY_PATH="\${bundled_lib_path}\${LD_LIBRARY_PATH:+:\${LD_LIBRARY_PATH}}" \\
    "\${HERE}/usr/bin/melonDS" "\$@"
EOF
    chmod +x "${output_path}"
}

# Azahar/Cemu have no in-process remote-server path at all (see
# host/azahar-patches/README.md, host/cemu-patches/README.md) -- they
# always need a running Host Service to connect to over adapter-ipc. This
# AppRun first tries the DEFAULT shared adapter socket
# (scripts/lib/../../adapter-sdk/ipc/src/socket_path.cpp's
# defaultAdapterSocketPath(), so a Phase B persistent daemon -- once it
# exists -- is found automatically with no re-patch/re-package needed
# here) before falling back to spawning a private, ephemeral Host Service
# on a private socket and killing it on exit, exactly like today's
# packaged run-host-azahar.sh/run-host-cemu.sh do.
generate_apprun_out_of_process() {
    local prefix="$1" real_bin="$2" private_socket_name="$3" output_path="$4"
    cat > "${output_path}" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$(readlink -f "${0}")")" && pwd)"
# See bundle_library_dependencies()'s comment in
# scripts/lib/appimage_pack.sh -- both the real emulator binary and the
# bundled dualdeck-host-service below were built in a clean CI
# environment that may have newer Qt/etc. than whatever host actually
# launches this AppImage, so their real dependencies are bundled
# alongside them rather than assumed present on the host (glibc itself
# is deliberately excluded from that bundle -- see the same comment for
# why bundling it broke ld.so's own internal ABI).
#
# Real crash this caused when this WAS exported for the whole script,
# 2026-08-01: `mkdir -p` a few lines below (a completely unrelated
# system tool, not one of DualDeck's own bundled binaries) inherited
# this LD_LIBRARY_PATH and tried to run against the then-bundled
# libc.so.6 instead of the system's own, crashing with `undefined
# symbol: __nptl_change_stack_perm, version GLIBC_PRIVATE` before Cemu
# was ever even reached. glibc is no longer bundled at all (the deeper
# fix, see appimage_pack.sh), but this is kept as a second, independent
# layer: NOT exporting LD_LIBRARY_PATH globally means that even if some
# *other* bundled library ever collides with a system tool's own
# dependency in the future, only the two commands below that actually
# need the bundled libraries (dualdeck-host-service, __REALBIN__) are
# affected -- every other command this script runs (the python3 socket
# probe, mkdir, rm) always uses the host's own, completely unmodified
# environment.
bundled_lib_path="${HERE}/usr/lib"

default_socket="${XDG_RUNTIME_DIR:-}/dualdeck/adapter.sock"
if [[ -z "${XDG_RUNTIME_DIR:-}" ]]; then
    default_socket="${HOME}/.cache/dualdeck/adapter.sock"
fi

adapter_socket=""
# python3 is assumed present -- matching this project's existing
# convention (steam_shortcut.py, appimage_manifest.py, smoke_test.py are
# all python3-implemented with no availability check either).
if [[ -S "${default_socket}" ]] && python3 -c '
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(1)
try:
    s.connect(sys.argv[1])
    sys.exit(0)
except OSError:
    sys.exit(1)
' "${default_socket}" 2>/dev/null; then
    echo "DualDeck: found a running host service at ${default_socket}, connecting to it" >&2
    adapter_socket="${default_socket}"
else
    echo "DualDeck: no persistent host service running yet -- starting a private one for this session" >&2
    run_dir="${HOME}/.config/dualdeck/run"
    mkdir -p "${run_dir}"
    adapter_socket="${run_dir}/__PRIVATE_SOCKET_NAME__"
    rm -f "${adapter_socket}"
    env LD_LIBRARY_PATH="${bundled_lib_path}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
        "${HERE}/usr/bin/dualdeck-host-service" --adapter-ipc --adapter-socket "${adapter_socket}" \
        --state-dir "${HOME}/.config/melonds-remote" &
    host_service_pid=$!
    trap 'kill "${host_service_pid}" 2>/dev/null || true' EXIT
    sleep 0.5
fi

export __PREFIX___REMOTE_ENABLE=1
export __PREFIX___REMOTE_ADAPTER_SOCKET="${adapter_socket}"
export QT_QPA_PLATFORMTHEME=""
# Real user report, 2026-08-01: "is Host Control always running in the
# background now?" -- yes, and this is why. `exec` REPLACES this whole
# bash process's image with __REALBIN__'s -- there is no more shell left
# afterward to ever run the `trap ... EXIT` above, so a private
# dualdeck-host-service spawned by the branch above was silently
# orphaned forever the instant __REALBIN__ actually launched, regardless
# of whether __REALBIN__ later exited cleanly, crashed, or was killed --
# confirmed directly (a two-line bash repro: a trap set before `exec
# /bin/echo ...` never fires). Every closed game left its own host-
# service running and discoverable, presenting HostControl mode (no
# adapter registered) to any client that came looking, which is exactly
# what looked like "Host Control always running" and (per
# host_control_adapter.h's getLatestFrame(), hard-coded to return false
# -- there is no host-desktop screen-capture code yet, see
# docs/known-limitations.md's Phase C2 entries) also explains why
# connecting to it got stuck: a real, accepted connection into a mode
# that structurally never sends a video frame.
#
# Fixed the same way build-release.sh's own run-host-azahar.sh/
# run-host-cemu.sh already do this (their own "Not exec'd" comments) --
# not execing here at all, running __REALBIN__ as a normal foreground
# command instead, so this shell (and its EXIT trap) is still alive to
# clean up the private host-service once __REALBIN__ actually exits,
# however it exits.
env LD_LIBRARY_PATH="${bundled_lib_path}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
    "${HERE}/usr/bin/__REALBIN__" "$@"
EOF
    sed -i \
        -e "s/__PREFIX__/${prefix}/g" \
        -e "s/__REALBIN__/${real_bin}/g" \
        -e "s/__PRIVATE_SOCKET_NAME__/${private_socket_name}/g" \
        "${output_path}"
    chmod +x "${output_path}"
}
