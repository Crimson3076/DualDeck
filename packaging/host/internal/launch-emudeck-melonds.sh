#!/usr/bin/env bash
# DualDeck's melonDS launcher for EmuDeck / Steam ROM Manager shortcuts.
#
# EmuDeck's own melonds.sh is patched to a one-line `exec` of this file
# (see scripts/emudeck-replace-in-place.sh's install_emudeck_launcher_
# shims()), so all the real logic lives here, in a DualDeck-owned file a
# release can replace wholesale. Real user report, 2026-08-04, which is
# also where every behaviour below comes from -- the reporter arrived at
# a working launcher by hand and this is that launcher, generalised.
#
# Why this exists at all: patching ~/Applications/melonDS.AppImage was
# never going to fix Steam launches, because EmuDeck's stock melonds.sh
# runs the *Flatpak* (net.kuribo64.melonDS) and never references that
# AppImage. Steam shortcuts therefore launched a completely unpatched
# melonDS with no DualDeck streaming at all.
#
# Three things have to happen that EmuDeck's launcher does not do:
#
#   1. Route through Distrobox on immutable systems. The patched melonDS
#      dies with "libturbojpeg.so.0: cannot open shared object file" when
#      run against Bazzite's base image; it needs the dualdeck-host
#      container, same as the host menu's own DS path.
#   2. Translate Steam ROM Manager's argument form. SRM invokes the
#      launcher as `"/path/game.nds" -f`; the patched melonDS wants
#      `--boot always "/path/game.nds"` for the ROM. -f/--fullscreen is a
#      real melonDS option (CLI.cpp) and is passed straight through below
#      like any other extra arg -- it doesn't need translating -- but
#      run-host.sh now adds it by default anyway (real user report,
#      2026-08-28: melonDS otherwise opens small and windowed, matching
#      neither the Deck's display nor every other patched emulator), so
#      omitting it here changes nothing either way.
#   3. Preserve exit status, so Steam sees the emulator's result rather
#      than the wrapper's.
#
# Deliberately does NOT source EmuDeck's all.sh. That file is not safe
# under this script's own `set -euo pipefail`: it references an unbound
# `emudeckBackend` (fatal under -u) and runs a `systemctl --user stop
# EmuDeckCloudSync.service` that legitimately fails when the unit was
# never loaded (fatal under -e). Both were hit and diagnosed by the
# reporting user. Keeping strict mode here and not sourcing it is the
# trade this shim design exists to make -- cloud sync is the thing given
# up, and it is given up knowingly rather than by accident.
set -euo pipefail

install_root="${HOME}/.config/dualdeck/install"
wrapper="${install_root}/internal/run-host.sh"
container="dualdeck-host"

log="${HOME}/.cache/dualdeck/melonds-steam-launch.log"
mkdir -p "$(dirname "${log}")"
{
    echo "=== $(date --iso-8601=seconds) pid=$$ ==="
    printf 'arg: %q\n' "$@"
} >> "${log}" 2>&1

if [[ ! -x "${wrapper}" ]]; then
    echo "error: DualDeck's melonDS wrapper is missing: ${wrapper}" >&2
    echo "Re-run the DualDeck Host menu's \"Patch my EmuDeck-installed emulators\"." >&2
    echo "error: missing wrapper ${wrapper}" >> "${log}"
    exit 1
fi

# Steam ROM Manager sometimes passes the ROM already wrapped in literal
# single quotes; strip one balanced pair, never anything else, so paths
# that genuinely contain a quote survive untouched.
rom=""
extra_args=()
for argument in "$@"; do
    if [[ "${argument}" == \'*\' ]]; then
        argument="${argument:1:${#argument}-2}"
    fi
    case "${argument}" in
        -f|--fullscreen) extra_args+=("${argument}") ;; # real melonDS option (CLI.cpp) -- pass through
        -F|--FULLSCREEN) ;; # not a real melonDS option; melonDS's QCommandLineParser rejects unknown flags
        *.nds|*.NDS|*.srl|*.SRL) rom="${argument}" ;;
        *) extra_args+=("${argument}") ;;
    esac
done

# Normally auto-detected; DUALDECK_ASSUME_IMMUTABLE=1/0 forces it, which
# is what the launcher-translation tests use to exercise both routes on
# one machine (and is a way to check a shortcut's behaviour by hand
# without needing the other kind of system to hand).
immutable="${DUALDECK_ASSUME_IMMUTABLE:-auto}"
if [[ "${immutable}" == "auto" ]]; then
    if [[ -f /run/ostree-booted ]] || command -v rpm-ostree >/dev/null 2>&1; then
        immutable=1
    else
        immutable=0
    fi
fi

cmd=()
if [[ "${immutable}" == "1" ]]; then
    if ! command -v distrobox >/dev/null 2>&1; then
        echo "error: this is an immutable (rpm-ostree) system but distrobox is not installed." >&2
        exit 1
    fi
    cmd=(distrobox enter "${container}" -- "${wrapper}")
else
    cmd=("${wrapper}")
fi
# "${extra_args[@]+...}" so an empty array is safe under `set -u`.
cmd+=(${extra_args[@]+"${extra_args[@]}"})
if [[ -n "${rom}" ]]; then
    cmd+=(--boot always "${rom}")
fi

printf 'exec:' >> "${log}"; printf ' %q' "${cmd[@]}" >> "${log}"; echo >> "${log}"

# Escape hatch for the launcher-translation tests (and for anyone
# debugging a shortcut by hand): print the command that would run and
# stop, instead of launching an emulator.
if [[ "${DUALDECK_EMUDECK_LAUNCH_DRY_RUN:-0}" == "1" ]]; then
    printf '%q ' "${cmd[@]}"; echo
    exit 0
fi

exec "${cmd[@]}"
