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
#   ./scripts/emudeck-replace-in-place.sh [--emulator ...] --status
#   ./scripts/emudeck-replace-in-place.sh [--emulator ...] --restore [--dry-run]
#
# --status  Report, per emulator, where EmuDeck's install is, whether the
#           file there is still the one DualDeck installed (or the stock
#           original, or something newer EmuDeck has since replaced it
#           with), which DualDeck version patched it, and when. Changes
#           nothing. Start here when an emulator misbehaves after an
#           update -- it answers "what am I actually running?", which
#           a rising version number on its own does not.
# --restore Put the stock emulator back from the .dualdeck-original saved
#           at patch time, and clear the manifest so a later run patches
#           it cleanly from the original again. For melonDS this also
#           restores EmuDeck's launcher script, which the patcher rewrites
#           too -- restoring only the AppImage leaves a launcher still
#           pointing at the patched binary, which still looks broken.
#           The backup is copied, not moved, so this stays repeatable.
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
# --status: report only, change nothing. --restore: put the stock
# emulator back from the .dualdeck-original this script saved. Both added
# 2026-08-04 on a real user request, after an EmuDeck re-patch left
# melonDS broken and there was no supported way either to ask what was
# installed or to undo it -- the backup and the manifest (with its
# dualdeck_version) had existed all along, but nothing exposed them, so
# the only recovery was hand-copying files.
status_only=0
restore_only=0
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
        --status) status_only=1; shift ;;
        --restore) restore_only=1; shift ;;
        -h|--help)
            # Prints the header block by finding where it actually ends
            # rather than hardcoding a line number: the previous fixed
            # '2,39p' silently truncated --help mid-sentence the moment
            # the header grew, which is a documentation bug that hides
            # exactly the options a confused user is looking for.
            awk 'NR>1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "${BASH_SOURCE[0]}"
            exit 0
            ;;
        *)
            echo "error: unknown argument: $1" >&2
            exit 1
            ;;
    esac
done
# Validated here, up with the rest of the argument checking, rather than
# down beside the code that acts on it: the failure trap installed near
# the top of this script prints "work directory preserved for debugging"
# on any non-zero exit, which is actively misleading for what is just a
# bad pair of flags.
if [[ "${status_only}" -eq 1 && "${restore_only}" -eq 1 ]]; then
    # The EXIT trap is cleared first so this exits cleanly: that trap is
    # installed before any argument parsing can have happened, and would
    # otherwise announce "work directory preserved for debugging" for
    # what is simply a bad pair of flags -- pointing the user at an empty
    # temp directory and implying something broke mid-run.
    trap - EXIT
    rm -rf "${work_dir}"
    echo "error: --status and --restore are mutually exclusive." >&2
    exit 1
fi

if [[ "${#emulators[@]}" -eq 0 ]]; then
    emulators=(melonds azahar cemu)
fi
for e in "${emulators[@]}"; do
    case "${e}" in
        melonds|azahar|cemu) ;;
        *) echo "error: --emulator must be one of: melonds azahar cemu (got: ${e})" >&2; exit 1 ;;
    esac
done

# Only the patching path downloads anything, so --status and --restore
# deliberately skip these checks: a host whose install is already broken
# is exactly where you most want to be able to ask what happened and undo
# it, and refusing to do either over a missing download tool would be
# perverse.
if [[ "${status_only}" -eq 0 && "${restore_only}" -eq 0 ]]; then
    if ! command -v curl >/dev/null 2>&1; then
        echo "error: curl is required to download DualDeck's patched builds -- install it and try again." >&2
        exit 1
    fi
    if ! command -v sha256sum >/dev/null 2>&1; then
        echo "error: sha256sum is required to verify downloads -- install coreutils and try again." >&2
        exit 1
    fi
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

# bootstrap_melonds_flatpak_launcher <flatpak_launcher_path>
#
# Real user report, 2026-08-01: see find_emudeck_melonds_flatpak_launcher()
# in scripts/lib/emudeck_paths.sh for the full story -- EmuDeck launches
# melonDS via Flatpak on this configuration, so there is no existing
# AppImage to replace. Downloads the patched AppImage fresh to a fixed
# path under emudeck_applications_dir(), backs up melonds.sh, and
# rewrites its one `flatpak run` line to invoke the new AppImage instead.
# Real melonds.sh content confirmed 2026-08-03 (superseding this
# function's own earlier, apparently-stale assumption of a single `exec
# flatpak run ...` line): `/usr/bin/flatpak run net.kuribo64.melonDS
# "${@}"`, with real cleanup steps (cloud_sync_uploadForced, `rm -rf
# "$savesPath/.gaming"`) on the lines *after* it -- this rewrite
# deliberately does not introduce `exec` for exactly that reason (see the
# sed call below's own comment). Also drops EmuDeck's own `--boot=never`
# flag in the process where present (an AppImage launch takes the ROM
# path as a plain positional argument, the same convention azahar.sh/
# cemu.sh already use, so this also happens to fix the separately-
# reported "melonDS opens but doesn't load the ROM automatically," which
# was that flag's doing).
#
# Self-limiting to one run: once this succeeds, the newly-installed
# AppImage sits exactly where find_emudeck_melonds_appimage() looks, so
# every future invocation of this script takes the normal, already-
# established replace-in-place path automatically for melonds --
# this function never runs again after the first time.
bootstrap_melonds_flatpak_launcher() {
    local flatpak_launcher="$1"
    local apps_dir new_appimage_path
    apps_dir="$(emudeck_applications_dir)"
    new_appimage_path="${apps_dir}/melonDS.AppImage"

    echo "== melonds: EmuDeck launches melonDS via Flatpak (${flatpak_launcher}), not an" >&2
    echo "   AppImage -- DualDeck's patch has had no effect on this install. Redirecting" >&2
    echo "   EmuDeck's own launcher to a freshly-downloaded, patched AppImage instead. ==" >&2

    if [[ "${dry_run}" -eq 1 ]]; then
        echo "== melonds: --dry-run, stopping here (would install ${new_appimage_path} and" >&2
        echo "   redirect ${flatpak_launcher} to exec it) ==" >&2
        return 0
    fi

    if [[ "${assume_yes}" -ne 1 ]]; then
        read -r -p "Install DualDeck's patched melonDS at ${new_appimage_path} and redirect EmuDeck's launcher to use it instead of the Flatpak? [y/N] " reply
        case "${reply}" in
            [yY]|[yY][eE][sS]) ;;
            *) echo "== melonds: skipped (not confirmed) =="; return 0 ;;
        esac
    fi

    local downloaded_appimage
    downloaded_appimage="$(download_patched_appimage "melonds")"

    mkdir -p "${apps_dir}"
    cp "${downloaded_appimage}" "${new_appimage_path}"
    chmod +x "${new_appimage_path}"

    local launcher_backup="${flatpak_launcher}.dualdeck-original"
    if [[ ! -f "${launcher_backup}" ]]; then
        cp "${flatpak_launcher}" "${launcher_backup}"
    fi

    # replace_in_place_one()'s normal flow (which every future run of
    # this script for melonds takes once this function has run once --
    # see this function's own header comment) refuses to proceed if a
    # manifest exists but "${new_appimage_path}.dualdeck-original" is
    # missing, as a safety net against exactly the kind of corruption
    # that would otherwise be silent (see that check's own comment).
    # There genuinely is no original *AppImage* to back up here -- the
    # real original state is the Flatpak launcher, already preserved
    # above -- so this placeholder exists purely to satisfy that later
    # file-existence check while staying honest about what it actually
    # is, rather than a copy of anything real.
    if [[ ! -f "${new_appimage_path}.dualdeck-original" ]]; then
        echo "DualDeck note: melonDS was originally installed via Flatpak (net.kuribo64.melonDS)," \
             "not an AppImage -- there is no original AppImage to preserve here. The original" \
             "launcher script (before DualDeck redirected it) is backed up at:" \
             "${launcher_backup}" > "${new_appimage_path}.dualdeck-original"
    fi

    # Matches against net.kuribo64.melonDS -- the real, confirmed Flatpak
    # app ID EmuDeck uses -- rather than a looser pattern, so this only
    # ever rewrites the exact line it was written to handle. Refuses to
    # touch the file if that line isn't found (e.g. EmuDeck's own format
    # changed) rather than silently doing nothing to it.
    if ! grep -q "flatpak run net\.kuribo64\.melonDS" "${flatpak_launcher}"; then
        echo "error: melonds: ${flatpak_launcher} no longer contains the expected 'flatpak run" >&2
        echo "net.kuribo64.melonDS' line -- EmuDeck's launcher format may have changed. Not" >&2
        echo "touching it; the original is still backed up at ${launcher_backup}." >&2
        log "melonds: launcher redirect refused, expected line not found"
        return 1
    fi
    # Real user report, 2026-08-03: the actual current melonds.sh format
    # is `/usr/bin/flatpak run net.kuribo64.melonDS "${@}"` -- no leading
    # `exec`, and followed by real cleanup lines (cloud_sync_uploadForced,
    # `rm -rf "$savesPath/.gaming"`) that only run because the flatpak
    # invocation above them is a plain foreground command, not `exec`'d.
    # This replacement deliberately matches that same convention (no
    # `exec` here either) -- an earlier version of this line used `exec`,
    # which would have discarded those cleanup steps entirely the moment
    # this redirect took effect, a real behavioral regression to
    # EmuDeck's own save/cloud-sync flow that was never actually
    # triggered before this bug was found and fixed (see the
    # verification check right below, added at the same time: without
    # it, a sed pattern this loosened to match the real line above would
    # otherwise have needed a live test to catch introducing this new
    # problem while silently fixing the original one).
    sed -i -E "s|^.*flatpak run net\.kuribo64\.melonDS.*|\"${new_appimage_path}\" \"\\\$@\"|" \
        "${flatpak_launcher}"

    # Real bug: sed -i exits 0 even when its pattern matches nothing --
    # the grep check above only proves the *original* file contained the
    # expected substring, not that the anchored sed pattern (which used
    # to require the line to start with exactly "exec flatpak run", now
    # loosened to "anything...flatpak run" above for the same reason)
    # actually matched and rewrote it. Without this check, a launcher
    # format this pattern doesn't quite match would silently do nothing
    # while the rest of this function still reported success and counted
    # melonds as installed -- exactly indistinguishable, from the user's
    # side, from a real install (EmuDeck's Steam shortcut keeps launching
    # the untouched, still-Flatpak, still-completely-stock melonDS, with
    # no DualDeck patch, no remote-server toggle, nothing). Verify the
    # rewrite actually landed before reporting success at all.
    if grep -q "flatpak run net\.kuribo64\.melonDS" "${flatpak_launcher}"; then
        echo "error: melonds: rewriting ${flatpak_launcher} to exec the patched AppImage" >&2
        echo "didn't take effect (the flatpak run line is still present after the rewrite" >&2
        echo "attempt) -- EmuDeck's launcher format doesn't match what this script expects." >&2
        echo "Not reporting success; the original is still backed up at ${launcher_backup}." >&2
        log "melonds: launcher redirect verification failed after sed"
        return 1
    fi

    # There is no real "original AppImage" here to preserve a hash of --
    # the actual original was a Flatpak install, never touched by this
    # function at all. A sentinel string (never a valid sha256, so it can
    # never spuriously match a real file's hash) rather than leaving this
    # field empty, so a manifest that's read back stays self-explanatory.
    python3 "${manifest_py}" write "${new_appimage_path}" \
        --dualdeck-version "${dualdeck_version}" \
        --original-sha256 "none (bootstrapped from a Flatpak launcher, no original AppImage)"

    echo "== melonds: installed DualDeck's patched build at ${new_appimage_path} =="
    echo "   (EmuDeck's launcher redirected; original backed up at ${launcher_backup})"
    log "melonds: bootstrapped from Flatpak launcher, dualdeck_version=${dualdeck_version}"
    installed_count=$((installed_count + 1))
}

# ---- per-emulator orchestration ----
replace_in_place_one() {
    local emulator="$1"
    local appimage_path=""

    case "${emulator}" in
        melonds)
            if ! appimage_path="$(find_emudeck_melonds_appimage)"; then
                local flatpak_launcher
                if flatpak_launcher="$(find_emudeck_melonds_flatpak_launcher)"; then
                    bootstrap_melonds_flatpak_launcher "${flatpak_launcher}"
                    return $?
                fi
                echo "== melonds: no EmuDeck install found under $(emudeck_applications_dir), skipping =="
                return 0
            fi
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

    # Redirect EmuDeck's launcher too, every run. Replacing the AppImage
    # alone leaves Steam shortcuts launching the stock emulator (for
    # melonDS, the Flatpak, which never referenced the AppImage in the
    # first place), and re-running this after an EmuDeck reinstall is
    # exactly when the launcher needs putting back.
    install_emudeck_launcher_shims "${emulator}"
}

# resolve_installed_appimage <emulator>
#
# Prints the EmuDeck AppImage path for <emulator>, or returns non-zero if
# there isn't one. Read-only: unlike replace_in_place_one()'s own
# resolution it never bootstraps a Flatpak launcher or prints progress,
# so --status and --restore can ask "where is it?" without changing
# anything on the way past.
resolve_installed_appimage() {
    case "$1" in
        melonds) find_emudeck_melonds_appimage ;;
        azahar)  find_emudeck_azahar_appimage ;;
        cemu)    find_emudeck_cemu_appimage ;;
        *)       return 1 ;;
    esac
}

# ---- EmuDeck launcher shims (real user report, 2026-08-04) ----
#
# Replacing ~/Applications/<emu>.AppImage is not enough to make Steam
# shortcuts work, and for melonDS it never was: EmuDeck's stock
# melonds.sh runs the Flatpak (net.kuribo64.melonDS) and never mentions
# that AppImage at all, so a Steam launch got an entirely unpatched
# emulator. Azahar's launcher does use the AppImage, but running it
# directly on Bazzite dies in Vulkan init with ErrorIncompatibleDriver,
# where the same build works inside the dualdeck-host container.
#
# So the launcher itself has to be redirected. It is redirected to a
# one-line `exec` of a DualDeck-owned script under the central install
# directory, rather than having the real logic written into a file
# EmuDeck manages: releases can then replace the logic wholesale, the
# patch applied to EmuDeck's file stays trivial to verify, and there is
# no large generated script sitting somewhere EmuDeck may rewrite.
#
# Re-applied on every patch run, which is the drift repair the reporter
# asked for: reinstalling melonDS through EmuDeck restores its stock
# Flatpak launcher, and before this, re-running the DualDeck patch did
# not put the working launcher back.

emudeck_launcher_targets() {
    # Both locations matter: EmuDeck keeps launchers in tools/launchers,
    # but Steam ROM Manager shortcuts have been observed pointing at
    # roms/emulators too, and the reporter had to replace both before
    # shortcuts worked reliably.
    local emulator="$1"
    echo "$(emudeck_launchers_dir)/${emulator}.sh"
    echo "${HOME}/Emulation/roms/emulators/${emulator}.sh"
}

dualdeck_shim_for() {
    case "$1" in
        melonds) echo "launch-emudeck-melonds.sh" ;;
        azahar)  echo "launch-emudeck-azahar.sh" ;;
        *)       return 1 ;;
    esac
}

install_emudeck_launcher_shims() {
    local emulator="$1" shim target backup
    shim="$(dualdeck_shim_for "${emulator}")" || return 0  # cemu: no shim yet
    local shim_path="${HOME}/.config/dualdeck/install/internal/${shim}"
    local wrote_any=0

    for target in $(emudeck_launcher_targets "${emulator}"); do
        # Only touch launchers that already exist. Creating one where
        # EmuDeck never put one would invent a path nothing launches and
        # that EmuDeck might later collide with.
        [[ -f "${target}" ]] || continue

        if grep -q "internal/${shim}" "${target}" 2>/dev/null; then
            echo "== ${emulator}: launcher already points at DualDeck: ${target} =="
            wrote_any=1
            continue
        fi

        # Back up EmuDeck's own launcher once, and only when the current
        # contents are genuinely theirs -- guarded by the grep above, so
        # a second run can never overwrite the pristine copy with our own
        # shim (the failure mode that would make --restore useless).
        backup="${target}.dualdeck-original"
        if [[ ! -f "${backup}" ]]; then
            echo "== ${emulator}: backing up EmuDeck's launcher to ${backup} =="
            cp -a "${target}" "${backup}"
        fi

        if [[ "${dry_run}" -eq 1 ]]; then
            echo "== ${emulator}: [dry run] would redirect ${target} -> ${shim} =="
            wrote_any=1
            continue
        fi

        echo "== ${emulator}: redirecting ${target} -> ${shim} =="
        cat > "${target}" <<SHIM
#!/usr/bin/env bash
# Replaced by DualDeck (scripts/emudeck-replace-in-place.sh). The stock
# EmuDeck launcher is preserved next to this file as
# $(basename "${target}").dualdeck-original, and DualDeck's
# --restore mode puts it back.
exec "\$HOME/.config/dualdeck/install/internal/${shim}" "\$@"
SHIM
        chmod +x "${target}"
        wrote_any=1
    done

    if [[ "${wrote_any}" -eq 0 ]]; then
        echo "== ${emulator}: no EmuDeck launcher found to redirect (looked in" \
             "$(emudeck_launchers_dir) and ${HOME}/Emulation/roms/emulators) =="
        return 0
    fi

    verify_emudeck_launcher_shims "${emulator}"
}

# Fail loudly rather than reporting a successful patch that did not take
# -- explicitly requested, after a run that claimed success while Steam
# still launched the stock Flatpak.
verify_emudeck_launcher_shims() {
    local emulator="$1" shim target failed=0
    shim="$(dualdeck_shim_for "${emulator}")" || return 0
    local shim_path="${HOME}/.config/dualdeck/install/internal/${shim}"

    if [[ "${dry_run}" -ne 1 && ! -x "${shim_path}" ]]; then
        echo "error: ${emulator}: launcher now points at ${shim_path}, which is missing or" >&2
        echo "not executable. Install/update the DualDeck host first (its Steam shortcut," >&2
        echo "or host/internal/install-steam-shortcut.sh) so that file exists." >&2
        failed=1
    fi

    for target in $(emudeck_launcher_targets "${emulator}"); do
        [[ -f "${target}" ]] || continue
        if [[ "${dry_run}" -eq 1 ]]; then continue; fi
        if ! grep -q "internal/${shim}" "${target}"; then
            echo "error: ${emulator}: ${target} does not reference ${shim} after patching." >&2
            failed=1
        fi
    done

    if [[ "${failed}" -ne 0 ]]; then
        echo "error: ${emulator}: EmuDeck launcher integration could not be verified --" >&2
        echo "Steam shortcuts would still launch the unpatched emulator." >&2
        return 1
    fi
    echo "== ${emulator}: EmuDeck launcher integration verified =="
}

restore_emudeck_launcher_shims() {
    local emulator="$1" target backup
    dualdeck_shim_for "${emulator}" >/dev/null || return 0
    for target in $(emudeck_launcher_targets "${emulator}"); do
        backup="${target}.dualdeck-original"
        [[ -f "${backup}" ]] || continue
        if [[ "${dry_run}" -eq 1 ]]; then
            echo "${emulator}: [dry run] would restore launcher ${target}"
            continue
        fi
        echo "== ${emulator}: restoring EmuDeck's launcher ${target} =="
        cp -a "${backup}" "${target}"
    done
}

status_one() {
    local emulator="$1" appimage_path
    if ! appimage_path="$(resolve_installed_appimage "${emulator}")"; then
        echo "${emulator}: no EmuDeck AppImage install found"
        return 0
    fi
    echo "${emulator}:"
    python3 "${manifest_py}" status "${appimage_path}" | sed 's/^/  /'
}

restore_one() {
    local emulator="$1" appimage_path
    if ! appimage_path="$(resolve_installed_appimage "${emulator}")"; then
        echo "${emulator}: no EmuDeck AppImage install found, nothing to restore"
        return 0
    fi
    local backup_path="${appimage_path}.dualdeck-original"
    if [[ ! -f "${backup_path}" ]]; then
        echo "${emulator}: no backup at ${backup_path} -- DualDeck never patched this install," >&2
        echo "   or the backup was removed. Nothing to restore." >&2
        return 0
    fi

    if [[ "${dry_run}" -eq 1 ]]; then
        echo "${emulator}: [dry run] would restore ${backup_path} -> ${appimage_path}"
        return 0
    fi

    echo "== ${emulator}: restoring the original from ${backup_path} =="
    # cp, not mv: the backup stays put so this is repeatable, and so a
    # half-finished restore can't leave the install with neither file.
    cp -a "${backup_path}" "${appimage_path}"

    # melonDS's EmuDeck install is launched through a launcher script that
    # the patcher also rewrites (see bootstrap_melonds_flatpak_launcher).
    # Restoring only the AppImage would leave that script still pointing
    # at the patched binary -- a half-restore that still looks broken,
    # which is exactly the trap a by-hand recovery falls into.
    if [[ "${emulator}" == "melonds" ]]; then
        local launcher launcher_backup
        if launcher="$(find_emudeck_melonds_flatpak_launcher)"; then
            launcher_backup="${launcher}.dualdeck-original"
            if [[ -f "${launcher_backup}" ]]; then
                echo "== melonds: also restoring the EmuDeck launcher ${launcher} =="
                cp -a "${launcher_backup}" "${launcher}"
                rm -f "${launcher}.dualdeck.json"
            fi
        fi
    fi

    # Drop the manifest last, so an interrupted restore still looks
    # "patched" (and therefore recoverable) rather than pristine-but-
    # actually-patched. Clearing it resets this install to first-time
    # state, so a later re-patch starts from the stock binary instead of
    # refusing or stacking on top of itself.
    rm -f "${appimage_path}.dualdeck.json"

    # Put EmuDeck's own launcher back as well, for the same reason the
    # melonDS launcher script is restored above: leaving a launcher that
    # still execs DualDeck's shim, after the emulator underneath it has
    # been reverted to stock, is a half-restore that still looks broken.
    restore_emudeck_launcher_shims "${emulator}"

    echo "== ${emulator}: restored. Re-run this script without --restore to patch it again. =="
    log "${emulator}: restored original from backup"
}

if [[ "${status_only}" -eq 1 ]]; then
    for emulator in "${emulators[@]}"; do
        status_one "${emulator}"
    done
    exit 0
fi

if [[ "${restore_only}" -eq 1 ]]; then
    for emulator in "${emulators[@]}"; do
        restore_one "${emulator}"
    done
    exit 0
fi

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
