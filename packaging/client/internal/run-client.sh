#!/usr/bin/env bash
# Runs dualdeck-client with the bundled SDL3 shared library,
# auto-installing any missing runtime system libraries first (X11/
# Wayland etc. -- SDL3 itself is bundled in ../lib/, no install needed
# for that part). Normally launched via ../../dualdeck-client.sh's
# "Launch now" menu choice, or directly as the Steam shortcut's Exe
# (install-steam-shortcut.sh points --exe straight here, bypassing the
# menu entirely) -- so the auto-update check below has to live here, not
# only in the menu's own "Check for updates" choice, to actually catch
# every launch path.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
client_root="$(cd .. && pwd)"

# Auto-update (user request: "check for an update on launch and
# automatically apply it when there's an update detected" -- no
# confirmation prompt, unlike the menu's "Check for updates" choice,
# since the whole point here is not needing a manual step). Deliberately
# fails open: check-for-updates.sh itself caps its network call at 5s and
# never exits non-zero on a reachability/parse failure (see its own
# header comment), and apply-update.sh's ~180s download is only reached
# at all when an update was actually found -- so a normal launch with no
# update available never waits on the network beyond that 5s check, and
# any failure at any step here (offline, GitHub down, download failure)
# just falls through to launching the current version rather than
# blocking or erroring. Re-execs the freshly installed copy on success
# so the rest of this launch (library check, the binary itself) runs the
# new version, not whatever was already loaded into this process image.
update_script="$(dirname "${client_root}")/check-for-updates.sh"
settings_file="${HOME}/.config/dualdeck-client/settings.conf"
auto_update_on_launch=1
if [[ -f "${settings_file}" ]] &&
   grep -Eq '^[[:space:]]*auto_update_on_launch[[:space:]]*=[[:space:]]*(0|false|off)[[:space:]]*$' \
       "${settings_file}"; then
    auto_update_on_launch=0
fi
if (( auto_update_on_launch )) && [[ -x "${update_script}" ]]; then
    if update_report="$("${update_script}" 2>/dev/null)" && \
       echo "${update_report}" | grep -q "update available:"; then
        echo "DualDeck: update available, installing automatically..." >&2
        if "${client_root}/internal/apply-update.sh" >&2; then
            echo "DualDeck: updated, relaunching..." >&2
            exec "${HOME}/.config/dualdeck-client/install/internal/run-client.sh" "$@"
        else
            echo "DualDeck: auto-update failed, continuing with the current version" >&2
        fi
    fi
fi

# shellcheck source=scripts/lib/ensure-packages.sh
source ./ensure-packages.sh

# libopenh264-7/openh264/openh264 and libyuv0/libyuv/libyuv (2026-08-26
# latency-audit follow-up): real, ship-blocking gap this closes -- unlike
# every other optional-at-configure-time dependency in this project
# (X11/Wayland/OpenH264/libyuv itself at *build* time), dualdeck-client
# is never passed through bundle_library_dependencies() the way
# dualdeck-host-service is (see this file's own bundle_library_dependencies
# call and its "host/internal/lib ships its runtime deps" comment) -- only
# SDL3 is bundled alongside it; everything else, this list included, is
# expected to already be on the system or auto-installed here. Since the
# "build" ensure_packages list above already includes libopenh264-dev
# (H.264 protocol v13 support) and now libyuv-dev too (SIMD BGRA<->I420
# conversion, see that list's own comment), any CI-built dualdeck-client
# binary dynamically links against both .so files unconditionally --
# optional-at-*configure*-time only means "may not be compiled in," not
# "safe to be missing once it is." Without runtime packages for both here,
# every dualdeck-client launch on a real Deck lacking them system-wide
# would fail outright with a missing-shared-object error before even
# reaching main() -- not just "H.264 doesn't work," the whole client,
# JPEG included. Fedora's package names are best-effort/unverified the
# same way openh264-devel's own dnf name already is above (same Cisco-
# repo caveat); libyuv0/libyuv's names are directly confirmed present in
# their distro's standard repos, same as libyuv-dev's own build-time entry.
ensure_packages "client runtime" \
    "libx11-6 libxext6 libxrandr2 libxcursor1 libxfixes3 libxi6 libxss1 libwayland-client0 libwayland-cursor0 libwayland-egl1 libxkbcommon0 libdrm2 libgbm1 libdecor-0-0 libturbojpeg0 libopenh264-7 libyuv0" \
    "libX11 libXext libXrandr libXcursor libXfixes libXi libXScrnSaver libwayland-client libwayland-cursor libwayland-egl libxkbcommon libdrm mesa-libgbm libdecor turbojpeg openh264 libyuv" \
    "libx11 libxext libxrandr libxcursor libxfixes libxi libxss wayland libxkbcommon libdrm mesa libdecor libjpeg-turbo openh264 libyuv" \
    || echo "warning: could not verify/install client runtime libraries automatically; continuing anyway in case they're already present" >&2

# Read by main.cpp so the host can reject a connection from a client
# running a different, incompatible release -- see protocol.h's
# HelloPayload::appVersion and net_server.cpp's comparison logic.
# dirname(client_root) matches check-for-updates.sh's own VERSION lookup
# (the archive root, or the central install directory's parent).
export DUALDECK_VERSION="$(cat "$(dirname "${client_root}")/VERSION" 2>/dev/null || true)"

LD_LIBRARY_PATH="${client_root}/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec "${client_root}/dualdeck-client" "$@"
