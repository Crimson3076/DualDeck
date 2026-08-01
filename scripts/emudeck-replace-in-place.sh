#!/usr/bin/env bash
# Builds DualDeck's patched melonDS/Azahar/Cemu and installs each one
# directly at the exact path EmuDeck's own launcher scripts/Steam shortcuts
# already point at (~/Applications/*.AppImage), backing up the original
# first -- so an existing EmuDeck install and its Steam shortcuts keep
# working completely unedited, just running DualDeck's remote-streaming-
# capable binary instead of stock. See docs/known-limitations.md's Phase A
# entry for the full design rationale, verification status, and known
# blind spots (in particular: EmuDeck's exact AppImage naming/glob
# convention has not been confirmed against a real EmuDeck install from
# inside this repo -- see scripts/lib/emudeck_paths.sh's own header).
#
# This is a source-build tool (like scripts/build-release.sh and
# scripts/patch-existing-emulator.sh) -- it clones and compiles from
# source, it does not just drop in a prebuilt binary. Runnable either
# from a checkout of this repo (as build-release.sh itself uses it), or
# from the packaged copy build-release.sh bundles into every release
# archive at host/emudeck-integration/ (see that packaging step's own
# comment for why the copy works unmodified in both locations). Either
# way, run it on the same machine EmuDeck is installed on.
#
# Usage:
#   ./scripts/emudeck-replace-in-place.sh [--emulator melonds|azahar|cemu]... [--yes] [--dry-run]
#
# With no --emulator given, every emulator EmuDeck has installed (detected
# via scripts/lib/emudeck_paths.sh) is replaced. Requires interactive
# confirmation unless --yes is given, since this overwrites files EmuDeck
# itself manages -- mirroring scripts/lib/steam_shortcut.py's "ask before
# touching something the user didn't directly point at" posture.
set -euo pipefail

# Saved before anything (the arg-parsing loop below) consumes "$@" --
# needed verbatim if this re-execs itself inside a Distrobox build
# container on an immutable system (see run_in_distrobox_build_container
# further down).
original_args=("$@")

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work_dir="${EMUDECK_REPLACE_WORKDIR:-$(mktemp -d)}"
# Real user report: a build failure deep inside a dependency (e.g.
# Cemu's vcpkg install) points at a detailed log file under work_dir
# (e.g. .../buildtrees/openssl/config-x64-linux-dbg-err.log) that
# explains the *actual* underlying error -- but work_dir was always
# deleted unconditionally on exit, including on failure, so by the time
# anyone could go look, it (and the one piece of information that would
# actually explain the failure) was already gone. Preserve it on
# failure so a follow-up "what does the real log say" is answerable;
# still cleaned up on success, matching the original behavior exactly
# for the common case.
trap '
    status=$?
    if [[ "${status}" -ne 0 ]]; then
        echo "== Build failed -- work directory preserved for debugging: ${work_dir} ==" >&2
    else
        rm -rf "${work_dir}"
    fi
' EXIT

# shellcheck source=scripts/lib/pinned_commits.sh
source "${repo_root}/scripts/lib/pinned_commits.sh"
# shellcheck source=scripts/lib/ensure-packages.sh
source "${repo_root}/scripts/lib/ensure-packages.sh"
# shellcheck source=scripts/lib/build_emulator.sh
source "${repo_root}/scripts/lib/build_emulator.sh"
# shellcheck source=scripts/lib/build_cache.sh
source "${repo_root}/scripts/lib/build_cache.sh"
# shellcheck source=scripts/lib/emudeck_paths.sh
source "${repo_root}/scripts/lib/emudeck_paths.sh"
# shellcheck source=scripts/lib/appimage_pack.sh
source "${repo_root}/scripts/lib/appimage_pack.sh"

manifest_py="${repo_root}/scripts/lib/appimage_manifest.py"

# Distinct from install-host-distrobox.sh's "dualdeck-host" container:
# that one is runtime-libraries-only, meant to launch an already-
# compiled binary, and is a long-lived thing a user launches from
# repeatedly. This tool needs a full build toolchain (cmake, ninja,
# gcc-c++, the whole Qt6/X11/Wayland -devel set -- see the
# ensure_packages "build" call below) instead, a completely different,
# much heavier footprint that a "just launch melonDS" user has no
# reason to carry, and vice versa. Treated as disposable/cache-like
# (see run_in_distrobox_build_container's own comment) rather than
# something with a paired uninstaller -- reclaim its disk space any
# time with `distrobox rm dualdeck-emudeck-build --force`.
emudeck_build_container_name="dualdeck-emudeck-build"

# run_in_distrobox_build_container
#
# Called once, only when is_immutable_system() is true and dry_run=0
# (see the dependency-check block below). Creates/reuses a dedicated
# Fedora Distrobox build container and re-execs this entire script
# inside it with the original arguments, guarded by
# DUALDECK_INSIDE_EMUDECK_BUILD_CONTAINER=1 so the re-exec'd copy takes
# the normal (non-immutable) path instead of looping -- belt-and-
# suspenders on top of is_immutable_system() itself already being false
# inside a plain `fedora:latest` container.
#
# A full re-exec of the whole script, not just the build steps: $HOME
# is bind-mounted into the container at the same absolute path by
# Distrobox by default, so ~/.cache/dualdeck/emudeck-builds (the
# persistent build cache), ~/.cache/dualdeck/appimagetool-*.AppImage,
# ~/Applications (EmuDeck's own install dir this tool detects against),
# and ~/.config/dualdeck/emudeck-replace.log all resolve identically
# inside and outside the container with no path translation needed --
# so the exact same detection/confirm/build/package/install code that
# already works on a regular Linux host also works correctly,
# unmodified, once re-run in here. work_dir (this script's own scratch
# clone/build directory, under /tmp by default) is deliberately NOT
# expected to be shared -- nothing outside the container process ever
# needs to read it, so it's removed here rather than left as an
# orphaned empty directory on the host (exec replaces this process
# image, so the EXIT trap that would normally clean it up never runs).
#
# Real user report: launched via a Steam shortcut (this tool's own
# intended entry point), the surrounding shell has LD_PRELOAD set to
# Steam's overlay-injection libraries (gameoverlayrenderer.so,
# libextest.so). distrobox enter forwards the caller's environment into
# the container by default, and unlike on the host -- where the 64-bit
# overlay lib loads fine and the 32-bit one is silently skipped
# (harmless "wrong ELF class" warnings) -- inside a freshly-created,
# not-yet-provisioned fedora:latest container the 64-bit overlay lib's
# own dependency, libGL.so.1, doesn't exist yet (mesa isn't installed
# until the ensure_packages "build" call further down), so ld.so's
# preload fails hard. That took down `env` itself, the very first thing
# exec'd inside the container, before this script's own code ever ran
# (exit 127, "cannot open shared object file"). LD_LIBRARY_PATH is
# unset for the same reason -- Steam also points it at its own bundled
# runtime, which is just as capable of shadowing a library the
# container's package manager needs.
run_in_distrobox_build_container() {
    if ! command -v distrobox >/dev/null 2>&1; then
        echo "error: 'distrobox' not found. Bazzite ships it by default; on other" >&2
        echo "rpm-ostree systems, install it first -- see" >&2
        echo "https://github.com/89luca89/distrobox" >&2
        exit 1
    fi

    echo "== This looks like an rpm-ostree (immutable) system, e.g. Bazzite =="
    echo "== Building inside Distrobox container \"${emudeck_build_container_name}\" instead =="
    echo "Creating/reusing Distrobox container \"${emudeck_build_container_name}\" (Fedora-based) ..."
    distrobox create --name "${emudeck_build_container_name}" --image fedora:latest --yes

    local self_path="${repo_root}/scripts/$(basename "${BASH_SOURCE[0]}")"
    rm -rf "${work_dir}"
    unset LD_PRELOAD LD_LIBRARY_PATH
    exec distrobox enter "${emudeck_build_container_name}" -- \
        env DUALDECK_INSIDE_EMUDECK_BUILD_CONTAINER=1 \
        "${self_path}" "${original_args[@]}"
}

# Prefers a VERSION file (present when this is run from a packaged
# release's host/emudeck-integration/ bundle, which has no .git
# directory at all) over `git describe`, so this works both from a
# real source checkout (build-release.sh's own use case) and from an
# extracted release archive (a packaged, downloadable copy of this same
# tool -- see build-release.sh's "EmuDeck integration bundle" packaging
# step). Never hard-fails even if neither is available.
if [[ -f "${repo_root}/VERSION" ]]; then
    dualdeck_version="$(cat "${repo_root}/VERSION")"
else
    dualdeck_version="$(cd "${repo_root}" && git describe --tags --always --dirty 2>/dev/null || \
        git rev-parse --short HEAD 2>/dev/null || echo "unknown")"
fi

log_dir="${HOME}/.config/dualdeck"
log_file="${log_dir}/emudeck-replace.log"
mkdir -p "${log_dir}"
log() {
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") $*" >> "${log_file}"
}
trap 'log "FAILED at line ${LINENO}: ${BASH_COMMAND}"' ERR

# ---- argument parsing ----
emulators=()
dry_run=0
assume_yes=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --emulator)
            emulators+=("$2")
            shift 2
            ;;
        --dry-run) dry_run=1; shift ;;
        --yes) assume_yes=1; shift ;;
        -h|--help)
            sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
            exit 0
            ;;
        *)
            echo "error: unknown argument: $1" >&2
            exit 1
            ;;
    esac
done
if [[ "${#emulators[@]}" -eq 0 ]]; then
    emulators=(melonds azahar cemu)
fi
for e in "${emulators[@]}"; do
    case "${e}" in
        melonds|azahar|cemu) ;;
        *) echo "error: --emulator must be one of: melonds azahar cemu (got: ${e})" >&2; exit 1 ;;
    esac
done

# Skipped entirely in --dry-run mode -- installing build dependencies
# (sudo apt/dnf/pacman install) is a real side effect, and --dry-run
# should have none. Real detection/plan reporting below doesn't need any
# of these packages, only the actual build step does.
cmake_launcher_args=()
if [[ "${dry_run}" -ne 1 ]]; then
    if is_immutable_system && [[ "${DUALDECK_INSIDE_EMUDECK_BUILD_CONTAINER:-0}" != "1" ]]; then
        run_in_distrobox_build_container   # execs; never returns on success
    fi

    echo "== Checking build dependencies =="
    ensure_packages "build" \
        "cmake extra-cmake-modules ninja-build build-essential git python3 file libcurl4-gnutls-dev libpcap0.8-dev libsdl2-dev libarchive-dev libenet-dev libzstd-dev libfaad-dev qt6-base-dev qt6-base-private-dev qt6-multimedia-dev qt6-svg-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libwayland-dev libxkbcommon-dev libdrm-dev libgbm-dev libdecor-0-dev libturbojpeg0-dev" \
        "cmake extra-cmake-modules ninja-build gcc-c++ git python3 curl file libcurl-devel libpcap-devel SDL2-devel libarchive-devel enet-devel libzstd-devel faad2-devel qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtmultimedia-devel qt6-qtsvg-devel libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXfixes-devel libXi-devel libXScrnSaver-devel wayland-devel libxkbcommon-devel libdrm-devel mesa-libgbm-devel libdecor-devel turbojpeg-devel" \
        "cmake extra-cmake-modules ninja base-devel git python curl file libpcap sdl2 libarchive enet zstd faad2 qt6-base qt6-multimedia qt6-svg libx11 libxext libxrandr libxcursor libxfixes libxi libxss wayland libxkbcommon libdrm mesa libdecor libjpeg-turbo"
    if [[ " ${emulators[*]} " == *" azahar "* ]]; then
        ensure_packages "azahar build" \
            "libvulkan-dev libboost-dev libboost-iostreams-dev libboost-thread-dev libpulse-dev libasound2-dev" \
            "vulkan-loader-devel vulkan-headers boost-devel pulseaudio-libs-devel alsa-lib-devel" \
            "vulkan-headers vulkan-icd-loader boost pulseaudio alsa-lib"
    fi
    if [[ " ${emulators[*]} " == *" cemu "* ]]; then
        # linux-libc-dev/kernel-headers/linux-headers: real Bazzite hardware
        # test -- Cemu's vcpkg-built openssl dependency needs Linux kernel
        # headers (its ./Configure script probes for them, e.g. AF_ALG
        # socket support), which a minimal Fedora Distrobox image doesn't
        # ship by default (unlike a typical Debian/Ubuntu install, which
        # usually has linux-libc-dev pulled in transitively already) --
        # openssl's own vcpkg portfile prints a hint about this ("openssl
        # requires Linux kernel headers from the system package manager")
        # right before the actual configure command fails, easy to miss
        # among vcpkg's otherwise-routine warnings.
        ensure_packages "cemu build" \
            "freeglut3-dev libbluetooth-dev libgcrypt20-dev libglm-dev libgtk-3-dev libpulse-dev libsecret-1-dev libsystemd-dev libtool nasm libusb-1.0-0-dev linux-libc-dev" \
            "freeglut-devel bluez-libs-devel libgcrypt-devel glm-devel gtk3-devel pulseaudio-libs-devel libsecret-devel systemd-devel libtool nasm libusb1-devel perl-IPC-Cmd kernel-headers" \
            "freeglut bluez-libs libgcrypt glm gtk3 libpulse libsecret systemd libtool nasm libusb linux-headers"
    fi

    if command -v sccache >/dev/null 2>&1; then
        cmake_launcher_args=(-DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache)
    fi
fi

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
# scripts/lib/appimage_pack.sh -- this binary was built inside a
# Distrobox container that may have newer glibc/Qt/etc. than whatever
# host actually launches this AppImage, so its real dependencies are
# bundled alongside it rather than assumed present on the host.
export LD_LIBRARY_PATH="\${HERE}/usr/lib\${LD_LIBRARY_PATH:+:\${LD_LIBRARY_PATH}}"
export MELONDS_REMOTE_ENABLE=1
export MELONDS_REMOTE_VERSION="${dualdeck_version_arg}"
exec "\${HERE}/usr/bin/melonDS" "\$@"
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
# bundled dualdeck-host-service below were built inside a Distrobox
# container that may have newer glibc/Qt/etc. than whatever host
# actually launches this AppImage, so their real dependencies are
# bundled alongside them rather than assumed present on the host.
export LD_LIBRARY_PATH="${HERE}/usr/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

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
    "${HERE}/usr/bin/dualdeck-host-service" --adapter-ipc --adapter-socket "${adapter_socket}" \
        --state-dir "${HOME}/.config/melonds-remote" &
    host_service_pid=$!
    trap 'kill "${host_service_pid}" 2>/dev/null || true' EXIT
    sleep 0.5
fi

export __PREFIX___REMOTE_ENABLE=1
export __PREFIX___REMOTE_ADAPTER_SOCKET="${adapter_socket}"
export QT_QPA_PLATFORMTHEME=""
exec "${HERE}/usr/bin/__REALBIN__" "$@"
EOF
    sed -i \
        -e "s/__PREFIX__/${prefix}/g" \
        -e "s/__REALBIN__/${real_bin}/g" \
        -e "s/__PRIVATE_SOCKET_NAME__/${private_socket_name}/g" \
        "${output_path}"
    chmod +x "${output_path}"
}

# Built once, lazily, only if at least one out-of-process emulator
# (Azahar/Cemu) is actually being installed this run.
host_service_bin=""
ensure_host_service_binary() {
    if [[ -n "${host_service_bin}" ]]; then
        return
    fi
    echo "== Building dualdeck-host-service (bundled into Azahar/Cemu AppImages) =="
    local repo_build="${work_dir}/repo-build"
    cmake -S "${repo_root}" -B "${repo_build}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DDUALDECK_BUILD_CLIENT=OFF \
        -DDUALDECK_BUILD_HOST=ON -DDUALDECK_BUILD_TESTS=OFF \
        "${cmake_launcher_args[@]}"
    cmake --build "${repo_build}" --target dualdeck-host-service -j"$(nproc)"
    host_service_bin="${repo_build}/host/remote-server/dualdeck-host-service"
}

# ---- per-emulator orchestration ----
replace_in_place_one() {
    local emulator="$1"
    local appimage_path=""

    case "${emulator}" in
        melonds)
            appimage_path="$(find_emudeck_melonds_appimage)" || {
                echo "== melonds: no EmuDeck install found under $(emudeck_applications_dir), skipping =="
                return 0
            }
            ;;
        azahar)
            appimage_path="$(find_emudeck_azahar_appimage)" || {
                echo "== azahar: no EmuDeck install found under $(emudeck_applications_dir), skipping =="
                return 0
            }
            ;;
        cemu)
            if ! appimage_path="$(find_emudeck_cemu_appimage)"; then
                local flatpak_id
                if flatpak_id="$(find_emudeck_cemu_flatpak_id)"; then
                    echo "== cemu: found a Flatpak install (${flatpak_id}) -- replace-in-place doesn't" >&2
                    echo "   support Flatpak yet (a different, sandboxed install shape -- see" >&2
                    echo "   docs/known-limitations.md's Phase A entry). Skipping cemu. ==" >&2
                else
                    echo "== cemu: no EmuDeck install found under $(emudeck_applications_dir), skipping =="
                fi
                return 0
            fi
            ;;
    esac

    echo "== ${emulator}: found EmuDeck install at ${appimage_path} =="

    local backup_path="${appimage_path}.dualdeck-original"
    local drift_status
    drift_status="$(python3 "${manifest_py}" check "${appimage_path}")"

    local original_sha256
    case "${drift_status}" in
        no_manifest)
            echo "== ${emulator}: first-time install -- backing up the original to ${backup_path} =="
            original_sha256="$(sha256sum "${appimage_path}" | cut -d' ' -f1)"
            ;;
        matches_patched|matches_original|drifted)
            if [[ ! -f "${backup_path}" ]]; then
                echo "error: ${emulator}: a DualDeck manifest exists at ${appimage_path}.dualdeck.json but" >&2
                echo "the backup at ${backup_path} is missing -- refusing to proceed without a" >&2
                echo "known-good original to preserve. Restore or remove the manifest manually first." >&2
                log "${emulator}: refused -- manifest present, backup missing"
                return 1
            fi
            original_sha256="$(python3 -c "
import json
print(json.load(open('${appimage_path}.dualdeck.json'))['original_sha256'])
")"
            if [[ "${drift_status}" == "drifted" ]]; then
                echo "== ${emulator}: installed file doesn't match what DualDeck installed last time" >&2
                echo "   (EmuDeck's own updater likely replaced it) -- re-patching over it now. ==" >&2
                log "${emulator}: drift detected, re-patching"
            fi
            ;;
    esac

    if [[ "${dry_run}" -eq 1 ]]; then
        echo "== ${emulator}: --dry-run, stopping here (would build + install over ${appimage_path}) =="
        return 0
    fi

    if [[ "${assume_yes}" -ne 1 ]]; then
        read -r -p "Replace ${appimage_path} with DualDeck's patched build? [y/N] " reply
        case "${reply}" in
            [yY]|[yY][eE][sS]) ;;
            *) echo "== ${emulator}: skipped (not confirmed) =="; return 0 ;;
        esac
    fi

    local patched_bin app_name real_bin_name apprun_path extra_bins=""
    case "${emulator}" in
        melonds)
            local melonds_patch_file="${repo_root}/host/melonds-patches/0001-remote-server-integration.patch"
            if ! try_cached_build patched_bin melonds "${melonds_patch_file}" "${MELONDS_COMMIT}"; then
                build_melonds patched_bin "${work_dir}" "${repo_root}" "${MELONDS_COMMIT}"
                save_build_cache melonds "${melonds_patch_file}" "${MELONDS_COMMIT}" "${patched_bin}"
            fi
            app_name="melonDS"
            real_bin_name="melonDS"
            apprun_path="${work_dir}/AppRun-melonds"
            generate_apprun_melonds "${apprun_path}" "${dualdeck_version}"
            ;;
        azahar)
            local azahar_patch_file="${repo_root}/host/azahar-patches/0001-remote-server-integration.patch"
            if ! try_cached_build patched_bin azahar "${azahar_patch_file}" "${AZAHAR_COMMIT}"; then
                build_azahar patched_bin "${work_dir}" "${repo_root}" "${AZAHAR_COMMIT}"
                save_build_cache azahar "${azahar_patch_file}" "${AZAHAR_COMMIT}" "${patched_bin}"
            fi
            ensure_host_service_binary
            app_name="azahar"
            real_bin_name="azahar"
            apprun_path="${work_dir}/AppRun-azahar"
            generate_apprun_out_of_process AZAHAR azahar azahar-apprun-adapter.sock "${apprun_path}"
            extra_bins="${host_service_bin}"
            ;;
        cemu)
            local cemu_patch_file="${repo_root}/host/cemu-patches/0001-remote-server-integration.patch"
            if ! try_cached_build patched_bin cemu "${cemu_patch_file}" "${CEMU_COMMIT}"; then
                build_cemu patched_bin "${work_dir}" "${repo_root}" "${CEMU_COMMIT}"
                save_build_cache cemu "${cemu_patch_file}" "${CEMU_COMMIT}" "${patched_bin}"
            fi
            ensure_host_service_binary
            app_name="cemu"
            real_bin_name="cemu"
            apprun_path="${work_dir}/AppRun-cemu"
            generate_apprun_out_of_process CEMU cemu cemu-apprun-adapter.sock "${apprun_path}"
            extra_bins="${host_service_bin}"
            ;;
    esac

    # Staged in its own directory under a filename matching real_bin_name
    # exactly (the basename, not just a prefix) -- pack_appimage() names
    # the file inside AppDir/usr/bin/ after basename(binary_path), and
    # AppRun execs "usr/bin/${real_bin_name}" literally, so this rename
    # has to happen even though build_melonds()/build_azahar()'s own
    # output already happens to be named "melonDS"/"azahar" (Cemu's build
    # output is literally named Cemu_release, not "cemu", so this is
    # load-bearing for at least that one case).
    local staged_dir="${work_dir}/staged-${emulator}"
    mkdir -p "${staged_dir}"
    local staged_bin="${staged_dir}/${real_bin_name}"
    cp "${patched_bin}" "${staged_bin}"

    local new_appimage="${appimage_path}.new"
    pack_appimage "${staged_bin}" "${new_appimage}" "${app_name}" "${apprun_path}" "${extra_bins}"

    if [[ "${drift_status}" == "no_manifest" ]]; then
        cp "${appimage_path}" "${backup_path}"
    fi
    mv "${new_appimage}" "${appimage_path}"
    chmod +x "${appimage_path}"

    python3 "${manifest_py}" write "${appimage_path}" \
        --dualdeck-version "${dualdeck_version}" \
        --original-sha256 "${original_sha256}"

    echo "== ${emulator}: installed DualDeck's patched build at ${appimage_path} =="
    echo "   (original backed up at ${backup_path})"
    log "${emulator}: installed dualdeck_version=${dualdeck_version}"
}

for emulator in "${emulators[@]}"; do
    replace_in_place_one "${emulator}"
done

echo "== Done. Launch your existing EmuDeck/Steam shortcuts as usual -- no shortcut edits needed. =="
