#!/usr/bin/env bash
# Downloads DualDeck's prebuilt, patched melonDS/Azahar/Cemu AppImages
# (published by scripts/build-release.sh as release assets) and installs
# each one directly at the exact path EmuDeck's own launcher scripts/Steam
# shortcuts already point at (~/Applications/*.AppImage), backing up the
# original first -- so an existing EmuDeck install and its Steam shortcuts
# keep working completely unedited, just running DualDeck's remote-
# streaming-capable binary instead of stock. See docs/known-limitations.md's
# 2026-08-01 "prebuilt AppImages" entry for the full design rationale
# (in particular: EmuDeck's exact AppImage naming/glob convention has not
# been confirmed against a real EmuDeck install from inside this repo --
# see scripts/lib/emudeck_paths.sh's own header).
#
# This used to clone and compile melonDS/Azahar/Cemu from source on
# whatever machine ran it -- a Distrobox container on immutable systems,
# 30+ build-dependency packages, vcpkg for Cemu -- which meant every user's
# own uncontrolled machine was a build environment, and a long tail of
# real-hardware failures (LD_PRELOAD leaking into child processes' output,
# glibc/Qt ABI mismatches, missing kernel headers) traced back to exactly
# that. The actual builds now happen once, in CI's controlled environment
# (see build-release.sh's "Packaging prebuilt AppImages" step), and this
# script's only job is downloading the result and installing it at the
# right path -- the same download-and-verify approach scripts/
# DualDeck-Installer.sh already uses for the client/host archives.
#
# Runnable either from a checkout of this repo, or from the packaged copy
# build-release.sh bundles into every release archive at
# host/emudeck-integration/ (see that packaging step's own comment for why
# the copy works unmodified in both locations). Either way, run it on the
# same machine EmuDeck is installed on.
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

# Cheap, unconditional insurance against Steam's LD_PRELOAD/LD_LIBRARY_PATH
# (this tool is normally launched from a Steam shortcut) leaking into any
# child process this script spawns -- curl, sha256sum, python3. See
# docs/known-limitations.md's 2026-08-01 entries for the real-world reports
# that motivated always stripping both unconditionally, before any child
# process can inherit them, rather than only at whichever call site first
# surfaces a problem.
unset LD_PRELOAD LD_LIBRARY_PATH

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work_dir="${EMUDECK_REPLACE_WORKDIR:-$(mktemp -d)}"
trap '
    status=$?
    if [[ "${status}" -ne 0 ]]; then
        echo "== Failed -- work directory preserved for debugging: ${work_dir} ==" >&2
    else
        rm -rf "${work_dir}"
    fi
' EXIT

# shellcheck source=scripts/lib/emudeck_paths.sh
source "${repo_root}/scripts/lib/emudeck_paths.sh"
# shellcheck source=scripts/lib/host_firewall.sh
source "${repo_root}/scripts/lib/host_firewall.sh"

manifest_py="${repo_root}/scripts/lib/appimage_manifest.py"

repo="Crimson3076/DualDeck"
# Overridable for testing only (e.g. pointing at a local fake HTTP server
# serving fixture release assets), matching DualDeck-Installer.sh's
# identical DUALDECK_INSTALLER_DOWNLOAD_BASE override -- never something a
# real user needs to set.
download_base="${DUALDECK_REPLACE_DOWNLOAD_BASE:-https://github.com/${repo}/releases/latest/download}"

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
# Counts real installs this run (incremented in replace_in_place_one),
# so the firewall step at the end only runs when it might actually
# matter -- not on a --dry-run, and not when every emulator was skipped
# (not found, or the user declined the confirmation prompt).
installed_count=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --emulator)
            emulators+=("$2")
            shift 2
            ;;
        --dry-run) dry_run=1; shift ;;
        --yes) assume_yes=1; shift ;;
        -h|--help)
            sed -n '2,39p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
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

if ! command -v curl >/dev/null 2>&1; then
    echo "error: curl is required to download DualDeck's patched builds -- install it and try again." >&2
    exit 1
fi
if ! command -v sha256sum >/dev/null 2>&1; then
    echo "error: sha256sum is required to verify downloads -- install coreutils and try again." >&2
    exit 1
fi

# ---- download + verify ----
# Downloaded once (lazily, on first use) and reused for every emulator
# processed this run -- GitHub issue #26's same "verify before installing
# anything" posture DualDeck-Installer.sh already applies to the client/
# host archives, just against the three prebuilt AppImages instead.
ensure_sha256sums_downloaded() {
    if [[ -f "${work_dir}/SHA256SUMS" ]]; then
        return
    fi
    echo "== Downloading SHA256SUMS to verify downloads against ==" >&2
    curl -fsSL --max-time 30 -o "${work_dir}/SHA256SUMS" "${download_base}/SHA256SUMS"
}

# download_patched_appimage <emulator>
#
# Downloads dualdeck-<emulator>-patched-linux-x86_64.AppImage from the
# latest DualDeck release into work_dir and verifies it against SHA256SUMS,
# refusing to return a path if verification fails. Prints the downloaded
# path on stdout on success (progress/errors go to stderr instead -- this
# is called as `path="$(download_patched_appimage ...)"`, so anything
# written to stdout here would silently corrupt the returned path). Cached
# within a single run (work_dir persists for the whole script) so re-
# processing the same emulator twice in one invocation doesn't re-download.
download_patched_appimage() {
    local emulator="$1"
    local asset_name="dualdeck-${emulator}-patched-linux-x86_64.AppImage"
    local dest_path="${work_dir}/${asset_name}"

    if [[ ! -f "${dest_path}" ]]; then
        ensure_sha256sums_downloaded
        echo "== ${emulator}: downloading ${asset_name} from the latest DualDeck release ==" >&2
        if ! curl -fsSL --max-time 180 -o "${dest_path}" "${download_base}/${asset_name}"; then
            echo "error: ${emulator}: couldn't download ${asset_name} from the latest GitHub release." >&2
            echo "Check your internet connection, or grab it manually from https://github.com/${repo}/releases/latest" >&2
            rm -f "${dest_path}"
            return 1
        fi

        echo "== ${emulator}: verifying download integrity ==" >&2
        if ! (cd "${work_dir}" && sha256sum -c --ignore-missing SHA256SUMS 2>/dev/null | grep -qx "${asset_name}: OK"); then
            echo "error: ${emulator}: checksum verification failed for ${asset_name} -- refusing to install" >&2
            echo "an unverified download. Try again, or download manually from https://github.com/${repo}/releases/latest" >&2
            rm -f "${dest_path}"
            return 1
        fi
        log "${emulator}: downloaded and verified ${asset_name}"
    fi

    echo "${dest_path}"
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
                echo "   (EmuDeck's own updater likely replaced it) -- re-installing over it now. ==" >&2
                log "${emulator}: drift detected, re-installing"
            fi
            ;;
    esac

    if [[ "${dry_run}" -eq 1 ]]; then
        echo "== ${emulator}: --dry-run, stopping here (would download + install over ${appimage_path}) =="
        return 0
    fi

    if [[ "${assume_yes}" -ne 1 ]]; then
        read -r -p "Replace ${appimage_path} with DualDeck's patched build? [y/N] " reply
        case "${reply}" in
            [yY]|[yY][eE][sS]) ;;
            *) echo "== ${emulator}: skipped (not confirmed) =="; return 0 ;;
        esac
    fi

    local downloaded_appimage
    downloaded_appimage="$(download_patched_appimage "${emulator}")"

    local new_appimage="${appimage_path}.new"
    cp "${downloaded_appimage}" "${new_appimage}"
    chmod +x "${new_appimage}"

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
    installed_count=$((installed_count + 1))
}

for emulator in "${emulators[@]}"; do
    replace_in_place_one "${emulator}"
done

# Real user report, 2026-08-01: a patched AppImage launched via EmuDeck's
# own Steam shortcut spawns its own private, ephemeral dualdeck-host-
# service (see scripts/lib/apprun_templates.sh's generate_apprun_out_of_
# process()) -- unlike install-steam-shortcut.sh/install-host-
# distrobox.sh, nothing in that launch path ever opened the host's
# firewall for it, so on a host with firewalld/ufw active (Bazzite's
# default) the client could discover it but never actually connect --
# with the further real bug (see client/src/net_client.cpp's
# kHandshakeTimeoutMs) that a silently-dropped connection attempt used
# to hang the whole client indefinitely, not just fail cleanly. Run once
# here, at install time (matching every other host-side install path's
# "open ports when something is installed, not on every subsequent game
# launch" convention -- this needs sudo, which has no business prompting
# in the middle of a Steam game launch), rather than from the AppRun
# script itself.
if [[ "${dry_run}" -ne 1 && "${installed_count}" -gt 0 ]]; then
    echo "== Opening this host's firewall for DualDeck's ports (if not already open) =="
    ensure_host_firewall_ports || true
fi

echo "== Done. Launch your existing EmuDeck/Steam shortcuts as usual -- no shortcut edits needed. =="
