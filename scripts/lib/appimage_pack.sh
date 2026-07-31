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

# pack_appimage <binary_path> <output_appimage_path> <app_name> <apprun_script_path>
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
# Uses a placeholder 1x1 PNG icon (appimagetool requires SOME icon file to
# exist and be referenced by the .desktop file, and this project has no
# real icon asset yet -- cosmetic only, doesn't affect functionality, but
# worth fixing once a real DualDeck icon exists).
pack_appimage() {
    local binary_path="$1" output_path="$2" app_name="$3" apprun_script_path="$4"
    local extra_binaries="${5:-}"

    [[ -f "${binary_path}" ]] || { echo "pack_appimage: no such binary: ${binary_path}" >&2; return 1; }
    [[ -f "${apprun_script_path}" ]] || { echo "pack_appimage: no such AppRun script: ${apprun_script_path}" >&2; return 1; }

    local appimagetool
    appimagetool="$(ensure_appimagetool)"

    local appdir
    appdir="$(mktemp -d)"
    trap 'rm -rf "${appdir}"' RETURN

    mkdir -p "${appdir}/usr/bin"
    cp "${binary_path}" "${appdir}/usr/bin/$(basename "${binary_path}")"
    chmod +x "${appdir}/usr/bin/$(basename "${binary_path}")"

    if [[ -n "${extra_binaries}" ]]; then
        local IFS=":"
        local extra
        for extra in ${extra_binaries}; do
            [[ -f "${extra}" ]] || { echo "pack_appimage: no such extra binary: ${extra}" >&2; return 1; }
            cp "${extra}" "${appdir}/usr/bin/$(basename "${extra}")"
            chmod +x "${appdir}/usr/bin/$(basename "${extra}")"
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
