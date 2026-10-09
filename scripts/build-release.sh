#!/usr/bin/env bash
# Builds a complete downloadable release package from scratch: the
# patched melonDS host binary and the SDL3 client binary, packaged
# together with docs and wrapper scripts into one tar.gz. Used by
# .github/workflows/release.yml to publish a uniquely-tagged GitHub
# Release whenever that workflow is run manually, and safe to run
# locally the same way.
#
# Build-time dependencies are detected and installed automatically (apt/
# dnf/pacman) -- see ensure_packages() below. No manual `apt install`
# needed before running this.
set -euo pipefail

# Real hardware finding, 2026-08-03: SDL_GetNumGamepadTouchpads() has
# always returned 0 for the Steam Deck's own built-in controller,
# independent of every Steam Input change tried -- confirmed by reading
# SDL's own git history directly (github.com/libsdl-org/SDL):
# src/joystick/hidapi/SDL_hidapi_steamdeck.c has zero touchpad-related
# code in every 3.2.x release, checked 3.2.16 through 3.2.30 (the last
# 3.2.x release). Steam Controller/Deck touchpad support (PR #15528,
# "Add Steam Controller touchpads, capacitive touch for sticks, and grip
# sense", plus several touchpad-specific bugfixes merged afterward, e.g.
# "Fix touchpad finger detection on Steam Deck") only landed starting in
# the 3.4.x release series -- release-3.4.0 has partial support,
# release-3.4.12 has the full set of touchpad fixes. Every Host Control
# touchpad fix this project shipped before this one (SDL gamepad-
# touchpad capture, Steam Input disable, the CWD path bug, the Steam
# auto-restart-on-toggle fix) was correct but could never have worked at
# all against the previously-pinned 3.2.16 -- SDL itself never exposed
# any touchpad data for this controller in that version, regardless of
# anything client- or host-side.
SDL3_TAG="release-3.4.12"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/lib/pinned_commits.sh
source "${repo_root}/scripts/lib/pinned_commits.sh"
work_dir="${BUILD_RELEASE_WORKDIR:-$(mktemp -d)}"
out_dir="${BUILD_RELEASE_OUTPUT_DIR:-${repo_root}/release-out}"
mkdir -p "${work_dir}" "${out_dir}"

# shellcheck source=scripts/lib/ensure-packages.sh
source "${repo_root}/scripts/lib/ensure-packages.sh"
# shellcheck source=scripts/lib/build_emulator.sh
source "${repo_root}/scripts/lib/build_emulator.sh"
# shellcheck source=scripts/lib/appimage_pack.sh
source "${repo_root}/scripts/lib/appimage_pack.sh"
# shellcheck source=scripts/lib/apprun_templates.sh
source "${repo_root}/scripts/lib/apprun_templates.sh"

echo "== [0/6] Checking build dependencies =="
# qt6-qtbase-private-devel (dnf): needed for Azahar's Qt6::GuiPrivate
# requirement, same reason qt6-base-private-dev is in the apt list --
# missing here for a while (the apt/dnf lists had silently drifted
# apart), confirmed by a real Fedora build failing to configure Azahar
# ("Failed to find required Qt component GuiPrivate") until this was
# added. See docs/history.md's real-world verification entry.
# qt6-wayland/qt6-qtwayland: real user report, 2026-08-01 -- neither
# melonDS nor Azahar (both Qt6 apps) could start at all on a real
# Bazzite/Fedora HTPC with no system Qt6 GUI stack installed
# ("Could not find the Qt platform plugin wayland/xcb in \"\"", followed
# by an abort). bundle_library_dependencies() (appimage_pack.sh) only
# ever bundles ldd-reported *linked* dependencies -- Qt's platform
# plugins (libqxcb.so, the Wayland ones) are dlopen()'d at runtime based
# on QT_PLUGIN_PATH, invisible to ldd, so they were never bundled at
# all, silently relying on the host having a working system Qt6 install
# -- which most target HTPC machines don't. Cemu (wxWidgets/GTK, not
# Qt) was unaffected, confirming the diagnosis. qt6-base-dev alone only
# provides the xcb platform plugin on Debian/Ubuntu -- Wayland-native
# support needs this separate package, added here so
# find_qt6_plugins_dir()/pack_appimage()'s bundled platforms/ directory
# (see the "Packaging prebuilt AppImages" step below) covers both
# session types rather than relying on XWayland compatibility alone.
# libxtst-dev/libXtst-devel/libxtst (X11 XTEST extension headers): new
# requirement as of bumping SDL3_TAG to release-3.4.12 (see that
# variable's own comment) -- SDL's X11 backend started hard-requiring it
# at configure time somewhere between 3.2.16 and 3.4.12 ("Couldn't find
# dependency package for XTEST"), where 3.2.16 built fine without it.
# libopenh264-dev/openh264-devel/openh264 (protocol v13's optional H.264
# video codec, host/remote-server/src/h264_encoder.cpp -- see
# docs/history.md's 2026-08-25 video-codec-negotiation entry):
# purely opt-in at configure time, same as X11/Wayland above, never
# FATAL_ERROR like TurboJPEG -- a build without it just never gets H.264
# capability, every session still runs JPEG. apt's name is confirmed
# (Ubuntu ships it in universe); dnf/pacman names are best-effort and
# unverified the same way this file's own Azahar Vulkan/Boost packages
# already are below -- Fedora in particular has historically kept
# H.264-capable codecs out of its own repos over patent licensing, even
# though OpenH264's source itself is BSD and Cisco's royalty coverage is
# specifically what makes that a non-issue for an *official* OpenH264
# build (not necessarily a distro's own rebuild) -- so `dnf install
# openh264-devel` may need Fedora's separate Cisco-hosted repo enabled,
# not just the base repos this script otherwise assumes.
# libyuv-dev/libyuv-devel/libyuv (2026-08-26 latency-audit follow-up):
# SIMD-accelerated BGRA<->I420 conversion for the H.264 path (host/
# remote-server/src/h264_encoder.cpp's bgraToI420()/client/src/
# h264_decoder.cpp's i420ToBgra() -- see top-level CMakeLists.txt's
# LibYuv::LibYuv detection and each function's own comment). Optional at
# configure time like openh264-dev above -- a build without it keeps the
# existing hand-rolled scalar conversion, just without libyuv's real
# measured speedup (see tools/codec-benchmark). Unlike openh264-devel,
# this isn't a patent-sensitive codec (a plain color-conversion library),
# so all three package names are directly confirmed present in their
# distro's standard repos (Fedora's packages.fedoraproject.org lists
# libyuv-devel; Arch's `extra` repo carries libyuv) rather than best-
# effort/unverified the way openh264-devel's Fedora name is above.
# Without this, bundle_library_dependencies() (appimage_pack.sh) simply
# never finds a libyuv.so to bundle -- ldd only reports what the binary
# actually links, so a prebuilt release built before this package was
# added here would silently ship the scalar fallback despite this
# project's own code supporting the SIMD path.
# perl-FindBin/perl-IPC-Cmd (dnf only): real Fedora build failure,
# 2026-08-26 -- Cemu's vcpkg dependency graph builds OpenSSL from source,
# whose own build tooling is Perl-based and needs FindBin.pm and
# IPC::Cmd, both Perl *core* modules (ship with every upstream Perl) but
# ones Fedora's own minimal `perl` package splits out into separate
# optional packages rather than including by default -- unlike Debian/
# Ubuntu's `perl` (apt) and Arch's `perl` (pacman), both of which include
# the full core module set already, so this is dnf-only.
ensure_packages "build" \
    "cmake extra-cmake-modules ninja-build build-essential git python3 libcurl4-gnutls-dev libpcap0.8-dev libsdl2-dev libarchive-dev libenet-dev libzstd-dev libfaad-dev qt6-base-dev qt6-base-private-dev qt6-multimedia-dev qt6-svg-dev qt6-wayland libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev libwayland-dev libxkbcommon-dev libdrm-dev libgbm-dev libdecor-0-dev libturbojpeg0-dev libdbus-1-dev libpipewire-0.3-dev libopenh264-dev libyuv-dev" \
    "cmake extra-cmake-modules ninja-build gcc-c++ git python3 libcurl-devel libpcap-devel SDL2-devel libarchive-devel enet-devel libzstd-devel faad2-devel qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtmultimedia-devel qt6-qtsvg-devel qt6-qtwayland libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXfixes-devel libXi-devel libXScrnSaver-devel libXtst-devel wayland-devel libxkbcommon-devel libdrm-devel mesa-libgbm-devel libdecor-devel turbojpeg-devel dbus-devel pipewire-devel openh264-devel libyuv-devel perl-FindBin perl-IPC-Cmd" \
    "cmake extra-cmake-modules ninja base-devel git python curl libpcap sdl2 libarchive enet zstd faad2 qt6-base qt6-multimedia qt6-svg qt6-wayland libx11 libxext libxrandr libxcursor libxfixes libxi libxss libxtst wayland libxkbcommon libdrm mesa libdecor libjpeg-turbo dbus libpipewire openh264 libyuv"

# Azahar (3DS) additionally needs a Vulkan SDK and Boost headers beyond
# melonDS's own dependency list above -- see
# docs/azahar-integration-analysis.md for why (real 3D rendering, unlike
# the DS's largely-2D workload). Debian package names verified against
# this exact sandbox; Fedora/Arch names are best-effort and unverified
# (see docs/history.md's AzaharAdapter entry).
ensure_packages "azahar build" \
    "libvulkan-dev libboost-dev libboost-iostreams-dev libboost-thread-dev libpulse-dev libasound2-dev" \
    "vulkan-loader-devel vulkan-headers boost-devel pulseaudio-libs-devel alsa-lib-devel" \
    "vulkan-headers vulkan-icd-loader boost pulseaudio alsa-lib"

# Cemu (Wii U) additionally needs the packages its own BUILD.md lists
# beyond melonDS's/Azahar's dependency lists above -- wxWidgets is
# fetched and built by Cemu's own vcpkg submodule, not from a system
# package, so it isn't listed here. libusb-1.0-0-dev works around a
# known vcpkg-hidapi build issue BUILD.md calls out explicitly. Debian
# package names taken directly from BUILD.md; Fedora/Arch names are
# best-effort and unverified -- see host/cemu-patches/README.md's "What
# is not verified yet" section (this project's sandbox cannot reach the
# hosts Cemu's vcpkg-based dependency graph needs, so this whole step
# has only ever been reasoned through, never actually run here).
# zlib-ng-compat-static (dnf only): real Fedora 44 build failure,
# 2026-10-09 -- Fedora's zlib-ng-compat-devel ships a ZLIBConfig.cmake
# whose ZLIB::ZLIBSTATIC imported target points at /usr/lib64/libz.a,
# but that file is only in the separate -static package, so vcpkg's
# tiff port (and anything else whose find_package(ZLIB) lands on that
# config) failed to configure with "references the file libz.a but this
# file does not exist." Debian/Arch don't split it this way.
# libpng-static: identical failure one step later, in Cemu's own
# find_package(PNG) -- Fedora's PNGConfig.cmake declares
# PNG::png_static pointing at /usr/lib64/libpng16.a, only shipped in
# libpng-static.
ensure_packages "cemu build" \
    "freeglut3-dev libbluetooth-dev libgcrypt20-dev libglm-dev libgtk-3-dev libpulse-dev libsecret-1-dev libsystemd-dev libtool nasm libusb-1.0-0-dev" \
    "freeglut-devel bluez-libs-devel libgcrypt-devel glm-devel gtk3-devel pulseaudio-libs-devel libsecret-devel systemd-devel libtool nasm libusb1-devel perl-IPC-Cmd zlib-ng-compat-static libpng-static" \
    "freeglut bluez-libs libgcrypt glm gtk3 libpulse libsecret systemd libtool nasm libusb"

# sccache (github.com/mozilla/sccache), if present on PATH -- installed
# by .github/workflows/release.yml's mozilla-actions/sccache-action step
# in CI; simply absent for a typical local run, where this falls back to
# no compiler launcher at all. Speeds up rebuilding after a patch change
# by caching object files keyed on preprocessed source content rather
# than on whether the same build directory happens to still be on disk:
# without this, every Azahar/Cemu patch iteration recompiles all ~500+
# translation units from scratch even though a patch usually touches
# only a handful of files.
cmake_launcher_args=()
melonds_bin='' azahar_bin='' cemu_bin='' # set by build_melonds/azahar/cemu via nameref below
if command -v sccache >/dev/null 2>&1; then
    echo "sccache found on PATH, enabling as CMake compiler launcher"
    cmake_launcher_args=(-DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache)
fi

# vcpkg's openssl port shells out to system Perl for its build (not
# Cemu-specific -- any vcpkg-based project hits this the same way), and
# recent Perl versions (5.40+, confirmed against Fedora 42's Perl 5.42)
# removed IPC::Cmd from the core distribution -- it now needs a real
# package. Confirmed via a real local build failure (not just read from
# docs): "Can't locate IPC/Cmd.pm in @INC ... Perl cannot find IPC::Cmd"
# aborting vcpkg's openssl:x64-linux port build entirely. Debian/Ubuntu
# and Arch's own `perl` packages still bundle it as of this writing (no
# separate package needed there); only Fedora's `perl-IPC-Cmd` is added
# above. If this starts failing on Debian/Arch too in the future, add
# their IPC::Cmd packages here the same way.

sdl3_src="${work_dir}/sdl3-src"
sdl3_install="${work_dir}/sdl3-install"

echo "== [1/6] SDL3 (${SDL3_TAG}) =="
# Real user report, 2026-08-03 (the parallel Cemu version bug, see
# release.yml's Cemu cache step comment for the full account): a "cache
# hit" check that only asks "does the install directory already exist"
# -- with no check of *what* was actually built there -- silently keeps
# serving a stale build forever once SDL3_TAG changes, regardless of
# how many times the pinned tag is bumped in this file, because
# release.yml's own actions/cache key is a hardcoded literal
# ("sdl3-release-3.2.16-...") that has to be remembered and bumped by
# hand in lockstep -- easy to forget, exactly as it was forgotten here
# once already. Writing (and checking) this marker file is the local,
# root-cause half of the fix: even if a future SDL3_TAG bump forgets to
# also bump release.yml's cache key, this check still catches the
# mismatch and rebuilds for real, rather than silently trusting a
# same-named-but-wrong-tag cached directory.
# -DCMAKE_INSTALL_LIBDIR=lib below: real Fedora 44 report, 2026-10-09 --
# GNUInstallDirs defaults to lib64/ on Fedora, so the cache check here
# (and the packaging step's `cp ${sdl3_install}/lib/libSDL3.so*`) only
# ever looked in lib/, rebuilding SDL3 on every run and failing at
# packaging. Pinning the libdir keeps every distro on the same layout.
sdl3_tag_marker="${sdl3_install}/.dualdeck-sdl3-tag"
if [[ -f "${sdl3_install}/lib/cmake/SDL3/SDL3Config.cmake" ]] && \
   [[ "$(cat "${sdl3_tag_marker}" 2>/dev/null)" == "${SDL3_TAG}" ]]; then
    echo "already built at ${sdl3_install} (tag ${SDL3_TAG}), skipping (cache hit)"
else
    rm -rf "${sdl3_src}" "${sdl3_install}"
    git clone --depth 1 --branch "${SDL3_TAG}" https://github.com/libsdl-org/SDL.git "${sdl3_src}"
    cmake -S "${sdl3_src}" -B "${sdl3_src}/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${sdl3_install}" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF \
        "${cmake_launcher_args[@]}"
    cmake --build "${sdl3_src}/build" -j"$(nproc)"
    cmake --install "${sdl3_src}/build"
    echo "${SDL3_TAG}" > "${sdl3_tag_marker}"
fi

pyrowave_src="${work_dir}/pyrowave-src"
pyrowave_install="${work_dir}/pyrowave-install"

echo "== [1/6] PyroWave (commit ${PYROWAVE_COMMIT}) =="
# Optional video codec -- see host/remote-server/include/host/
# pyrowave_encoder.h. Built from source (no distro packages it) into its
# own prefix, then handed to this repo's CMake configure via
# PKG_CONFIG_PATH below, where top-level CMakeLists.txt's
# pkg_check_modules(pyrowave-shared) picks it up. Its Vulkan-header
# build dependency is already covered by the "azahar build"
# ensure_packages list above; its shaders ship precompiled in its own
# repo, so no shader compiler is needed. libpyrowave-shared.so ends up
# in host/internal/lib/ and client/lib/ the same way every other linked
# library does -- bundle_library_dependencies() follows the binaries'
# build-tree RPATH to it.
#
# Deliberately non-fatal: PyroWave is opt-in and every session can fall
# back to JPEG/H.264, so a PyroWave build break (e.g. upstream's Granite
# checkout becoming unreachable) ships a release without PyroWave and a
# loud warning, instead of blocking the whole release. Same marker-file
# cache check as SDL3's above, for the same stale-cache reason.
pyrowave_commit_marker="${pyrowave_install}/.dualdeck-pyrowave-commit"
pyrowave_pkgconfig_dir=""
if [[ -f "${pyrowave_install}/lib/pkgconfig/pyrowave-shared.pc" ]] && \
   [[ "$(cat "${pyrowave_commit_marker}" 2>/dev/null)" == "${PYROWAVE_COMMIT}" ]]; then
    echo "already built at ${pyrowave_install} (commit ${PYROWAVE_COMMIT}), skipping (cache hit)"
    pyrowave_pkgconfig_dir="${pyrowave_install}/lib/pkgconfig"
# An explicit && chain rather than `set -e` inside the subshell: bash
# ignores errexit for anything evaluated as an if/elif condition,
# subshells included, so `set -e` there would silently report a failed
# build as success.
elif (
    rm -rf "${pyrowave_src}" "${pyrowave_install}" &&
    git clone https://github.com/Themaister/pyrowave.git "${pyrowave_src}" &&
    cd "${pyrowave_src}" &&
    git checkout "${PYROWAVE_COMMIT}" &&
    bash checkout_granite.sh &&
    cmake -S "${pyrowave_src}" -B "${pyrowave_src}/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${pyrowave_install}" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        "${cmake_launcher_args[@]}" &&
    cmake --build "${pyrowave_src}/build" -j"$(nproc)" &&
    cmake --install "${pyrowave_src}/build" &&
    [[ -f "${pyrowave_install}/lib/pkgconfig/pyrowave-shared.pc" ]] &&
    echo "${PYROWAVE_COMMIT}" > "${pyrowave_commit_marker}"
); then
    pyrowave_pkgconfig_dir="${pyrowave_install}/lib/pkgconfig"
else
    echo "WARNING: PyroWave build failed -- this release will be built WITHOUT PyroWave video support" >&2
    echo "         (JPEG/H.264 unaffected). See the output above for why." >&2
    rm -rf "${pyrowave_install}"
fi

echo "== [2/6] Patched melonDS host (commit ${MELONDS_COMMIT}) =="
build_melonds melonds_bin "${work_dir}" "${repo_root}" "${MELONDS_COMMIT}"

echo "== [3/6] Patched Azahar host (Nintendo 3DS, commit ${AZAHAR_COMMIT}) =="
# See docs/azahar-integration-analysis.md and
# docs/adr/0001-host-service-and-adapter-architecture.md's AzaharAdapter
# section for what this patch actually does. This is a much heavier
# build than melonDS's (36 git submodules -- Vulkan, boost, dynarmic,
# spirv-tools, etc. -- for real 3D emulation instead of the DS's mostly
# software-rendered 2D), so it's cached across runs by commit + patch
# hash (see build_azahar() in scripts/lib/build_emulator.sh -- shared
# with scripts/emudeck-replace-in-place.sh so both use the exact same
# cache-hit/patch-correctness logic, not two silently-drifting copies of
# it, after a real bug once shipped two releases with byte-for-byte
# identical azahar binaries despite the patch changing in between,
# because an earlier version of this check only tested file existence).
build_azahar azahar_bin "${work_dir}" "${repo_root}" "${AZAHAR_COMMIT}"

echo "== [4/6] Patched Cemu host (Nintendo Wii U, commit ${CEMU_COMMIT}) =="
# See host/cemu-patches/README.md and docs/history.md's
# 2026-07-22 Cemu entry for what this patch does and, importantly, what
# has and hasn't been verified -- this patch was written entirely from
# reading Cemu's source, never compiled in this project's own
# development sandbox (Cemu's vcpkg-based dependency graph needs
# unrestricted internet access that sandbox doesn't have). This is the
# first time it's actually being built, on whatever machine runs this
# script -- expect this step to need troubleshooting the first few
# times it runs for real (see Cemu's own BUILD.md "Troubleshooting
# Steps" section for common vcpkg issues). Cached the same way Azahar's
# step above is -- see build_cemu() in scripts/lib/build_emulator.sh.
build_cemu cemu_bin "${work_dir}" "${repo_root}" "${CEMU_COMMIT}" "${CEMU_VERSION_MAJOR}" "${CEMU_VERSION_MINOR}"

echo "== [5/6] Client + host prototype (this repo) =="
repo_build="${work_dir}/repo-build"
PKG_CONFIG_PATH="${pyrowave_pkgconfig_dir}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}" \
cmake -S "${repo_root}" -B "${repo_build}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DDUALDECK_BUILD_CLIENT=ON \
    -DDUALDECK_BUILD_HOST=ON -DDUALDECK_BUILD_HOST_UI=ON -DCMAKE_PREFIX_PATH="${sdl3_install}" \
    "${cmake_launcher_args[@]}"
cmake --build "${repo_build}" -j"$(nproc)"
ctest --test-dir "${repo_build}" --output-on-failure

echo "== [6/6] Packaging =="
commit_short="$(cd "${repo_root}" && git rev-parse --short HEAD)"
branch_name="$(cd "${repo_root}" && git rev-parse --abbrev-ref HEAD)"
# Set by .github/workflows/release.yml to the actual published tag
# (vX.Y.<run number>); falls back to a "dev-<commit>" placeholder for
# local runs outside CI, where there's no real release tag yet.
version_tag="${RELEASE_VERSION_TAG:-dev-${commit_short}}"
pkg_name="melonds-remote-${commit_short}-linux-x86_64"
pkg_dir="${work_dir}/${pkg_name}"
rm -rf "${pkg_dir}"
# host/ and client/ are each laid out the same way: the one script a
# user actually needs to double-click sits directly inside, alongside
# the binary it launches; everything else (install/uninstall scripts,
# shared helpers, the Distrobox path, etc.) lives one level down in
# internal/, out of the way. Both directories are fully self-contained
# (their own internal/ensure-packages.sh and internal/steam_shortcut.py
# copies) -- there's no shared top-level scripts/ directory in the
# archive at all, so host/ or client/ alone is always everything that
# directory needs.
mkdir -p "${pkg_dir}/host/internal" "${pkg_dir}/client/lib" "${pkg_dir}/client/internal" "${pkg_dir}/docs"

# Read by check-for-updates.sh below to compare against the latest
# published GitHub release.
echo "${version_tag}" > "${pkg_dir}/VERSION"

cp "${melonds_bin}" "${pkg_dir}/host/melonDS"
chmod +x "${pkg_dir}/host/melonDS"
ldd "${melonds_bin}" | awk '{print $1}' | sort -u \
    > "${pkg_dir}/host/internal/host-shared-library-dependencies.txt"

# Azahar (Nintendo 3DS, GitHub issue #4 follow-up) -- top-level
# alongside melonDS, matching how "one binary per double-clickable
# emulator, everything else under internal/" already works for melonDS.
# See host/internal/run-host-azahar.sh for how it's actually launched
# (always as an out-of-process adapter -- see that script's own comment
# for why).
cp "${azahar_bin}" "${pkg_dir}/host/azahar"
chmod +x "${pkg_dir}/host/azahar"
ldd "${azahar_bin}" | awk '{print $1}' | sort -u \
    > "${pkg_dir}/host/internal/azahar-shared-library-dependencies.txt"

# Cemu (Nintendo Wii U) -- top-level alongside melonDS/azahar, same
# "one binary per double-clickable emulator" layout. See
# host/internal/run-host-cemu.sh for how it's actually launched (always
# as an out-of-process adapter, same as Azahar -- see host/cemu-patches/
# README.md for why Cemu has no in-process device-approval path).
cp "${cemu_bin}" "${pkg_dir}/host/cemu"
chmod +x "${pkg_dir}/host/cemu"
ldd "${cemu_bin}" | awk '{print $1}' | sort -u \
    > "${pkg_dir}/host/internal/cemu-shared-library-dependencies.txt"

# Cemu's build places static `resources/`/`gameProfiles/` directories
# directly alongside Cemu_release in its own bin/ (confirmed by its
# CMakeLists.txt's macOS packaging step, which explicitly copies
# `${CMAKE_SOURCE_DIR}/bin/{gameProfiles,resources}` into the .app bundle
# for exactly this reason -- Linux never needed an equivalent copy step
# because the binary already lands in that directory) -- Cemu expects
# both to sit next to its own executable at runtime and can't fully
# initialize its GUI without them. cemu_bin's own directory
# (`${cemu_src}/bin`, from build_cemu()) still has both as siblings at
# this point, so they're copied from there, not re-derived. See
# pack_appimage()'s matching fix (extra_dirs) for the AppImage path
# below, and docs/history.md for the real-hardware repro that
# caught this being silently dropped everywhere Cemu was packaged.
cemu_bin_dir="$(dirname "${cemu_bin}")"
cp -a "${cemu_bin_dir}/resources" "${pkg_dir}/host/resources"
cp -a "${cemu_bin_dir}/gameProfiles" "${pkg_dir}/host/gameProfiles"

# The standalone Host Service binary (GitHub issue #4): not used by the
# default launch path (melonDS still runs its own in-process server, see
# EmuInstance::startRemoteServer()), but required for run-host.sh's
# opt-in "host-control mode" -- without shipping this, that mode could
# never actually be used from a downloaded release, only from a source
# build.
cp "${repo_build}/host/remote-server/dualdeck-host-service" "${pkg_dir}/host/internal/dualdeck-host-service"
chmod +x "${pkg_dir}/host/internal/dualdeck-host-service"

# Real Bazzite hardware report, 2026-08-02: starting the persistent Host
# Control daemon failed with "Unit dualdeck-host-control.service could
# not be found" -- traced back to this binary silently depending on a
# host-provided libturbojpeg (host/remote-server/CMakeLists.txt links
# TurboJPEG::TurboJPEG, a dynamic library found via find_library(), not
# statically linked -- the comment that used to be here claiming "no
# extra runtime library dependencies" was simply wrong), which an
# immutable/rpm-ostree system like Bazzite has no easy way to install
# system-wide. That in turn is why install-host-distrobox.sh and
# install-host-control-daemon.sh both used to refuse Host Control mode
# outright on immutable systems (see their own comments) -- routing
# through a Distrobox container was the only way to guarantee
# libturbojpeg was present. Bundling this one binary's shared library
# dependencies the exact same way pack_appimage() already does for the
# prebuilt Azahar/Cemu AppImages (bundle_library_dependencies(), sourced
# above) removes that dependency entirely: dualdeck-host-service needs no
# Qt/SDL (host/remote-server/CMakeLists.txt links only
# dualdeck_protocol/dualdeck_adapter_sdk/Threads/TurboJPEG), so once its
# one extra library is bundled alongside it, it runs identically on any
# Linux host regardless of package-manager mutability -- see
# run-host.sh's Host-Control branch, host-control-daemon.sh, and
# scripts/lib/adapter_socket_probe.sh for where LD_LIBRARY_PATH now
# points at this directory.
bundle_library_dependencies "${pkg_dir}/host/internal/dualdeck-host-service" "${pkg_dir}/host/internal/lib"

# The full-screen host menu window dualdeck-host.sh drives (host/ui/).
# Its libraries (SDL3 and what it links) go in their own ui-lib/ rather
# than lib/, so they never end up on the host service's or an
# emulator's LD_LIBRARY_PATH; dualdeck-host.sh points only the window at
# it. SDL3 is copied from the build prefix afterwards, as for the client.
cp "${repo_build}/host/ui/dualdeck-host-ui" "${pkg_dir}/host/internal/dualdeck-host-ui"
chmod +x "${pkg_dir}/host/internal/dualdeck-host-ui"
bundle_library_dependencies "${pkg_dir}/host/internal/dualdeck-host-ui" "${pkg_dir}/host/internal/ui-lib"
cp -a "${sdl3_install}"/lib/libSDL3.so* "${pkg_dir}/host/internal/ui-lib/"
# The fonts compiled into it are OFL-licensed; their license travels too.
cp "${repo_root}"/host/ui/fonts/OFL-*.txt "${pkg_dir}/docs/"

# Prebuilt, patched, self-contained AppImages for
# emudeck-replace-in-place.sh to download and drop straight into an
# existing EmuDeck install (see docs/history.md's 2026-08-01
# "prebuilt AppImages" entry for the full story). This tool used to
# clone/patch/compile melonDS/Azahar/Cemu on the *user's own machine*
# on every run -- Distrobox, vcpkg, and glibc/Qt ABI mismatches between
# that machine and wherever the resulting AppImage got launched turned
# out to be an enormous, repeated source of real-world failures (a
# Steam-inherited LD_PRELOAD corrupting unrelated subprocess output,
# missing kernel headers, missing runtime libraries), none of which
# have anything to do with this project's own patches. CI already
# builds all three from source successfully on every release (a
# controlled, Steam-free, consistently-provisioned environment) -- so
# building the actual installable artifact here too, once, and letting
# every user's machine just download it, eliminates that entire class
# of problem outright rather than chasing it fix by fix. bundle_library_
# dependencies() (see appimage_pack.sh) makes each AppImage genuinely
# self-contained regardless of what glibc/Qt version the target host
# happens to have.
echo "== Packaging prebuilt AppImages (melonDS/Azahar/Cemu) for emudeck-replace-in-place.sh =="

# Real user report, 2026-08-01: neither melonDS nor Azahar (both Qt6
# apps) could start at all on a real Bazzite/Fedora HTPC with no system
# Qt6 GUI stack installed -- "Could not find the Qt platform plugin
# wayland/xcb in ''", then an abort. bundle_library_dependencies() only
# ever bundles ldd-reported *linked* dependencies; Qt's platform plugins
# are dlopen()'d at runtime based on QT_PLUGIN_PATH, invisible to ldd,
# so they were never bundled at all -- silently relying on the host
# having a working system Qt6 install, which most target HTPC machines
# don't. Cemu (wxWidgets/GTK, not Qt) was unaffected, confirming the
# diagnosis. Staged once here and reused for both AppImages below; lands
# at AppDir/usr/bin/platforms/ via extra_dirs (matching where the AppRun
# templates below point QT_PLUGIN_PATH), the same mechanism already
# proven for Cemu's resources/gameProfiles above.
qt6_plugins_dir="$(find_qt6_plugins_dir)" || {
    echo "error: could not locate Qt6's plugins directory (platforms/libqxcb.so) on this" >&2
    echo "build machine -- install qt6-base-dev (or your distro's equivalent) first." >&2
    exit 1
}
# Real user report, 2026-08-01: even after platforms/ was bundled (the
# entry above -- fixes the outright abort), melonDS/Azahar both launched
# with no crash, a working NetServer, and a working client video stream,
# but produced literally no window at all on the host. The QPA platform
# plugin (libqxcb.so/libqwayland-egl.so, both in platforms/) is only
# *one* of several plugin categories Qt needs to actually create and map
# a GL-backed toplevel window -- xcbglintegrations/ (GLX/EGL integration
# for xcb) and, on Wayland, wayland-shell-integration/ (the xdg-shell
# protocol code that actually tells the compositor to map the window),
# wayland-decoration-client/, and wayland-graphics-integration-client/
# (the EGL/Wayland buffer backend). None of those were ever bundled --
# unlike a missing platforms/ plugin, a missing plugin in *these*
# categories doesn't make Qt abort at all: it just silently never maps a
# window, while the OpenGL context and framebuffer still exist enough for
# GLBottomScreenCapture/AdapterBridge's own frame-grabbing (which reads
# the render target directly, not the composited window) to keep working
# -- exactly matching the report ("client sees video, host sees nothing,
# no crash, no error"). xcbglintegrations is required unconditionally
# (X11/XWayland always needs it for a GL-backed window); the three
# wayland-* categories are staged only if this build machine's Qt install
# has them (a build machine without qt6-wayland installed simply won't
# have these directories -- degrades to "Wayland sessions may still not
# show a window," not a hard build failure, since XCB/XWayland is enough
# to fix the reported bug either way).
qt_plugin_staging_root="${work_dir}/qt6-plugins"
qt_plugin_extra_dirs=""
# "multimedia" added 2026-08-03: real user report, a hard abort
# ("could not load multimedia backend \"\"" / "QtMultimedia is not
# currently supported on this platform or compiler") hitting Azahar's
# Configure dialog (its Camera tab uses QtMultimedia for webcam-as-3DS-
# camera capture; some builds also probe it for audio device
# enumeration). Same root cause as platforms/xcbglintegrations/wayland-*
# above -- Qt's multimedia backend is its own dlopen()'d plugin
# (typically an FFmpeg- or GStreamer-backed .so under plugins/
# multimedia/), invisible to bundle_library_dependencies()'s ldd-based
# scan of the main binary, so it was never bundled and QtMultimedia had
# nothing to load at all on a host without a system Qt6 install. Kept
# optional (soft warning, not a hard build failure) like the wayland-*
# categories: the 3DS camera feature this backs is a rarely-used
# secondary feature, not core functionality worth blocking the whole
# build over on a machine that happens to lack qt6-multimedia-dev.
for qt_plugin_category in platforms xcbglintegrations wayland-shell-integration \
    wayland-decoration-client wayland-graphics-integration-client multimedia; do
    if [[ -d "${qt6_plugins_dir}/${qt_plugin_category}" ]]; then
        mkdir -p "${qt_plugin_staging_root}"
        cp -a "${qt6_plugins_dir}/${qt_plugin_category}" "${qt_plugin_staging_root}/${qt_plugin_category}"
        qt_plugin_extra_dirs="${qt_plugin_extra_dirs:+${qt_plugin_extra_dirs}:}${qt_plugin_staging_root}/${qt_plugin_category}"
    elif [[ "${qt_plugin_category}" == "xcbglintegrations" ]]; then
        echo "warning: this build machine's Qt6 install has no xcbglintegrations plugin --" >&2
        echo "packaged melonDS/Azahar may fail to create a GL-backed window over X11/XWayland." >&2
    elif [[ "${qt_plugin_category}" == "multimedia" ]]; then
        echo "warning: this build machine's Qt6 install has no multimedia plugin -- Azahar's" >&2
        echo "Camera configuration tab may abort instead of opening on a host with no system" >&2
        echo "Qt6 multimedia stack (install qt6-multimedia-dev or your distro's equivalent)." >&2
    fi
done

melonds_apprun="${work_dir}/AppRun-melonds"
generate_apprun_melonds "${melonds_apprun}" "${version_tag}"
# dualdeck-host-service bundled too (like Azahar/Cemu below): the AppRun
# now runs melonDS out-of-process through it by default -- see
# generate_apprun_melonds()'s own comment.
pack_appimage "${melonds_bin}" "${out_dir}/dualdeck-melonds-patched-linux-x86_64.AppImage" \
    melonDS "${melonds_apprun}" "${repo_build}/host/remote-server/dualdeck-host-service" \
    "${qt_plugin_extra_dirs}"

azahar_apprun="${work_dir}/AppRun-azahar"
generate_apprun_out_of_process AZAHAR azahar azahar-apprun-adapter.sock "${azahar_apprun}"
pack_appimage "${azahar_bin}" "${out_dir}/dualdeck-azahar-patched-linux-x86_64.AppImage" \
    azahar "${azahar_apprun}" "${repo_build}/host/remote-server/dualdeck-host-service" \
    "${qt_plugin_extra_dirs}"

cemu_apprun="${work_dir}/AppRun-cemu"
generate_apprun_out_of_process CEMU cemu cemu-apprun-adapter.sock "${cemu_apprun}"
# pack_appimage() names the file inside AppDir/usr/bin/ after
# basename(binary_path), and the AppRun execs "usr/bin/cemu" literally
# (matching Azahar's build output, which happens to already be named
# "azahar") -- but Cemu's own build output is literally named
# Cemu_release, not "cemu", so it has to be staged under the right name
# first, same as emudeck-replace-in-place.sh used to do locally.
cemu_staged="${work_dir}/staged-cemu/cemu"
mkdir -p "$(dirname "${cemu_staged}")"
cp "${cemu_bin}" "${cemu_staged}"
# resources/gameProfiles (extra_dirs) land in AppDir/usr/bin/, alongside
# usr/bin/cemu -- see the "resources/gameProfiles" comment on the
# `host/cemu` copy above for why Cemu needs both there at runtime.
pack_appimage "${cemu_staged}" "${out_dir}/dualdeck-cemu-patched-linux-x86_64.AppImage" \
    cemu "${cemu_apprun}" "${repo_build}/host/remote-server/dualdeck-host-service" \
    "${cemu_bin_dir}/resources:${cemu_bin_dir}/gameProfiles"

# Shared scripts/lib/ helpers each side's internal/ scripts source or
# run. Bundled per side (not shared from a top-level folder) so host/
# and client/ each stay self-contained once installed. dualdeck_branch.sh
# backs Advanced -> Installation branch; release_install.sh holds the
# error trap and download/verify/extract steps.
for lib in ensure-packages.sh steam_shortcut.py steam_restart_helper.sh dualdeck_branch.sh release_install.sh \
           host_firewall.sh adapter_socket_probe.sh pipewire_env.sh; do
    cp "${repo_root}/scripts/lib/${lib}" "${pkg_dir}/host/internal/${lib}"
done

cp "${repo_build}/client/dualdeck-client" "${pkg_dir}/client/dualdeck-client"
chmod +x "${pkg_dir}/client/dualdeck-client"

# Bundle the client's own linked dependencies, exactly as the host
# service already does (see the dualdeck-host-service call earlier in
# this file). Real user report, 2026-08-04: the client would not start
# on a Bazzite *client* Deck because libturbojpeg.so.0 was missing --
# the client links it to decode incoming video frames (protocol v8's
# JPEG frames), but only libSDL3 was ever bundled, so everything else
# silently relied on the host distribution shipping it.
#
# That reliance cannot hold on an immutable system, which is the whole
# point: client/internal/ensure-packages.sh's dnf branch cannot install
# anything on an rpm-ostree base without a reboot, so "just install
# libjpeg-turbo" is not a fix available to the launcher at runtime. Same
# root cause as the melonDS libturbojpeg failure on the host side; this
# is the client half of it.
#
# Deliberately the shared bundler rather than another one-off `cp`:
# it already excludes Mesa/Vulkan/GL and libwayland-client/-egl/-cursor,
# each of which was added after a real breakage when bundled copies
# shadowed the host's own drivers. A hand-rolled copy here would have to
# rediscover all of that.
bundle_library_dependencies "${pkg_dir}/client/dualdeck-client" "${pkg_dir}/client/lib"

# After the bundler, so this stays the authoritative copy of the SDL3
# this client was actually built against rather than whatever ldd
# happened to resolve.
cp -a "${sdl3_install}"/lib/libSDL3.so* "${pkg_dir}/client/lib/"

# Same shared helpers as host/internal/ above, plus steam_input_config.py
# for the client-only touchpad-as-native-input experiment (see its module
# docstring).
for lib in ensure-packages.sh steam_shortcut.py steam_restart_helper.sh dualdeck_branch.sh release_install.sh \
           steam_input_config.py; do
    cp "${repo_root}/scripts/lib/${lib}" "${pkg_dir}/client/internal/${lib}"
done

# packaging/ mirrors the release layout, so it's copied as whole trees.
cp -a "${repo_root}/packaging/host/." "${pkg_dir}/host/"
cp -a "${repo_root}/packaging/client/." "${pkg_dir}/client/"
cp "${repo_root}/packaging/check-for-updates.sh" "${pkg_dir}/check-for-updates.sh"
find "${pkg_dir}/host" "${pkg_dir}/client" -name '*.sh' -exec chmod +x {} +
chmod +x "${pkg_dir}/check-for-updates.sh"

cp "${repo_root}/docs/building.md" "${repo_root}/docs/steam-deck-setup.md" \
   "${repo_root}/docs/bazzite-host-setup.md" "${repo_root}/docs/troubleshooting.md" \
   "${repo_root}/docs/known-limitations.md" "${repo_root}/docs/history.md" \
   "${repo_root}/docs/protocol.md" \
   "${pkg_dir}/docs/"
cp "${repo_root}/LICENSE" "${pkg_dir}/"
cp "${repo_root}/docs/release-readme.md" "${pkg_dir}/README.md"

commit_full="$(cd "${repo_root}" && git rev-parse HEAD)"
built_at="$(date -u +"%Y-%m-%d %H:%M UTC")"
cat > "${pkg_dir}/RELEASE_NOTES.md" <<NOTES
# DualDeck (\`${version_tag}\`)

Built from commit \`${commit_full}\` on branch \`${branch_name}\`,
${built_at}. This is a distinct, permanently-retained release -- it will
not be overwritten by a later build, so if this version has a problem,
just grab an earlier one from the Releases page instead.

Contains \`host/\` (melonDS, Nintendo DS, patched with
\`host/melonds-patches/0001-remote-server-integration.patch\` against
upstream commit \`${MELONDS_COMMIT}\`; and Azahar, Nintendo 3DS,
experimental, patched with
\`host/azahar-patches/0001-remote-server-integration.patch\` against
upstream commit \`${AZAHAR_COMMIT}\`) and \`client/\` (SDL3 Steam Deck
client, SDL3 ${SDL3_TAG} bundled in \`client/lib/\`). All are Linux
x86_64 binaries built on Ubuntu (GitHub Actions \`ubuntu-latest\` or
equivalent) -- see \`docs/known-limitations.md\` (bundled here) for what's
verified and portability notes.

See \`README.md\` for how to actually run this. In short:
double-click \`host/dualdeck-host.sh\` on your HTPC and
\`client/dualdeck-client.sh\` on your Steam Deck -- each opens a
menu that covers launching, Steam integration, and updates.

Also includes \`host/emudeck-integration/\` (experimental): installs
DualDeck's patched emulators directly over an existing EmuDeck
install so its Steam shortcuts keep working unedited, instead of a
separate DualDeck-managed install. See that directory's own README.md.
NOTES

echo "== Bundling EmuDeck integration tool (host/emudeck-integration/) =="
# Ships scripts/emudeck-replace-in-place.sh + scripts/emudeck-check-drift.sh
# as part of the release archive itself, mirroring the exact relative
# layout those scripts already expect from a real source checkout
# (<root>/scripts/...) so neither script needs any changes to work
# unmodified from inside a packaged, downloaded release. host/'s existing
# install/update path (install-steam-shortcut.sh's/
# install-host-distrobox.sh's `cp -a "${host_root}/."`) already
# recursively copies any new subdirectory here into
# ~/.config/dualdeck/install/ on every future "Check for updates" -- zero
# changes needed to those install/update scripts either.
#
# Much smaller than it used to be: emudeck-replace-in-place.sh no longer
# builds anything locally (see its own header comment) -- it downloads
# already-built, already-patched AppImages from this same release instead
# -- so the whole build toolchain this bundle used to carry (pinned_
# commits.sh, ensure-packages.sh, build_emulator.sh, build_cache.sh,
# appimage_pack.sh, the three emulator patch files, and a full copy of
# CMakeLists.txt/protocol/adapter-sdk/host/remote-server needed to compile
# dualdeck-host-service on demand) is gone. All that's left is EmuDeck
# AppImage-path detection and the sidecar drift-detection manifest.
emudeck_bundle_dir="${pkg_dir}/host/emudeck-integration"
mkdir -p "${emudeck_bundle_dir}/scripts/lib"

cp "${repo_root}/scripts/emudeck-replace-in-place.sh" "${emudeck_bundle_dir}/scripts/"
cp "${repo_root}/scripts/emudeck-check-drift.sh" "${emudeck_bundle_dir}/scripts/"
cp "${repo_root}/scripts/lib/emudeck_paths.sh" "${emudeck_bundle_dir}/scripts/lib/"
cp "${repo_root}/scripts/lib/appimage_manifest.py" "${emudeck_bundle_dir}/scripts/lib/"
# GitHub/real-hardware report, 2026-08-01: emudeck-replace-in-place.sh
# now opens this host's firewall for DualDeck's ports at install time
# (see that script's own comment) -- needs this bundled too, the same
# reason emudeck_paths.sh/appimage_manifest.py are.
cp "${repo_root}/scripts/lib/host_firewall.sh" "${emudeck_bundle_dir}/scripts/lib/"
chmod +x "${emudeck_bundle_dir}/scripts/emudeck-replace-in-place.sh" \
         "${emudeck_bundle_dir}/scripts/emudeck-check-drift.sh"

# Read by emudeck-replace-in-place.sh's own version-detection fallback
# (no .git directory exists inside a packaged release) -- same
# version_tag every other part of this release reports.
echo "${version_tag}" > "${emudeck_bundle_dir}/VERSION"

cat > "${emudeck_bundle_dir}/README.md" <<EMUDECK_README
# EmuDeck / RetroDECK integration (experimental)

Installs DualDeck's patched melonDS/Azahar/Cemu directly at the same
path your existing EmuDeck installation's Steam shortcuts and launcher
scripts already point to (\`~/Applications/*.AppImage\`), so they keep
working unedited -- no need to change any EmuDeck shortcut, no separate
DualDeck-managed install directory for these emulators.

Also works with no EmuDeck installed at all, for **RetroDECK**: when no
existing EmuDeck AppImage is found, this installs one fresh at the same
\`~/Applications/*.AppImage\` path anyway. RetroDECK bundles its own
unpatched copy of each emulator inside its own Flatpak sandbox, but its
ES-DE-based game launcher already checks that exact host path first and
only falls back to its own bundled copy if nothing is there (confirmed
directly against RetroDECK's own source, not assumed -- see
\`docs/history.md\`'s 2026-08-28 "RetroDECK" entry) -- so
installing here is picked up automatically, no RetroDECK-specific setup
needed. One real caveat: RetroDECK's Nintendo DS system defaults to a
RetroArch core, not standalone melonDS, so you'll need to switch it to
"melonDS (Standalone)" in RetroDECK's own per-system emulator settings
for this to actually get used there (Cemu and Azahar are already
standalone-only/default, so those need no such change).

This downloads already-built, already-patched AppImages from this same
GitHub release (built once in CI, not compiled on your machine) and
verifies them against the release's \`SHA256SUMS\` before installing
anything -- no build toolchain, no Distrobox, nothing to compile.

Confirmed working end to end on real hardware (all three emulators
launching via their existing EmuDeck/Steam shortcuts) -- see
\`docs/history.md\`'s 2026-08-01 entries for the full
verification history and what's still outstanding. The RetroDECK/fresh-
install path is new and not yet confirmed against a real RetroDECK
install (see that same 2026-08-28 entry).

## Usage

    cd emudeck-integration/scripts
    ./emudeck-replace-in-place.sh --dry-run

Start with \`--dry-run\` -- it only detects your EmuDeck installs (or, for
an emulator with no EmuDeck install found, reports it would do a fresh
RetroDECK-compatible install instead) and reports what it would do, with
zero side effects (it won't even download anything). Once you're happy
with the plan:

    ./emudeck-replace-in-place.sh

Add \`--emulator melonds\` / \`--emulator azahar\` / \`--emulator cemu\`
(repeatable) to target specific emulators instead of every one found.
Add \`--yes\` to skip the confirmation prompt.

This also opens this host's firewall for DualDeck's ports (via
\`firewall-cmd\`/\`ufw\`, whichever is present -- may prompt for your
password) the first time it actually installs something, so the client
can reach the private host-service each patched AppImage starts on its
own when launched -- see \`docs/history.md\`'s "client hangs
on connecting" entry for why this matters.

To check later whether EmuDeck's own updater has silently replaced an
installed AppImage (losing DualDeck's patch), and re-patch it if so:

    ./emudeck-check-drift.sh
    ./emudeck-check-drift.sh --fix

Re-patching after drift like this just re-downloads and re-verifies the
same prebuilt AppImage from this release -- no rebuild, ever. Applies to
all three emulators.
EMUDECK_README

echo "Bundled EmuDeck integration tool at host/emudeck-integration/"

# The archive itself gets a *constant* filename (unlike the internal
# directory name above, which embeds the commit for clarity once
# extracted) so that re-uploading it to the same GitHub Release asset
# name replaces the previous build instead of accumulating a new,
# differently-named asset on every push (softprops/action-gh-release
# matches on asset filename to decide what to replace).
archive_name="melonds-remote-linux-x86_64.tar.gz"
tar czf "${out_dir}/${archive_name}" -C "${work_dir}" "${pkg_name}"
echo "Wrote ${out_dir}/${archive_name} (contains ${pkg_name}/)"

# melonds-remote-linux-x86_64.tar.gz above is the archive's permanent,
# never-renamed filename (see the comment on archive_name) -- every
# already-installed client/host's apply-update.sh has that exact
# download URL hardcoded, so it must keep existing forever for
# auto-update to keep working across the melonDS-Remote -> DualDeck
# rebrand. This is a same-bytes second copy under the new name, for a
# tidier-looking Releases page and for DualDeck-Installer.sh going
# forward -- not a replacement. Copied here, before SHA256SUMS is
# computed below, so both names get a checksum entry -- doing this copy
# later (e.g. as a separate release.yml step, which is where this used
# to live) left DualDeck-Installer.sh downloading a file SHA256SUMS had
# no entry for at all, so every fresh install/update failed checksum
# verification unconditionally.
alias_archive_name="dualdeck-linux-x86_64.tar.gz"
cp "${out_dir}/${archive_name}" "${out_dir}/${alias_archive_name}"
echo "Wrote ${out_dir}/${alias_archive_name} (same bytes as ${archive_name})"

# GitHub issue #26: lets DualDeck-Installer.sh (and anyone else) verify
# the archive's integrity before extracting/installing anything from
# it, rather than trusting a plain HTTPS download unconditionally. A
# constant filename for the same reason the archive itself has one --
# re-publishing to the same release asset name replaces it instead of
# accumulating stale duplicates. Also covers the three prebuilt AppImages
# above -- emudeck-replace-in-place.sh verifies its download against this
# same file before installing anything over a user's existing EmuDeck
# AppImage.
(cd "${out_dir}" && sha256sum "${archive_name}" "${alias_archive_name}" \
    dualdeck-melonds-patched-linux-x86_64.AppImage \
    dualdeck-azahar-patched-linux-x86_64.AppImage \
    dualdeck-cemu-patched-linux-x86_64.AppImage \
    > SHA256SUMS)
echo "Wrote ${out_dir}/SHA256SUMS"

# DualDeck-Installer.sh needs no build-time-computed values (it always
# fetches "latest" dynamically, same as check-for-updates.sh/
# apply-update.sh above), so it's a plain static repo file rather than
# a heredoc here -- this just publishes it as its own downloadable
# release asset alongside the archive it knows how to fetch/verify/
# install.
cp "${repo_root}/scripts/DualDeck-Installer.sh" "${out_dir}/DualDeck-Installer.sh"
echo "Wrote ${out_dir}/DualDeck-Installer.sh"
