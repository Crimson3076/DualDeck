#!/usr/bin/env bash
# AppImage packaging, meant to be `source`d (not executed directly) by
# scripts/emudeck-replace-in-place.sh. Wraps `appimagetool` to build a
# single-file AppImage from an already-built ELF binary plus a caller-
# supplied AppRun script -- kept generic/emulator-agnostic on purpose: the
# emulator-specific "connect to the shared daemon socket if one's already
# listening, else spawn a private ephemeral dualdeck-host-service, then
# exec the real binary with the right env vars" logic belongs to the
# caller (it differs per emulator -- AZAHAR_REMOTE_* vs CEMU_REMOTE_* env
# var names), not to this generic packaging step.

# ensure_appimagetool
#
# Downloads and caches appimagetool itself (not distro-packaged on
# Debian/Fedora/Arch -- unlike this project's other build dependencies,
# which do go through ensure_packages()/apt-dnf-pacman, appimagetool is
# only ever distributed as its own AppImage release on GitHub, so this
# follows the same "download once, cache across runs" pattern
# build-release.sh already uses for SDL3, keyed on a pinned version rather
# than a package-manager name that doesn't reliably exist across distros).
# Prints the cached tool's path on success.
APPIMAGETOOL_VERSION="13"
ensure_appimagetool() {
    local cache_dir="${HOME}/.cache/dualdeck"
    local tool_path="${cache_dir}/appimagetool-x86_64.AppImage"

    if [[ ! -x "${tool_path}" ]]; then
        mkdir -p "${cache_dir}"
        echo "Downloading appimagetool (continuous build, tag ${APPIMAGETOOL_VERSION}) ..." >&2
        curl -fL --retry 3 -o "${tool_path}.tmp" \
            "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"
        chmod +x "${tool_path}.tmp"
        mv "${tool_path}.tmp" "${tool_path}"
    fi

    echo "${tool_path}"
}

# find_qt6_plugins_dir
#
# Locates Qt6's plugins directory (containing platforms/libqxcb.so etc.)
# on this build machine, so its "platforms" subdirectory can be bundled
# into a Qt6-based AppImage (melonDS, Azahar) via pack_appimage()'s
# extra_dirs. Real user report, 2026-08-01: neither melonDS nor Azahar
# could start at all on a real Bazzite/Fedora HTPC with no system Qt6 GUI
# stack installed ("Could not find the Qt platform plugin wayland/xcb in
# \"\"", then an abort) -- bundle_library_dependencies() only ever
# bundles ldd-reported *linked* dependencies, and Qt's platform plugins
# are dlopen()'d at runtime based on QT_PLUGIN_PATH, invisible to ldd, so
# they were never bundled at all. Prints the plugins directory (the
# parent of "platforms", not "platforms" itself) on success.
#
# Tries qtpaths6 first (Qt's own portable way to query this, present
# once qt6-base-dev-tools or equivalent is installed), falling back to
# a search of common distro install prefixes if that's not available.
find_qt6_plugins_dir() {
    if command -v qtpaths6 >/dev/null 2>&1; then
        local dir
        dir="$(qtpaths6 --query QT_INSTALL_PLUGINS 2>/dev/null || true)"
        if [[ -n "${dir}" && -d "${dir}/platforms" ]]; then
            echo "${dir}"
            return 0
        fi
    fi

    local candidate
    for candidate in /usr/lib/x86_64-linux-gnu/qt6/plugins /usr/lib/qt6/plugins /usr/lib64/qt6/plugins; do
        if [[ -d "${candidate}/platforms" ]]; then
            echo "${candidate}"
            return 0
        fi
    done
    return 1
}

# bundle_library_dependencies <binary_path> <dest_lib_dir>
#
# Real Bazzite hardware bug: this binary was compiled inside a Distrobox
# build container (a plain `fedora:latest` image, deliberately always
# current -- see run_in_distrobox_build_container's comment in
# emudeck-replace-in-place.sh), but the AppImage it ends up in is
# launched directly on the *host* system afterward (EmuDeck's own Steam
# shortcuts exec the .AppImage file as-is -- the entire point of
# "replace in place" is that nothing about how EmuDeck launches it
# changes). If the container's glibc/libstdc++/Qt6 etc. are newer than
# whatever the host actually has, the binary fails to even start
# there: `azahar: /lib64/libm.so.6: version 'GLIBC_2.43' not found`,
# same for libstdc++/libQt6Core, plus a flat `cannot open shared object
# file` for libturbojpeg (a build dependency inside the container, not
# necessarily installed on the bare host at all). A real AppImage is
# supposed to be self-contained precisely so this can't happen --
# pack_appimage() was copying just the raw binary and relying entirely
# on whatever the launching system happens to already have, which is
# not a safe assumption once the build and run environments genuinely
# differ. Fixed by actually bundling every `ldd`-reported dependency
# (ldd already resolves the full transitive closure, not just direct
# dependencies, so one pass is enough) into AppDir/usr/lib/, skipping
# only the two lines that aren't real bundleable files: the kernel-
# provided linux-vdso.so.1 (`ldd` prints an address for it, but there
# is no on-disk file to copy), and the dynamic linker/loader itself
# (`ld-linux-x86-64.so.2`, invoked by the kernel directly via the
# binary's PT_INTERP before any AppRun-set LD_LIBRARY_PATH could matter
# -- bundling it would need explicitly re-invoking it with
# --library-path, a bigger change not attempted here).
#
# Real user report, 2026-08-01, on the CI-built prebuilt AppImages this
# function's original fix was extended to cover: bundling glibc itself
# (libc.so.6 and its close relatives) is NOT safe the way every other
# library here is -- it used to be included on the theory that "bundle
# everything ldd reports, glibc included, since the observed problem was
# a too-*old* host glibc missing symbols the build environment had."
# That reasoning doesn't hold for CI-built AppImages: glibc's own ABI
# between ld.so (the dynamic linker/interpreter, always the HOST's copy
# -- PT_INTERP is a fixed absolute path, entirely unaffected by
# LD_LIBRARY_PATH) and libc.so.6 relies on private, version-pinned
# symbols (e.g. `GLIBC_PRIVATE`) that are only guaranteed to match
# *within one specific glibc build*, never across two independently
# built ones. A bundled libc.so.6 paired with the host's own (different)
# ld.so isn't "an older/newer libc," it's an ABI mismatch -- and it
# doesn't just break the emulator binary, it breaks *any* plain system
# utility the AppRun script itself invokes after exporting
# LD_LIBRARY_PATH (real crash: `mkdir -p` inside the out-of-process
# AppRun template died with `undefined symbol: __nptl_change_stack_perm,
# version GLIBC_PRIVATE` against the bundled libc.so.6, before Cemu was
# even reached). Fixed by excluding glibc's own component libraries from
# bundling outright -- standard practice for portable Linux apps
# (linuxdeploy and friends maintain the same blacklist for the same
# reason): these must always come from the host's own matched ld.so, not
# from LD_LIBRARY_PATH. This reintroduces a narrower version of the
# original too-old-host-glibc risk in theory, but CI's build environment
# (GitHub's Ubuntu runner) is realistically no newer than the rolling-
# release Fedora-based hosts (Bazzite, SteamOS) this project actually
# targets, unlike the old Distrobox-container scenario this function was
# first written for -- and a working host mkdir is a harder requirement
# than a hypothetical newer-glibc symbol.
#
# Real user report, 2026-08-03: Cemu, launched via a DualDeck-patched
# AppImage on real Bazzite hardware (a machine with a real, working
# discrete AMD GPU -- confirmed via its own log.txt, not a system
# without one), failed with "The following required Vulkan instance
# extensions are not supported: VK_KHR_surface, VK_KHR_wayland_surface"
# -- traced directly to Cemu's own unmodified upstream code (this
# project's patch never touches vkEnumerateInstanceExtensionProperties()
# or anything near it, confirmed by reading the actual diff), calling
# dlopen("libvulkan.so"/"libvulkan.so.1") with no path -- a bare-soname
# dlopen resolves through the *same* LD_LIBRARY_PATH-first search order
# as any NEEDED library. Cemu's own CMakeLists.txt links Wayland::Client
# directly (target_link_libraries(CemuCafe PUBLIC Wayland::Client)), so
# ldd's recursive closure -- and therefore bundle_library_dependencies()
# above -- can pull in Vulkan-loader-adjacent libraries from the CI
# runner (a headless build machine with no real GPU/ICD ever
# discoverable there) into the very same AppImage LD_LIBRARY_PATH
# apprun_templates.sh's generated AppRun script points Cemu at before
# it launches -- structurally identical to this function's own
# documented glibc bug above (a build-environment copy of something
# that must always be the host's own, shadowing the correct one via
# LD_LIBRARY_PATH), just manifesting as "no GPU/graphics device
# detected" instead of a glibc symbol crash. Mesa/Vulkan/GL driver
# libraries are universally excluded from AppImage/Flatpak/Snap bundles
# for exactly this reason (they're the userspace half of the kernel's
# own GPU driver, not a portable application dependency) -- this
# extends the same, already-proven exclusion pattern to that whole
# class rather than adding Vulkan alone and leaving GL/EGL/DRM/GBM as
# the next version of this same bug waiting to happen.
_is_never_bundle_library() {
    case "$(basename "$1")" in
        libc.so.*|libm.so.*|libpthread.so.*|libdl.so.*|librt.so.*|\
        libresolv.so.*|libnsl.so.*|libutil.so.*|libnss_*.so.*|\
        libvulkan.so.*|libGL.so.*|libGLX.so.*|libGLdispatch.so.*|\
        libEGL.so.*|libgbm.so.*|libdrm.so.*|libdrm_*.so.*)
            return 0 ;;
        *)
            return 1 ;;
    esac
}

bundle_library_dependencies() {
    local binary_path="$1" dest_lib_dir="$2"
    mkdir -p "${dest_lib_dir}"

    local ldd_line lib_path
    while IFS= read -r ldd_line; do
        # Matches "name.so => /real/path (0xaddr)" and bare
        # "/real/path (0xaddr)" (the form ldd uses for the interpreter
        # itself) -- extracts the path in either case; skips
        # "name.so (0xaddr)" (no "=>", no leading "/") entirely, which
        # is linux-vdso.so.1's form (no real file to copy).
        if [[ "${ldd_line}" == *"=>"* ]]; then
            lib_path="$(echo "${ldd_line}" | sed -n 's/.*=> \(\/[^ ]*\).*/\1/p')"
        elif [[ "${ldd_line}" == /* ]]; then
            lib_path="$(echo "${ldd_line}" | sed -n 's/^[[:space:]]*\(\/[^ ]*\).*/\1/p')"
        else
            continue
        fi
        [[ -n "${lib_path}" && -f "${lib_path}" ]] || continue
        # ld-linux itself, and glibc's own tightly-ld.so-coupled
        # components: see this function's header comment for why both
        # are deliberately not bundled.
        [[ "$(basename "${lib_path}")" == ld-linux-* ]] && continue
        _is_never_bundle_library "${lib_path}" && continue
        [[ -e "${dest_lib_dir}/$(basename "${lib_path}")" ]] && continue
        cp -L "${lib_path}" "${dest_lib_dir}/$(basename "${lib_path}")"
    done < <(ldd "${binary_path}" 2>/dev/null || true)
}

# pack_appimage <binary_path> <output_appimage_path> <app_name> <apprun_script_path> [extra_binaries] [extra_dirs]
#
# <apprun_script_path> must already be a complete, executable AppRun
# script (its content becomes AppDir/AppRun verbatim) -- see the callers
# in scripts/emudeck-replace-in-place.sh for what that script actually
# does per emulator. <app_name> is used for the .desktop file's basename,
# Icon= key, and the binary's installed name under AppDir/usr/bin/.
#
# <extra_binaries> is an optional colon-separated list of additional file
# paths to copy into AppDir/usr/bin/ alongside the main binary, kept
# generic rather than a fixed second parameter -- Azahar's/Cemu's AppRun
# needs a bundled dualdeck-host-service to spawn as a fallback when no
# persistent daemon is already running (see
# scripts/emudeck-replace-in-place.sh), melonDS's doesn't need it at all,
# and a future adapter might need more than one extra binary.
#
# <extra_dirs> is an optional colon-separated list of directory paths to
# copy into AppDir/usr/bin/ (each under its own basename), needed because
# not every emulator is fully self-contained in its binary alone -- real
# bug, caught by an actual real-hardware Cemu launch attempt after this
# tool switched to prebuilt AppImages: Cemu's own CMakeLists.txt ships a
# `resources/` and `gameProfiles/` directory as static content sitting
# directly next to `Cemu_release` in its build output (confirmed by its
# macOS packaging step, which explicitly copies
# `${CMAKE_SOURCE_DIR}/bin/{gameProfiles,resources}` into the .app
# bundle for exactly this reason -- Linux builds never needed an
# equivalent copy step because the binary already lands in that same
# directory). `pack_appimage()` previously copied only the raw binary,
# silently dropping both directories -- Cemu can't fully initialize its
# GUI without them (no window ever appears, no error printed; see
# docs/known-limitations.md's entry on this for the real-hardware
# repro). melonDS/Azahar need no extra directories.
#
# Uses a placeholder 1x1 PNG icon (appimagetool requires SOME icon file to
# exist and be referenced by the .desktop file, and this project has no
# real icon asset yet -- cosmetic only, doesn't affect functionality, but
# worth fixing once a real DualDeck icon exists).
pack_appimage() {
    local binary_path="$1" output_path="$2" app_name="$3" apprun_script_path="$4"
    local extra_binaries="${5:-}"
    local extra_dirs="${6:-}"

    [[ -f "${binary_path}" ]] || { echo "pack_appimage: no such binary: ${binary_path}" >&2; return 1; }
    [[ -f "${apprun_script_path}" ]] || { echo "pack_appimage: no such AppRun script: ${apprun_script_path}" >&2; return 1; }

    local appimagetool
    appimagetool="$(ensure_appimagetool)"

    local appdir
    appdir="$(mktemp -d)"
    # Real bug: a `trap ... RETURN` set inside a function is NOT scoped to
    # that function's own return -- it's global shell state that fires on
    # the return of the very next function call anywhere in the script,
    # by which point `appdir` (a `local` here) is out of scope, crashing
    # under `set -u` ("appdir: unbound variable") on whatever unrelated
    # function happens to return next -- in practice, this function's own
    # caller, immediately after a fully successful AppImage repack.
    # Self-clearing (`trap - RETURN` inside the trap body itself) so it
    # only ever fires once, for this function's own return.
    trap 'rm -rf "${appdir}"; trap - RETURN' RETURN

    mkdir -p "${appdir}/usr/bin" "${appdir}/usr/lib"
    cp "${binary_path}" "${appdir}/usr/bin/$(basename "${binary_path}")"
    chmod +x "${appdir}/usr/bin/$(basename "${binary_path}")"
    bundle_library_dependencies "${binary_path}" "${appdir}/usr/lib"

    if [[ -n "${extra_binaries}" ]]; then
        local IFS=":"
        local extra
        for extra in ${extra_binaries}; do
            [[ -f "${extra}" ]] || { echo "pack_appimage: no such extra binary: ${extra}" >&2; return 1; }
            cp "${extra}" "${appdir}/usr/bin/$(basename "${extra}")"
            chmod +x "${appdir}/usr/bin/$(basename "${extra}")"
            bundle_library_dependencies "${extra}" "${appdir}/usr/lib"
        done
    fi

    if [[ -n "${extra_dirs}" ]]; then
        local IFS=":"
        local dir
        for dir in ${extra_dirs}; do
            [[ -d "${dir}" ]] || { echo "pack_appimage: no such extra directory: ${dir}" >&2; return 1; }
            cp -a "${dir}" "${appdir}/usr/bin/$(basename "${dir}")"

            # Real user report, 2026-08-01: melonDS/Azahar got past
            # "Could not find the Qt platform plugin" (the extra_dirs
            # bundling fix that added Qt's platforms/ directory here)
            # only to hit "Could not LOAD the Qt platform plugin ...
            # even though it was found" -- libqxcb.so/libqwayland-*.so
            # are themselves real ELF shared objects with their own large
            # dependency chains (libQt6XcbQpa.so.6, a dozen libxcb-*.so,
            # libxkbcommon.so, libEGL.so, libfontconfig.so, ...) that
            # bundle_library_dependencies() never saw, because it only
            # ever ran against the *main binary* above -- a dlopen()'d
            # plugin's own dependencies are invisible to `ldd
            # <main binary>` no matter how thorough that pass is, since
            # the plugin is a separate ELF file loaded later, not part of
            # the main binary's own dynamic dependency graph at all.
            # Scans every extra_dir for actual shared-library files
            # (*.so, *.so.N) and bundles each one's own dependencies too
            # -- a no-op for Cemu's resources/gameProfiles (plain data,
            # no .so files in there), but load-bearing for melonDS/
            # Azahar's bundled Qt platform plugins.
            local plugin_so
            while IFS= read -r -d '' plugin_so; do
                bundle_library_dependencies "${plugin_so}" "${appdir}/usr/lib"
            done < <(find "${dir}" -type f \( -name '*.so' -o -name '*.so.*' \) -print0)
        done
    fi

    cp "${apprun_script_path}" "${appdir}/AppRun"
    chmod +x "${appdir}/AppRun"

    cat > "${appdir}/${app_name}.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=${app_name}
Exec=AppRun
Icon=${app_name}
Categories=Game;Emulator;
DESKTOP

    # Minimal valid 1x1 transparent PNG -- see this function's own header
    # comment for why a placeholder is used here.
    base64 -d > "${appdir}/${app_name}.png" <<'PNG'
iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=
PNG

    rm -f "${output_path}"
    # --appimage-extract-and-run: appimagetool is itself distributed as an
    # AppImage, which normally self-mounts via FUSE -- unavailable in most
    # CI containers and some sandboxed desktop environments. This flag
    # extracts and runs it directly instead, the standard workaround (see
    # AppImage/AppImageKit's own docs), needed here since this runs inside
    # build-release.sh-style CI environments as often as on a real desktop.
    ARCH=x86_64 "${appimagetool}" --appimage-extract-and-run "${appdir}" "${output_path}"
    chmod +x "${output_path}"
}
