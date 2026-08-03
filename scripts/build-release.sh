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
# added. See docs/known-limitations.md's real-world verification entry.
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
ensure_packages "build" \
    "cmake extra-cmake-modules ninja-build build-essential git python3 libcurl4-gnutls-dev libpcap0.8-dev libsdl2-dev libarchive-dev libenet-dev libzstd-dev libfaad-dev qt6-base-dev qt6-base-private-dev qt6-multimedia-dev qt6-svg-dev qt6-wayland libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev libwayland-dev libxkbcommon-dev libdrm-dev libgbm-dev libdecor-0-dev libturbojpeg0-dev" \
    "cmake extra-cmake-modules ninja-build gcc-c++ git python3 libcurl-devel libpcap-devel SDL2-devel libarchive-devel enet-devel libzstd-devel faad2-devel qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtmultimedia-devel qt6-qtsvg-devel qt6-qtwayland libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXfixes-devel libXi-devel libXScrnSaver-devel libXtst-devel wayland-devel libxkbcommon-devel libdrm-devel mesa-libgbm-devel libdecor-devel turbojpeg-devel" \
    "cmake extra-cmake-modules ninja base-devel git python curl libpcap sdl2 libarchive enet zstd faad2 qt6-base qt6-multimedia qt6-svg qt6-wayland libx11 libxext libxrandr libxcursor libxfixes libxi libxss libxtst wayland libxkbcommon libdrm mesa libdecor libjpeg-turbo"

# Azahar (3DS) additionally needs a Vulkan SDK and Boost headers beyond
# melonDS's own dependency list above -- see
# docs/azahar-integration-analysis.md for why (real 3D rendering, unlike
# the DS's largely-2D workload). Debian package names verified against
# this exact sandbox; Fedora/Arch names are best-effort and unverified
# (see docs/known-limitations.md's AzaharAdapter entry).
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
ensure_packages "cemu build" \
    "freeglut3-dev libbluetooth-dev libgcrypt20-dev libglm-dev libgtk-3-dev libpulse-dev libsecret-1-dev libsystemd-dev libtool nasm libusb-1.0-0-dev" \
    "freeglut-devel bluez-libs-devel libgcrypt-devel glm-devel gtk3-devel pulseaudio-libs-devel libsecret-devel systemd-devel libtool nasm libusb1-devel perl-IPC-Cmd" \
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
if [[ -f "${sdl3_install}/lib/cmake/SDL3/SDL3Config.cmake" ]]; then
    echo "already built at ${sdl3_install}, skipping (cache hit)"
else
    rm -rf "${sdl3_src}"
    git clone --depth 1 --branch "${SDL3_TAG}" https://github.com/libsdl-org/SDL.git "${sdl3_src}"
    cmake -S "${sdl3_src}" -B "${sdl3_src}/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${sdl3_install}" \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF \
        "${cmake_launcher_args[@]}"
    cmake --build "${sdl3_src}/build" -j"$(nproc)"
    cmake --install "${sdl3_src}/build"
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
# See host/cemu-patches/README.md and docs/known-limitations.md's
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
build_cemu cemu_bin "${work_dir}" "${repo_root}" "${CEMU_COMMIT}"

echo "== [5/6] Client + host prototype (this repo) =="
repo_build="${work_dir}/repo-build"
cmake -S "${repo_root}" -B "${repo_build}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DDUALDECK_BUILD_CLIENT=ON \
    -DDUALDECK_BUILD_HOST=ON -DCMAKE_PREFIX_PATH="${sdl3_install}" \
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
# below, and docs/known-limitations.md for the real-hardware repro that
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

# Prebuilt, patched, self-contained AppImages for
# emudeck-replace-in-place.sh to download and drop straight into an
# existing EmuDeck install (see docs/known-limitations.md's 2026-08-01
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
for qt_plugin_category in platforms xcbglintegrations wayland-shell-integration \
    wayland-decoration-client wayland-graphics-integration-client; do
    if [[ -d "${qt6_plugins_dir}/${qt_plugin_category}" ]]; then
        mkdir -p "${qt_plugin_staging_root}"
        cp -a "${qt6_plugins_dir}/${qt_plugin_category}" "${qt_plugin_staging_root}/${qt_plugin_category}"
        qt_plugin_extra_dirs="${qt_plugin_extra_dirs:+${qt_plugin_extra_dirs}:}${qt_plugin_staging_root}/${qt_plugin_category}"
    elif [[ "${qt_plugin_category}" == "xcbglintegrations" ]]; then
        echo "warning: this build machine's Qt6 install has no xcbglintegrations plugin --" >&2
        echo "packaged melonDS/Azahar may fail to create a GL-backed window over X11/XWayland." >&2
    fi
done

melonds_apprun="${work_dir}/AppRun-melonds"
generate_apprun_melonds "${melonds_apprun}" "${version_tag}"
pack_appimage "${melonds_bin}" "${out_dir}/dualdeck-melonds-patched-linux-x86_64.AppImage" \
    melonDS "${melonds_apprun}" "" "${qt_plugin_extra_dirs}"

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

cp "${repo_root}/scripts/lib/ensure-packages.sh" "${pkg_dir}/host/internal/ensure-packages.sh"
cp "${repo_root}/scripts/lib/steam_shortcut.py" "${pkg_dir}/host/internal/steam_shortcut.py"
cp "${repo_root}/scripts/lib/steam_restart_helper.sh" "${pkg_dir}/host/internal/steam_restart_helper.sh"
cp "${repo_root}/scripts/lib/host_firewall.sh" "${pkg_dir}/host/internal/host_firewall.sh"
cp "${repo_root}/scripts/lib/adapter_socket_probe.sh" "${pkg_dir}/host/internal/adapter_socket_probe.sh"

cp "${repo_build}/client/dualdeck-client" "${pkg_dir}/client/dualdeck-client"
chmod +x "${pkg_dir}/client/dualdeck-client"
cp -a "${sdl3_install}"/lib/libSDL3.so* "${pkg_dir}/client/lib/"

cp "${repo_root}/scripts/lib/ensure-packages.sh" "${pkg_dir}/client/internal/ensure-packages.sh"
cp "${repo_root}/scripts/lib/steam_shortcut.py" "${pkg_dir}/client/internal/steam_shortcut.py"
cp "${repo_root}/scripts/lib/steam_restart_helper.sh" "${pkg_dir}/client/internal/steam_restart_helper.sh"
# Opt-in touchpad-as-native-input experiment (client-only -- this is
# about the Deck's own trackpads, not anything host-side) -- see
# steam_input_config.py's own module docstring for the real research
# behind it.
cp "${repo_root}/scripts/lib/steam_input_config.py" "${pkg_dir}/client/internal/steam_input_config.py"

# Real user report, 2026-08-02: the trackpad-experiment toggle only
# existed in dualdeck-client.sh's outer shell menu, but that menu is
# unreachable from Gaming Mode -- install-steam-shortcut.sh points the
# Steam shortcut's Exe straight at run-client.sh (see its own comment
# just below), never at this menu script, so the only way to reach it
# was double-clicking dualdeck-client.sh manually in Desktop Mode. The
# user (reasonably) expected it in the client's own in-app Settings
# screen instead, which the Steam shortcut always reaches. This thin
# wrapper is the one place that knows how to check/toggle the
# experiment (sourcing steam_restart_helper.sh for the same
# Steam-caches-this-file-in-memory safety steam_shortcut.py's own writes
# already have), so both dualdeck-client.sh's menu AND main.cpp's
# Settings screen (which shells out to this, not to steam_input_config.py
# directly, since it's a plain script call away rather than needing
# main.cpp to know steam_restart_helper.sh's bash-specific machinery)
# stay in sync with exactly one implementation.
cat > "${pkg_dir}/client/internal/configure-trackpad-experiment.sh" <<'WRAP'
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
# shellcheck source=scripts/lib/steam_restart_helper.sh
source ./steam_restart_helper.sh

error_log="${HOME}/.config/dualdeck-client/install.log"
# Keep in sync with the same constant in install-steam-shortcut.sh,
# uninstall-steam-shortcut.sh, apply-update.sh, and dualdeck-client.sh's
# own copy -- steam_input_config.py needs the exact same --exe/--name
# those already use so it computes the identical shortcut appid.
client_shortcut_exe="${HOME}/.config/dualdeck-client/install/internal/run-client.sh"
client_shortcut_name="DualDeck"

if [[ "${1:-}" == "--status" ]]; then
    exec python3 ./steam_input_config.py --exe "${client_shortcut_exe}" --name "${client_shortcut_name}" --status
fi

no_restart=0
extra_args=()
for arg in "$@"; do
    case "${arg}" in
        --remove) extra_args+=(--remove) ;;
        --no-restart) no_restart=1 ;;
    esac
done

# Real user report, 2026-08-02: toggling this from the client's own
# in-app Settings screen "seemingly crashes Steam and restarts it" --
# real, not a misread: run_steam_shortcut_with_restart's automatic
# `steam -shutdown` + relaunch (below) is the right call for
# dualdeck-client.sh's own standalone menu, run in Desktop Mode BEFORE
# anything is launched, but the client is normally itself a
# Steam-launched process in Gaming Mode -- killing Steam out from under
# a game it's actively running is jarring at best and, since Steam
# effectively *is* the Gaming Mode session on a real Deck, risks tearing
# down more than just Steam. main.cpp's Settings-screen calls pass
# --no-restart for exactly this reason; dualdeck-client.sh's own menu
# doesn't, so its existing auto-restart behavior (a real convenience on
# a controller-only Desktop Mode setup with no other way to reopen a
# closed Steam) is unchanged.
if [[ "${no_restart}" -eq 1 ]]; then
    # Writes anyway (--force, same accepted "Steam's in-memory cache
    # could overwrite this on its next save" tradeoff steam_shortcut.py's
    # own --force already carries) rather than just refusing outright --
    # the change still needs to land somewhere, and a manual Steam
    # restart later (the user's own choice, on their own schedule) will
    # pick it up correctly either way.
    python3 ./steam_input_config.py --exe "${client_shortcut_exe}" --name "${client_shortcut_name}" \
        --force "${extra_args[@]}"
    echo "Restart Steam yourself whenever's convenient for this to take effect -- not done automatically from here."
else
    run_steam_shortcut_with_restart ./steam_input_config.py "${error_log}" \
        --exe "${client_shortcut_exe}" --name "${client_shortcut_name}" "${extra_args[@]}"
fi
WRAP
chmod +x "${pkg_dir}/client/internal/configure-trackpad-experiment.sh"

cat > "${pkg_dir}/client/internal/run-client.sh" <<'WRAP'
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

ensure_packages "client runtime" \
    "libx11-6 libxext6 libxrandr2 libxcursor1 libxfixes3 libxi6 libxss1 libwayland-client0 libwayland-cursor0 libwayland-egl1 libxkbcommon0 libdrm2 libgbm1 libdecor-0-0 libturbojpeg0" \
    "libX11 libXext libXrandr libXcursor libXfixes libXi libXScrnSaver libwayland-client libwayland-cursor libwayland-egl libxkbcommon libdrm mesa-libgbm libdecor turbojpeg" \
    "libx11 libxext libxrandr libxcursor libxfixes libxi libxss wayland libxkbcommon libdrm mesa libdecor libjpeg-turbo" \
    || echo "warning: could not verify/install client runtime libraries automatically; continuing anyway in case they're already present" >&2

# Read by main.cpp so the host can reject a connection from a client
# running a different, incompatible release -- see protocol.h's
# HelloPayload::appVersion and net_server.cpp's comparison logic.
# dirname(client_root) matches check-for-updates.sh's own VERSION lookup
# (the archive root, or the central install directory's parent).
export DUALDECK_VERSION="$(cat "$(dirname "${client_root}")/VERSION" 2>/dev/null || true)"

LD_LIBRARY_PATH="${client_root}/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec "${client_root}/dualdeck-client" "$@"
WRAP
chmod +x "${pkg_dir}/client/internal/run-client.sh"

cat > "${pkg_dir}/client/internal/install-steam-shortcut.sh" <<'WRAP'
#!/usr/bin/env bash
# Registers dualdeck-client as a Steam non-Steam-game shortcut --
# see ./steam_shortcut.py for exactly what this does and why it's
# careful about it (backs up shortcuts.vdf first, refuses to run while
# Steam is open unless --force). Any arguments given here are passed
# through as the shortcut's launch options, e.g.:
#   ./install-steam-shortcut.sh --host 192.168.1.50
# Normally launched via ../../dualdeck-client.sh's "Add to Steam"
# menu choice, not directly.
#
# Copies the whole client/ directory (this script's parent) into a
# fixed central location -- see central_install_dir below -- and points
# the Steam shortcut at run-client.sh there (specifically, not the raw
# binary, so the bundled SDL3 library is found via LD_LIBRARY_PATH).
# That way, re-running this from a newer release's extracted archive
# later always updates the same shortcut instead of leaving a stale
# duplicate around pointing at a now-deleted download folder, and the
# extracted archive can be deleted once this has run --
# uninstall-steam-shortcut.sh only ever needs the central copy.
#
# A failed update can't break a working install (GitHub issue #11): the
# new files are staged in a separate directory first and only swapped
# into place once staging succeeds, keeping the replaced version as a
# one-generation backup (*.previous) rather than deleting it outright.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
client_root="$(cd .. && pwd)"
# shellcheck source=scripts/lib/steam_restart_helper.sh
source ./steam_restart_helper.sh

# Surfaces failures visibly instead of just closing silently when
# double-clicked with no visible terminal attached (GitHub issue #11) --
# logs to a persistent file and, when available (SteamOS Desktop Mode/
# Bazzite are both KDE Plasma), pops up a graphical error dialog via
# kdialog.
error_log="${HOME}/.config/dualdeck-client/install.log"
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") install-steam-shortcut.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "DualDeck" \
            --error "Installing the Steam shortcut failed: ${failing_cmd}
(exit code ${exit_code})

Details logged to:
${error_log}" 2>/dev/null || true
    fi
}
trap 'ec=$?; on_error "${ec}" "${LINENO}" "${BASH_COMMAND}"' ERR

launch_options=""
extra_args=()
dry_run=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) extra_args+=("$1"); dry_run=1; shift ;;
        --force) extra_args+=("$1"); shift ;;
        --user) extra_args+=("$1" "$2"); shift 2 ;;
        --user=*) extra_args+=("$1"); shift ;;
        *) launch_options+="${launch_options:+ }$1"; shift ;;
    esac
done

# Keep in sync with the same constant in uninstall-steam-shortcut.sh and
# apply-update.sh.
central_install_dir="${HOME}/.config/dualdeck-client/install"
staging_dir="${central_install_dir}.new"
previous_dir="${central_install_dir}.previous"

# One-time melonDS-Remote -> DualDeck rebrand migration: this central
# install dir moved (old: ~/.config/melonds-remote-client/install) and
# the Steam AppName changed ("melonDS Remote" -> "DualDeck")
# simultaneously, so steam_shortcut.py's Exe-OR-AppName matching can't
# reliably find-and-update the old entry on its own. Explicitly remove
# the old entry by its old identity first, best-effort.
old_central_install_dir="${HOME}/.config/melonds-remote-client/install"
if [[ -d "${old_central_install_dir}" && "${dry_run}" -eq 0 ]]; then
    python3 ./steam_shortcut.py \
        --exe "${old_central_install_dir}/internal/run-client.sh" \
        --name "melonDS Remote" \
        --remove "${extra_args[@]}" >/dev/null 2>&1 || true
fi

if [[ "${dry_run}" -eq 0 ]]; then
    rm -rf "${staging_dir}"
    mkdir -p "${staging_dir}"
    cp -a "${client_root}/." "${staging_dir}/"
    find "${staging_dir}" -name '*.sh' -exec chmod +x {} +

    # Only reached if staging succeeded -- safe to activate now. Keeps
    # just one backup generation, not unbounded.
    rm -rf "${previous_dir}"
    if [[ -d "${central_install_dir}" ]]; then
        mv "${central_install_dir}" "${previous_dir}"
    fi
    mv "${staging_dir}" "${central_install_dir}"

    # Same reasoning as the host equivalent in install-host-distrobox.sh:
    # keeps "../check-for-updates.sh" (the menu's "Check for updates")
    # and run-client.sh's own auto-update check both resolvable from a
    # copy of this install later run from inside the central directory
    # itself, after the original downloaded archive is gone.
    cp "$(dirname "${client_root}")/check-for-updates.sh" "$(dirname "${central_install_dir}")/check-for-updates.sh" 2>/dev/null || true
    cp "$(dirname "${client_root}")/VERSION" "$(dirname "${central_install_dir}")/VERSION" 2>/dev/null || true
fi

run_steam_shortcut_with_restart ./steam_shortcut.py "${error_log}" \
    --exe "${central_install_dir}/internal/run-client.sh" \
    --name "DualDeck" \
    --launch-options "${launch_options}" \
    "${extra_args[@]}" && shortcut_exit=0 || shortcut_exit=$?
if [[ "${shortcut_exit}" -ne 0 ]]; then
    on_error "${shortcut_exit}" "${LINENO}" "steam_shortcut.py"
    exit "${shortcut_exit}"
fi
WRAP
chmod +x "${pkg_dir}/client/internal/install-steam-shortcut.sh"

cat > "${pkg_dir}/client/internal/uninstall-steam-shortcut.sh" <<'WRAP'
#!/usr/bin/env bash
# Removes the DualDeck Steam non-Steam-game shortcut added by
# install-steam-shortcut.sh, and deletes the central install directory
# that script copies everything into. See ./steam_shortcut.py for
# exactly what the shortcut-removal part does and why it's careful
# about it (backs up shortcuts.vdf first, refuses to run while Steam is
# open unless --force). Normally launched via
# ../../dualdeck-client.sh's "Remove from Steam" menu choice, not
# directly.
#
# Always targets the fixed central install directory below for the
# actual --exe match, regardless of where this script itself is run
# from: this exact file is also the one copied into that central
# directory by install-steam-shortcut.sh, and must keep removing the
# same shortcut from there indefinitely, even after this archive has
# been deleted. It does fall back to this archive's own sibling copy of
# steam_shortcut.py (see below) purely so a freshly downloaded archive
# can still clean up a shortcut from an older version of this project
# that was never migrated to the central directory -- steam_shortcut.py's
# own AppName fallback (see its module docstring) is what actually finds
# and removes that kind of stale entry.
set -euo pipefail

# Surfaces failures visibly instead of just closing silently when
# double-clicked with no visible terminal attached (GitHub issue #11) --
# logs to a persistent file and, when available (SteamOS Desktop Mode/
# Bazzite are both KDE Plasma), pops up a graphical error dialog via
# kdialog.
error_log="${HOME}/.config/dualdeck-client/install.log"
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") uninstall-steam-shortcut.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "DualDeck" \
            --error "Removing the Steam shortcut failed: ${failing_cmd}
(exit code ${exit_code})

Details logged to:
${error_log}" 2>/dev/null || true
    fi
}
trap 'ec=$?; on_error "${ec}" "${LINENO}" "${BASH_COMMAND}"' ERR

# Keep in sync with the same constant in install-steam-shortcut.sh and
# apply-update.sh.
central_install_dir="${HOME}/.config/dualdeck-client/install"
self_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

steam_shortcut_py=""
for candidate in \
    "${central_install_dir}/internal/steam_shortcut.py" \
    "${self_dir}/steam_shortcut.py"
do
    if [[ -f "${candidate}" ]]; then
        steam_shortcut_py="${candidate}"
        break
    fi
done

if [[ -z "${steam_shortcut_py}" ]]; then
    echo "Nothing installed at ${central_install_dir}, and no local copy of" \
         "steam_shortcut.py found either -- nothing to remove."
    exit 0
fi

# Same two-candidate resolution as steam_shortcut_py above -- this
# script runs either from an extracted archive or from the central
# install copy, and steam_restart_helper.sh is bundled alongside
# steam_shortcut.py in both places (scripts/build-release.sh's
# packaging step).
steam_restart_helper=""
for candidate in \
    "${central_install_dir}/internal/steam_restart_helper.sh" \
    "${self_dir}/steam_restart_helper.sh"
do
    if [[ -f "${candidate}" ]]; then
        steam_restart_helper="${candidate}"
        break
    fi
done
if [[ -n "${steam_restart_helper}" ]]; then
    # shellcheck source=scripts/lib/steam_restart_helper.sh
    source "${steam_restart_helper}"
fi

# One-time melonDS-Remote -> DualDeck rebrand cleanup: also try removing
# under the old identity (old central dir, old Exe, old AppName), since
# the Exe-OR-AppName fallback alone can't bridge a compound change of
# both fields at once. No-op if it was never installed there or was
# already migrated.
old_central_install_dir="${HOME}/.config/melonds-remote-client/install"
python3 "${steam_shortcut_py}" \
    --exe "${old_central_install_dir}/internal/run-client.sh" \
    --name "melonDS Remote" \
    --remove "$@" >/dev/null 2>&1 || true

if [[ -n "${steam_restart_helper}" ]]; then
    run_steam_shortcut_with_restart "${steam_shortcut_py}" "${error_log}" \
        --exe "${central_install_dir}/internal/run-client.sh" \
        --name "DualDeck" \
        --remove \
        "$@"
else
    python3 "${steam_shortcut_py}" \
        --exe "${central_install_dir}/internal/run-client.sh" \
        --name "DualDeck" \
        --remove \
        "$@"
fi

dry_run=0
for arg in "$@"; do
    [[ "${arg}" == "--dry-run" ]] && dry_run=1
done

if [[ "${dry_run}" -eq 0 ]]; then
    # cd out first -- this script may itself be running from inside
    # central_install_dir (it's the copy install-steam-shortcut.sh made
    # there), so deleting the shell's own current directory out from
    # under it is avoided by leaving before removing anything.
    cd /
    for dir in "${central_install_dir}" "${central_install_dir}.new" "${central_install_dir}.previous"; do
        if [[ -d "${dir}" ]]; then
            echo "Removing ${dir}"
            rm -rf -- "${dir}"
        fi
    done

    # check-for-updates.sh/VERSION are staged as siblings of install/
    # (see install-steam-shortcut.sh), not inside any of the three
    # directories just removed above.
    rm -f -- "$(dirname "${central_install_dir}")/check-for-updates.sh" \
             "$(dirname "${central_install_dir}")/VERSION"
fi
WRAP
chmod +x "${pkg_dir}/client/internal/uninstall-steam-shortcut.sh"

cat > "${pkg_dir}/client/internal/apply-update.sh" <<'WRAP'
#!/usr/bin/env bash
# Downloads the latest DualDeck release and installs the client
# part, by handing off to that release's own
# client/internal/install-steam-shortcut.sh --force -- reusing its
# already-verified stage-then-swap file safety rather than duplicating
# any of that logic here. This script's only job is fetching and
# extracting the new release archive. Normally invoked from
# ../../dualdeck-client.sh's "Check for updates" menu choice
# after the user confirms; also runnable standalone. See
# host/internal/apply-update.sh for the host equivalent (same design,
# minus the Distrobox complexity the client doesn't need).
#
# --force here does not mean "always touch shortcuts.vdf": since
# GitHub issue "atomic updates -- Steam shouldn't need to be closed to
# update", install-steam-shortcut.sh only writes shortcuts.vdf when the
# shortcut's Exe/AppName/StartDir/LaunchOptions actually differ from
# what's already there (see steam_shortcut.py's shortcut_up_to_date()) --
# which never happens for a routine version bump, since those are all
# derived from the fixed central install directory, not the release
# version. --force only matters for the rare case something genuinely
# needs to change; a plain update's file swap (already atomic via
# stage-then-rename) is the only thing that happens, and Steam never
# needs to be closed or restarted for it.
#
# Only ever downloads from this exact, hardcoded GitHub Releases URL
# over HTTPS -- never anything derived from user input, an environment
# variable, or a config file.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

error_log="${HOME}/.config/dualdeck-client/install.log"
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") apply-update.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "DualDeck" --error "Updating failed: ${failing_cmd}
(exit code ${exit_code})

Details logged to:
${error_log}" 2>/dev/null || true
    fi
}
trap 'ec=$?; on_error "${ec}" "${LINENO}" "${BASH_COMMAND}"' ERR

if ! command -v curl >/dev/null 2>&1; then
    echo "error: curl is required to download updates -- install it, or download" >&2
    echo "the latest release manually from https://github.com/Crimson3076/DualDeck/releases/latest" >&2
    exit 1
fi

repo="Crimson3076/DualDeck"
download_url="https://github.com/${repo}/releases/latest/download/melonds-remote-linux-x86_64.tar.gz"

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

echo "Downloading the latest release..."
curl -fsSL --max-time 180 -o "${work_dir}/release.tar.gz" "${download_url}"

echo "Extracting..."
tar xzf "${work_dir}/release.tar.gz" -C "${work_dir}"

extracted_dir=""
for candidate in "${work_dir}"/melonds-remote-*; do
    [[ -d "${candidate}" ]] && extracted_dir="${candidate}" && break
done
if [[ -z "${extracted_dir}" ]]; then
    echo "error: couldn't find the extracted release directory" >&2
    exit 1
fi

echo "Installing..."
# Not exec'd: the work_dir EXIT trap above must still fire to clean up
# the download afterward, which exec'ing over this process would skip.
"${extracted_dir}/client/internal/install-steam-shortcut.sh" --force
WRAP
chmod +x "${pkg_dir}/client/internal/apply-update.sh"

cat > "${pkg_dir}/client/dualdeck-client.sh" <<'WRAP'
#!/usr/bin/env bash
# The one thing to double-click to set up or run the DualDeck
# client -- shows a simple menu and delegates to whichever of the
# internal/ scripts actually applies, so a user never has to figure out
# which one they need. See ../host/dualdeck-host.sh for the host
# equivalent (same menu shape).
#
# Uses a graphical kdialog menu when available (SteamOS Desktop Mode
# and Bazzite are both KDE Plasma, where kdialog is standard) and falls
# back to a plain numbered prompt in a terminal otherwise.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

error_log="${HOME}/.config/dualdeck-client/install.log"
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") dualdeck-client.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "DualDeck" --error "Something went wrong: ${failing_cmd}
(exit code ${exit_code})

Details logged to:
${error_log}" 2>/dev/null || true
    fi
}
trap 'ec=$?; on_error "${ec}" "${LINENO}" "${BASH_COMMAND}"' ERR

# Keep in sync with the same constant in internal/install-steam-shortcut.sh,
# internal/uninstall-steam-shortcut.sh, internal/apply-update.sh, and
# internal/configure-trackpad-experiment.sh's own copy -- only needed
# directly in this script for steam-remove's quiet, non-restart-helper
# cleanup call below (see its own comment for why that one deliberately
# doesn't go through configure-trackpad-experiment.sh).
client_shortcut_exe="${HOME}/.config/dualdeck-client/install/internal/run-client.sh"
client_shortcut_name="DualDeck"

have_kdialog() { command -v kdialog >/dev/null 2>&1; }

info() {
    if have_kdialog; then
        kdialog --title "DualDeck" --msgbox "$1" 2>/dev/null
    else
        echo
        echo "$1"
        echo
    fi
}

confirm() {
    if have_kdialog; then
        kdialog --title "DualDeck" --yesno "$1" 2>/dev/null
    else
        read -rp "$1 [y/N] " reply
        [[ "${reply}" =~ ^[Yy]$ ]]
    fi
}

# Real user request, 2026-08-02: "the controller layout should function
# exactly like a steam controller/steam deck" -- specifically, the
# touchpad only forwarded raw touch while the STEAM button was held
# (see steam_input_config.py's own module docstring for the actual
# research and fix: disabling Steam Input for this one shortcut, not a
# custom Controller Layout, per RPCS3's real precedent). This same
# toggle also lives in the client's own in-app Settings screen
# (client/src/main.cpp) -- both call internal/
# configure-trackpad-experiment.sh, the one place that actually knows
# how to check/change this, rather than duplicating that logic here.
# Dynamic label mirrors ../host/dualdeck-host.sh's own
# hostcontrol-daemon precedent -- doesn't write anything to check
# status, so this is always safe/instant to show regardless of whether
# Steam is running.
trackpad_experiment_label() {
    local status
    status="$(./internal/configure-trackpad-experiment.sh --status 2>/dev/null || echo "enabled")"
    if [[ "${status}" == "disabled" ]]; then
        echo "trackpad-experiment (Steam Input disabled, ON)"
    else
        echo "trackpad-experiment (Steam Input enabled, off)"
    fi
}

choose_action() {
    if have_kdialog; then
        kdialog --title "DualDeck" --menu "What would you like to do?" \
            launch "Launch DualDeck now" \
            steam-add "Add to Steam (Big Picture / Gaming Mode)" \
            steam-remove "Remove from Steam / uninstall" \
            trackpad-experiment "$(trackpad_experiment_label)" \
            update "Check for updates / update" \
            2>/dev/null || echo "cancel"
    else
        # All of this goes to stderr, not stdout -- the caller captures
        # this function's stdout as the actual selection
        # ("action=\"\$(choose_action)\"" below), so any of the menu
        # display text leaking onto stdout would get appended to that
        # and break the case match entirely.
        {
            echo "DualDeck"
            echo "  1) Launch DualDeck now"
            echo "  2) Add to Steam (Big Picture / Gaming Mode)"
            echo "  3) Remove from Steam / uninstall"
            echo "  4) $(trackpad_experiment_label)"
            echo "  5) Check for updates / update"
            echo "  6) Exit"
        } >&2
        read -rp "Choice [1-6]: " choice
        case "${choice}" in
            1) echo "launch" ;;
            2) echo "steam-add" ;;
            3) echo "steam-remove" ;;
            4) echo "trackpad-experiment" ;;
            5) echo "update" ;;
            *) echo "cancel" ;;
        esac
    fi
}

action="$(choose_action)"

case "${action}" in
    launch)
        # exec, not a plain call: this becomes the foreground process,
        # same as launching any other way (Steam shortcut, double-
        # clicking internal/run-client.sh directly, etc.) -- no menu
        # process left hanging around behind it.
        exec ./internal/run-client.sh
        ;;
    steam-add)
        if ./internal/install-steam-shortcut.sh; then
            info "Added DualDeck to Steam. Restart Steam (or switch to Gaming Mode) to see it, and set its Controller Layout to a plain Gamepad template once it's there."
        fi
        # A failure here already logged and showed its own error dialog
        # (install-steam-shortcut.sh has the same error-trap pattern as
        # this script) -- nothing more to do.
        ;;
    trackpad-experiment)
        current_status="$(./internal/configure-trackpad-experiment.sh --status 2>/dev/null || echo "enabled")"
        if [[ "${current_status}" == "disabled" ]]; then
            if confirm "Re-enable Steam Input for the DualDeck Client shortcut? This turns the trackpad-as-native-touchpad experiment back off -- the touchpad goes back to only working while STEAM is held."; then
                if ./internal/configure-trackpad-experiment.sh --remove; then
                    info "Steam Input re-enabled for DualDeck Client. Restart Steam for this to take effect."
                fi
            fi
        else
            if confirm "Disable Steam Input for the DualDeck Client shortcut? This is an experimental fix for the touchpad only working while STEAM is held -- see docs/known-limitations.md's 2026-08-02 entry. Not yet confirmed on real hardware; you can turn it back off from this same menu."; then
                if ./internal/configure-trackpad-experiment.sh; then
                    info "Steam Input disabled for DualDeck Client. Restart Steam for this to take effect, then relaunch DualDeck and test the touchpad without holding STEAM."
                fi
            fi
        fi
        ;;
    steam-remove)
        if confirm "This removes the Steam shortcut and the installed files. Continue?"; then
            # Best-effort, before the shortcut it's keyed off of is
            # removed below -- a full uninstall should also undo the
            # trackpad experiment if it was ever turned on, not leave a
            # stale Steam Input override pointing at an appid a later
            # re-add would recompute identically from the same --exe/
            # --name (so leaving it wouldn't even be inert -- it would
            # silently carry over to whatever gets installed next). Not
            # --force'd, same as uninstall-steam-shortcut.sh's own plain
            # (non-restart-helper) call just below -- if Steam happens to
            # be running, this silently no-ops rather than risking a
            # clobber; a stale key surviving one uninstall cycle is a
            # minor loose end, not worth forcing past that safety check.
            python3 ./internal/steam_input_config.py --exe "${client_shortcut_exe}" \
                --name "${client_shortcut_name}" --remove >/dev/null 2>&1 || true
            if ./internal/uninstall-steam-shortcut.sh; then
                info "Removed."
            fi
        fi
        ;;
    update)
        update_report="$(../check-for-updates.sh)"
        if echo "${update_report}" | grep -q "update available:"; then
            latest_version="$(echo "${update_report}" | sed -n 's/.*update available: //p' | head -1)"
            if confirm "${update_report}

Install ${latest_version} now? This downloads it from GitHub and also adds/updates the Steam shortcut."; then
                # Captured (not just checked for success) so the message
                # below can tell whether the Steam shortcut's Exe/AppName/
                # LaunchOptions actually changed -- for a routine update
                # they never do (see steam_shortcut.py's shortcut_up_to_date()),
                # so apply-update.sh doesn't touch shortcuts.vdf at all and
                # there's nothing for a running Steam to need reloading.
                # Only mention restarting Steam when apply-update.sh's own
                # output says it actually wrote the file.
                if update_output="$(./internal/apply-update.sh 2>&1)"; then
                    if echo "${update_output}" | grep -q "Restart Steam"; then
                        info "Updated to ${latest_version}. Restart Steam (or switch to Gaming Mode) to see the change."
                    else
                        info "Updated to ${latest_version}."
                    fi
                fi
                # A failure here already logged and showed its own error
                # dialog (apply-update.sh has the same error-trap pattern
                # as this script) -- nothing more to do.
            fi
        else
            info "${update_report}"
        fi
        ;;
    *)
        exit 0
        ;;
esac
WRAP
chmod +x "${pkg_dir}/client/dualdeck-client.sh"

cat > "${pkg_dir}/host/internal/run-host.sh" <<'WRAP'
#!/usr/bin/env bash
# Runs the patched melonDS binary with the remote server enabled,
# auto-installing any missing runtime system libraries first (Qt6, SDL2,
# libarchive, libenet, libfaad, etc. -- see
# host-shared-library-dependencies.txt for the exact list this binary
# was linked against).
# No arguments needed: this just opens melonDS itself, and you pick a
# ROM through its own File > Open ROM dialog same as any other launch
# (it defaults to EmuDeck's ROM folder the first time, and remembers
# your last one after that). A path can still be passed through if you
# want a specific one to open immediately, e.g.
# ./run-host.sh --boot always /path/to/your/game.nds -- see melonDS's
# own --help for the rest.
# Omit MELONDS_REMOTE_AUTH_TOKEN (the default) to use zero-typing
# device-approval authentication instead of a static shared secret.
# Normally launched via ../../dualdeck-host.sh's "Launch now" menu
# choice, not directly.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
host_root="$(cd .. && pwd)"

# shellcheck source=scripts/lib/ensure-packages.sh
source ./ensure-packages.sh

ensure_packages "host runtime" \
    "libcurl4-gnutls-dev libpcap0.8-dev libsdl2-dev libarchive-dev libenet-dev libzstd-dev libfaad-dev qt6-base-dev qt6-base-private-dev qt6-multimedia-dev qt6-svg-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libwayland-dev libxkbcommon-dev libdrm-dev libgbm-dev libdecor-0-dev libturbojpeg0-dev" \
    "libcurl-devel libpcap-devel SDL2-devel libarchive-devel enet-devel libzstd-devel faad2-devel qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtmultimedia-devel qt6-qtsvg-devel libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXfixes-devel libXi-devel libXScrnSaver-devel wayland-devel libxkbcommon-devel libdrm-devel mesa-libgbm-devel libdecor-devel turbojpeg-devel" \
    "curl libpcap sdl2 libarchive enet zstd faad2 qt6-base qt6-multimedia qt6-svg libx11 libxext libxrandr libxcursor libxfixes libxi libxss wayland libxkbcommon libdrm mesa libdecor libjpeg-turbo" \
    || echo "warning: could not verify/install host runtime libraries automatically; see host-shared-library-dependencies.txt and docs/troubleshooting.md" >&2

if [[ -f /run/ostree-booted ]] || command -v rpm-ostree >/dev/null 2>&1; then
    echo "Note: this looks like an immutable (rpm-ostree) system, e.g. Bazzite --" >&2
    echo "if melonDS fails to start below over a missing shared library, use" >&2
    echo "./install-host-distrobox.sh instead, which runs it inside a Distrobox" >&2
    echo "container with everything it needs already installed. See" >&2
    echo "docs/bazzite-host-setup.md." >&2
fi

export MELONDS_REMOTE_ENABLE=1
# Read by the patched melonDS's remote-server integration (see
# host/melonds-patches/0001-remote-server-integration.patch's
# MELONDS_REMOTE_VERSION wiring) so it can reject a connecting client
# running a different, incompatible release -- see
# protocol.h's HelloPayload::appVersion. dirname(host_root) is the
# archive root (or the central install directory's parent), matching
# check-for-updates.sh's own VERSION lookup.
export MELONDS_REMOTE_VERSION="$(cat "$(dirname "${host_root}")/VERSION" 2>/dev/null || true)"

# Host-control mode (GitHub issue #4, experimental): set by
# ../dualdeck-host.sh's "Host control only -- no emulator" menu choice,
# not on by default. Starts the standalone dualdeck-host-service binary
# in --adapter-ipc mode and runs it in the foreground until Ctrl+C --
# see docs/adr/0001-host-service-and-adapter-architecture.md section 10
# for why this is a separate process from any emulator: a client needs
# somewhere to connect and navigate *before* an emulator has even
# started. Uses the same zero-typing device-approval flow as melonDS's
# own in-process dialog -- a kdialog Yes/No popup on the host's own
# desktop (see kdialog_approval_prompt.h) -- unless
# MELONDS_REMOTE_AUTH_TOKEN is set, in which case that static shared
# secret is used instead. --state-dir points at the same directory
# melonDS's own device-approval uses, so a device approved once is
# approved for every emulator.
#
# Real user report, 2026-08-01: this used to *also* launch melonDS
# immediately afterward (as an out-of-process adapter, so it would be
# "ready" the moment a ROM was picked). That defeated the entire point:
# ModeCoordinator switches to Emulation mode the instant *any* adapter
# connects, with no concept of "connected but idle, no ROM loaded yet"
# -- so melonDS's out-of-process bridge connecting within a fraction of
# a second of starting flipped the session out of Host Control mode
# before the user could ever interact with it, every single time,
# regardless of whether a ROM was ever actually loaded. The client sat
# silently in Emulation mode waiting for video frames that would never
# arrive (no ROM = nothing for melonDS to render), which looked
# identical to "the touchpad/buttons just don't work."
#
# This manual menu choice is a *temporary, private* Host Control session
# on its own private socket, separate from the real fix for "genuinely
# emulator-agnostic hand-off": a persistent Host Control daemon
# (host/internal/dualdeck-host-control.service, installed via
# install-host-control-daemon.sh -- see ../dualdeck-host.sh's own menu)
# that stays up independent of any single Steam shortcut/session and
# listens on the shared default socket every emulator's own launch path
# now probes first (scripts/lib/adapter_socket_probe.sh). If that
# persistent daemon is already running, this manual choice is almost
# never what you actually want -- it doesn't replace the daemon's own
# session, it just starts a second, separate, throwaway one on a
# different, private socket -- so this only ever prints an informational
# note about that rather than refusing to run (deliberately non-blocking:
# a user who genuinely wants a second, temporary session for some reason
# should still be able to get one).
if [[ "${DUALDECK_HOST_CONTROL:-0}" == "1" ]]; then
    if [[ ! -x "${host_root}/internal/dualdeck-host-service" ]]; then
        echo "error: host/internal/dualdeck-host-service is missing from this" >&2
        echo "install -- re-download the release archive." >&2
        exit 1
    fi

    if command -v systemctl >/dev/null 2>&1 && \
       systemctl --user is-active --quiet dualdeck-host-control.service 2>/dev/null; then
        echo "Note: a persistent Host Control daemon already appears to be running" >&2
        echo "(dualdeck-host-control.service) -- you probably don't need this manual" >&2
        echo "session too. Starting one anyway, on its own private socket ..." >&2
    fi

    run_dir="${HOME}/.config/dualdeck/run"
    mkdir -p "${run_dir}"
    adapter_socket="${run_dir}/adapter.sock"
    rm -f "${adapter_socket}"

    auth_token_args=()
    if [[ -n "${MELONDS_REMOTE_AUTH_TOKEN:-}" ]]; then
        auth_token_args=(--auth-token "${MELONDS_REMOTE_AUTH_TOKEN}")
    fi

    echo "Starting the standalone Host Service (host-control mode, experimental) --" >&2
    echo "no emulator will be launched. Use its own Steam shortcut/launcher" >&2
    echo "whenever you're ready to play something -- see this script's own" >&2
    echo "header comment for why that starts a separate session rather than" >&2
    echo "taking over this one (yet)." >&2
    # See build-release.sh's bundle_library_dependencies() call for this
    # binary -- host/internal/lib ships its runtime deps (libturbojpeg)
    # so it runs on any host regardless of whether that library happens
    # to be installed system-wide (the real Bazzite bug this fixes).
    exec env LD_LIBRARY_PATH="${host_root}/internal/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
        "${host_root}/internal/dualdeck-host-service" --adapter-ipc --adapter-socket "${adapter_socket}" \
        --state-dir "${HOME}/.config/melonds-remote" "${auth_token_args[@]}" \
        --app-version "${MELONDS_REMOTE_VERSION}"
fi

# Real user request, 2026-08-01: auto-switch mode "if it detects that a
# compatible emulator or separate server is trying to open" -- if a
# persistent Host Control daemon is already listening on the shared
# default socket, connect melonDS to it out-of-process (letting
# ModeCoordinator switch that session straight to Emulation mode)
# instead of always defaulting to melonDS's own in-process remote
# server. Deliberately just a probe, not a spawn-if-missing (unlike
# probe_or_spawn_adapter_socket(), used by Azahar/Cemu, which have no
# in-process fallback at all): melonDS's in-process default is already
# proven and correct for the common case of no daemon running, so this
# only ever adds a better path on top of it, never removes the existing
# one.
# shellcheck source=scripts/lib/adapter_socket_probe.sh
source ./adapter_socket_probe.sh
default_socket="$(default_adapter_socket_path)"
if is_adapter_socket_live "${default_socket}"; then
    echo "DualDeck: found a running Host Control daemon -- melonDS will connect to" >&2
    echo "it out-of-process instead of running its own in-process remote server." >&2
    export MELONDS_REMOTE_OUT_OF_PROCESS=1
    export MELONDS_REMOTE_ADAPTER_SOCKET="${default_socket}"
fi
exec "${host_root}/melonDS" "$@"
WRAP
chmod +x "${pkg_dir}/host/internal/run-host.sh"

cat > "${pkg_dir}/host/internal/run-host-azahar.sh" <<'WRAP'
#!/usr/bin/env bash
# Runs the patched Azahar binary (Nintendo 3DS) -- see
# docs/azahar-integration-analysis.md and
# docs/adr/0001-host-service-and-adapter-architecture.md (section 11)
# in the DualDeck repository this was built from.
#
# Unlike run-host.sh's melonDS, Azahar's integration has only ever been
# built as an out-of-process adapter -- Azahar itself has no in-process
# approval dialog. This script always starts the standalone Host Service
# in the background first, the same way run-host.sh's opt-in
# host-control mode does; it pops the same zero-typing kdialog Yes/No
# approval prompt melonDS's in-process dialog gives you (see
# kdialog_approval_prompt.h), unless AZAHAR_REMOTE_AUTH_TOKEN is set, in
# which case that static shared secret is used instead. Normally
# launched via ../dualdeck-host.sh's "Launch..." menu, not
# directly.
#
# Pick a ROM through Azahar's own game list / File > Load File once it
# opens, same as any other Azahar launch.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
host_root="$(cd .. && pwd)"

# shellcheck source=scripts/lib/ensure-packages.sh
source ./ensure-packages.sh

ensure_packages "azahar runtime" \
    "qt6-base-dev libvulkan1 libsdl2-2.0-0 libopenal1 libboost-iostreams1.83.0 libboost-thread1.83.0" \
    "qt6-qtbase vulkan-loader SDL2 openal-soft boost-iostreams boost-thread" \
    "qt6-base vulkan-icd-loader sdl2 openal-soft boost-libs" \
    || echo "warning: could not verify/install Azahar runtime libraries automatically; see docs/troubleshooting.md" >&2

if [[ -f /run/ostree-booted ]] || command -v rpm-ostree >/dev/null 2>&1; then
    echo "Note: this looks like an immutable (rpm-ostree) system, e.g. Bazzite -- Azahar's" >&2
    echo "Distrobox launch path isn't built yet (see docs/known-limitations.md's" >&2
    echo "AzaharAdapter entry), so this may fail if a required library isn't already" >&2
    echo "present on the base image." >&2
fi

if [[ ! -x "${host_root}/internal/dualdeck-host-service" ]]; then
    echo "error: host/internal/dualdeck-host-service is missing from this install --" >&2
    echo "re-download the release archive." >&2
    exit 1
fi
if [[ ! -x "${host_root}/azahar" ]]; then
    echo "error: host/azahar is missing from this install -- re-download the" >&2
    echo "release archive." >&2
    exit 1
fi

# shellcheck source=scripts/lib/adapter_socket_probe.sh
source ./adapter_socket_probe.sh

# See run-host.sh's identical MELONDS_REMOTE_VERSION comment -- same
# central VERSION file, read the same way.
export AZAHAR_REMOTE_VERSION="$(cat "$(dirname "${host_root}")/VERSION" 2>/dev/null || true)"

auth_token_args=()
if [[ -n "${AZAHAR_REMOTE_AUTH_TOKEN:-}" ]]; then
    auth_token_args=(--auth-token "${AZAHAR_REMOTE_AUTH_TOKEN}")
fi

probe_or_spawn_adapter_socket "${HOME}/.config/dualdeck/run/azahar-adapter.sock" \
    "${host_root}/internal/dualdeck-host-service" "${HOME}/.config/melonds-remote" \
    "${AZAHAR_REMOTE_VERSION}" "${auth_token_args[@]}"
adapter_socket="${ADAPTER_SOCKET}"
if [[ -n "${HOST_SERVICE_PID}" ]]; then
    # Not exec'd below, same reason as run-host.sh's host-control branch:
    # this trap needs to still be able to run once Azahar exits -- only
    # meaningful when probe_or_spawn_adapter_socket() actually spawned a
    # private Host Service above; nothing to clean up here if this
    # connected to an already-running persistent daemon instead.
    trap 'kill "${HOST_SERVICE_PID}" 2>/dev/null || true' EXIT
fi

export AZAHAR_REMOTE_ENABLE=1
export AZAHAR_REMOTE_ADAPTER_SOCKET="${adapter_socket}"
# Performance tuning (optional): set AZAHAR_REMOTE_CAPTURE_FPS (1-60,
# default 60) before running this script to change how often the video
# capture loop polls for a new frame -- no rebuild needed, e.g.
# `AZAHAR_REMOTE_CAPTURE_FPS=30 ./dualdeck-host.sh` if 60 turns
# out to visibly affect the game's own performance on your hardware.
# Also AZAHAR_REMOTE_CAPTURE_SCALE (1-10): overrides how many multiples
# of the bottom screen's native 320x240 get streamed, e.g.
# `AZAHAR_REMOTE_CAPTURE_SCALE=2 ./dualdeck-host.sh` for 640x480. Not
# needed just to match Azahar's own "Internal Resolution" graphics
# setting -- the stream already follows that automatically now (see
# docs/known-limitations.md's capture-scale-follows-resolution-factor
# entry); only set this to stream at a different resolution than what's
# configured there, e.g. a sharper local picture than is worth sending
# over a particular link.
# Already inherited by the exec below with no extra wiring needed --
# see docs/known-limitations.md's performance-tuning entry.

# Works around a known class of Qt6-on-Linux crash (reported for many
# Qt6 apps, not specific to Azahar): a GTK3 platform-theme bug in
# window parenting can crash the process the moment a native file
# dialog opens -- exactly the "crashes when browsing files to pick a
# ROM" symptom this project's own user hit with File > Load File. An
# empty QT_QPA_PLATFORMTHEME disables that native GTK integration
# entirely, so Qt falls back to its own built-in (non-native) file
# dialog implementation, which isn't affected. The only downside is
# cosmetic: dialogs render in Qt's own style instead of matching the
# desktop's native GTK theme.
export QT_QPA_PLATFORMTHEME=""
"${host_root}/azahar" "$@"
WRAP
chmod +x "${pkg_dir}/host/internal/run-host-azahar.sh"

cat > "${pkg_dir}/host/internal/run-host-cemu.sh" <<'WRAP'
#!/usr/bin/env bash
# Runs the patched Cemu binary (Nintendo Wii U) -- see
# host/cemu-patches/README.md and
# docs/adr/0001-host-service-and-adapter-architecture.md in the
# DualDeck repository this was built from.
#
# Same out-of-process-only shape as run-host-azahar.sh: Cemu has no
# in-process device-approval dialog either, so this script always
# starts the standalone Host Service in the background first, which
# pops the same zero-typing kdialog Yes/No approval prompt melonDS's
# own in-process dialog gives you (see kdialog_approval_prompt.h),
# unless CEMU_REMOTE_AUTH_TOKEN is set, in which case that static
# shared secret is used instead. Normally launched via
# ../dualdeck-host.sh's "Launch..." menu, not directly.
#
# Pick a game through Cemu's own game list / File > Load once it opens,
# same as any other Cemu launch. The GamePad/DRC screen streams to the
# DualDeck client; the TV output stays on this host's own display, same
# "host shows one screen, client shows the other" split as melonDS/
# Azahar.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
host_root="$(cd .. && pwd)"

# shellcheck source=scripts/lib/ensure-packages.sh
source ./ensure-packages.sh

ensure_packages "cemu runtime" \
    "libvulkan1 freeglut3 libgtk-3-0 libpulse0 libsecret-1-0 libsystemd0 libbluetooth3 libgcrypt20 libusb-1.0-0" \
    "vulkan-loader freeglut gtk3 pulseaudio-libs libsecret systemd-libs bluez-libs libgcrypt libusb1" \
    "vulkan-icd-loader freeglut gtk3 libpulse libsecret systemd bluez-libs libgcrypt libusb" \
    || echo "warning: could not verify/install Cemu runtime libraries automatically; see docs/troubleshooting.md" >&2

if [[ -f /run/ostree-booted ]] || command -v rpm-ostree >/dev/null 2>&1; then
    echo "Note: this looks like an immutable (rpm-ostree) system, e.g. Bazzite -- Cemu's" >&2
    echo "Distrobox launch path isn't built yet (see host/cemu-patches/README.md), so" >&2
    echo "this may fail if a required library isn't already present on the base image." >&2
fi

if [[ ! -x "${host_root}/internal/dualdeck-host-service" ]]; then
    echo "error: host/internal/dualdeck-host-service is missing from this install --" >&2
    echo "re-download the release archive." >&2
    exit 1
fi
if [[ ! -x "${host_root}/cemu" ]]; then
    echo "error: host/cemu is missing from this install -- re-download the" >&2
    echo "release archive." >&2
    exit 1
fi

# shellcheck source=scripts/lib/adapter_socket_probe.sh
source ./adapter_socket_probe.sh

# See run-host.sh's identical MELONDS_REMOTE_VERSION comment -- same
# central VERSION file, read the same way.
export CEMU_REMOTE_VERSION="$(cat "$(dirname "${host_root}")/VERSION" 2>/dev/null || true)"

auth_token_args=()
if [[ -n "${CEMU_REMOTE_AUTH_TOKEN:-}" ]]; then
    auth_token_args=(--auth-token "${CEMU_REMOTE_AUTH_TOKEN}")
fi

probe_or_spawn_adapter_socket "${HOME}/.config/dualdeck/run/cemu-adapter.sock" \
    "${host_root}/internal/dualdeck-host-service" "${HOME}/.config/melonds-remote" \
    "${CEMU_REMOTE_VERSION}" "${auth_token_args[@]}"
adapter_socket="${ADAPTER_SOCKET}"
if [[ -n "${HOST_SERVICE_PID}" ]]; then
    # Not exec'd below, same reason as run-host.sh's host-control branch:
    # this trap needs to still be able to run once Cemu exits -- only
    # meaningful when probe_or_spawn_adapter_socket() actually spawned a
    # private Host Service above; nothing to clean up here if this
    # connected to an already-running persistent daemon instead.
    trap 'kill "${HOST_SERVICE_PID}" 2>/dev/null || true' EXIT
fi

export CEMU_REMOTE_ENABLE=1
export CEMU_REMOTE_ADAPTER_SOCKET="${adapter_socket}"
# Performance tuning (optional): set CEMU_REMOTE_CAPTURE_FPS (1-60,
# default 30) before running this script to change how often the
# GamePad/TV video capture is allowed to run -- no rebuild needed, e.g.
# `CEMU_REMOTE_CAPTURE_FPS=20 ./dualdeck-host.sh` if 30 turns out to
# visibly affect the game's own performance on your hardware (Vulkan's
# capture path is a real GPU sync every time it runs, see
# host/cemu-patches/README.md). Already inherited by the exec below
# with no extra wiring needed.
"${host_root}/cemu" "$@"
WRAP
chmod +x "${pkg_dir}/host/internal/run-host-cemu.sh"

cat > "${pkg_dir}/host/internal/launch-custom-emulator.sh" <<'WRAP'
#!/usr/bin/env bash
# Launches a custom-patched emulator binary the user built themselves
# (via scripts/patch-existing-emulator.sh in the DualDeck repository, or
# manually following its same steps) instead of the bundled melonDS/
# Azahar binaries -- for anyone who already has an emulator set up
# elsewhere and doesn't want a separate DualDeck-managed copy alongside
# it. Normally launched via ../../dualdeck-host.sh's "Launch..."
# menu's "Custom" choice, not directly.
#
# The chosen path and system type are remembered in
# ~/.config/dualdeck/custom-emulator.conf so this only has to be
# configured once; pass --reconfigure to point at a different binary or
# change the system type.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
host_root="$(cd .. && pwd)"

config_dir="${HOME}/.config/melonds-remote"
config_file="${config_dir}/custom-emulator.conf"
mkdir -p "${config_dir}"

have_kdialog() { command -v kdialog >/dev/null 2>&1; }

prompt_path() {
    if have_kdialog; then
        kdialog --title "DualDeck Host" --getopenfilename "${HOME}" 2>/dev/null || true
    else
        read -rp "Path to your patched emulator binary: " path >&2
        echo "${path}"
    fi
}

prompt_type() {
    if have_kdialog; then
        kdialog --title "DualDeck Host" --menu "Which system is this?" \
            ds "Nintendo DS (melonDS-based)" \
            n3ds "Nintendo 3DS (Azahar-based)" \
            wiiu "Nintendo Wii U (Cemu-based)" \
            2>/dev/null || true
    else
        read -rp "Is this a (d)S-based, (3)DS-based, or (w)ii U-based emulator? [d/3/w]: " t >&2
        case "${t}" in
            3) echo "n3ds" ;;
            w) echo "wiiu" ;;
            *) echo "ds" ;;
        esac
    fi
}

if [[ "${1:-}" == "--reconfigure" ]]; then
    rm -f "${config_file}"
    shift
fi

if [[ ! -f "${config_file}" ]]; then
    path="$(prompt_path)"
    if [[ -z "${path}" || ! -x "${path}" ]]; then
        echo "error: no valid, executable binary path given." >&2
        exit 1
    fi
    type="$(prompt_type)"
    if [[ -z "${type}" ]]; then
        echo "error: no system type chosen." >&2
        exit 1
    fi
    {
        echo "type=${type}"
        echo "path=${path}"
    } > "${config_file}"
    chmod 600 "${config_file}"
fi

# shellcheck disable=SC1090
source "${config_file}"

if [[ ! -x "${path}" ]]; then
    echo "error: ${path} no longer exists or isn't executable -- run this again with" >&2
    echo "--reconfigure to point at a different binary." >&2
    exit 1
fi

case "${type}" in
    ds)
        # A custom melonDS-based build understands MELONDS_REMOTE_ENABLE
        # directly (its own patched-in in-process server + interactive
        # device-approval dialog, same as the bundled host/melonDS) --
        # no Host Service to start here.
        export MELONDS_REMOTE_ENABLE=1
        export MELONDS_REMOTE_VERSION="$(cat "$(dirname "${host_root}")/VERSION" 2>/dev/null || true)"
        exec "${path}" "$@"
        ;;
    n3ds)
        if [[ ! -x "${host_root}/internal/dualdeck-host-service" ]]; then
            echo "error: host/internal/dualdeck-host-service is missing from this" >&2
            echo "install -- re-download the release archive." >&2
            exit 1
        fi
        run_dir="${HOME}/.config/dualdeck/run"
        mkdir -p "${run_dir}"
        adapter_socket="${run_dir}/custom-adapter.sock"
        rm -f "${adapter_socket}"

        export AZAHAR_REMOTE_VERSION="$(cat "$(dirname "${host_root}")/VERSION" 2>/dev/null || true)"
        # No --auth-token by default: an unrecognized device pops the same
        # zero-typing kdialog Yes/No approval prompt melonDS's in-process
        # dialog gives you (see kdialog_approval_prompt.h). ${token:-} only
        # exists for config files written by older releases that generated
        # one automatically.
        auth_token_args=()
        if [[ -n "${token:-}" ]]; then
            auth_token_args=(--auth-token "${token}")
        fi
        # See build-release.sh's bundle_library_dependencies() call for
        # this binary -- host/internal/lib ships its runtime deps
        # (libturbojpeg) so it doesn't depend on the host having it.
        env LD_LIBRARY_PATH="${host_root}/internal/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
            "${host_root}/internal/dualdeck-host-service" --adapter-ipc --adapter-socket "${adapter_socket}" \
            --state-dir "${HOME}/.config/melonds-remote" "${auth_token_args[@]}" \
            --app-version "${AZAHAR_REMOTE_VERSION}" &
        host_service_pid=$!
        trap 'kill "${host_service_pid}" 2>/dev/null || true' EXIT
        sleep 0.5

        export AZAHAR_REMOTE_ENABLE=1
        export AZAHAR_REMOTE_ADAPTER_SOCKET="${adapter_socket}"
        # See internal/run-host-azahar.sh's identical comment: works
        # around a known Qt6-on-Linux native-file-dialog crash by
        # disabling the GTK3 platform theme integration that triggers
        # it (cosmetic-only downside).
        export QT_QPA_PLATFORMTHEME=""
        "${path}" "$@"
        ;;
    wiiu)
        if [[ ! -x "${host_root}/internal/dualdeck-host-service" ]]; then
            echo "error: host/internal/dualdeck-host-service is missing from this" >&2
            echo "install -- re-download the release archive." >&2
            exit 1
        fi
        run_dir="${HOME}/.config/dualdeck/run"
        mkdir -p "${run_dir}"
        adapter_socket="${run_dir}/custom-adapter.sock"
        rm -f "${adapter_socket}"

        export CEMU_REMOTE_VERSION="$(cat "$(dirname "${host_root}")/VERSION" 2>/dev/null || true)"
        # Same zero-typing kdialog approval as the n3ds case above --
        # see that case's identical comment.
        auth_token_args=()
        if [[ -n "${token:-}" ]]; then
            auth_token_args=(--auth-token "${token}")
        fi
        # See the n3ds case's identical comment above.
        env LD_LIBRARY_PATH="${host_root}/internal/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
            "${host_root}/internal/dualdeck-host-service" --adapter-ipc --adapter-socket "${adapter_socket}" \
            --state-dir "${HOME}/.config/melonds-remote" "${auth_token_args[@]}" \
            --app-version "${CEMU_REMOTE_VERSION}" &
        host_service_pid=$!
        trap 'kill "${host_service_pid}" 2>/dev/null || true' EXIT
        sleep 0.5

        export CEMU_REMOTE_ENABLE=1
        export CEMU_REMOTE_ADAPTER_SOCKET="${adapter_socket}"
        "${path}" "$@"
        ;;
    *)
        echo "error: unknown system type '${type}' in ${config_file} -- re-run with" >&2
        echo "--reconfigure." >&2
        exit 1
        ;;
esac
WRAP
chmod +x "${pkg_dir}/host/internal/launch-custom-emulator.sh"

cat > "${pkg_dir}/host/internal/launch-emudeck-integration.sh" <<'WRAP'
#!/usr/bin/env bash
# Entry point for ../../dualdeck-host.sh's "Patch my EmuDeck-installed
# emulators" menu choice -- runs the bundled
# emudeck-integration/scripts/emudeck-replace-in-place.sh (see
# scripts/build-release.sh's "Bundling EmuDeck integration tool" step in
# the DualDeck repository, and docs/known-limitations.md's Phase A
# entry there for the full design).
#
# Unlike every other dualdeck-host.sh menu choice, this one downloads and
# installs over files EmuDeck itself manages, with its own per-emulator
# y/N confirmation prompts read from the terminal -- not a quick
# kdialog-driven action. Two things this needs that no other menu choice
# does, both from real user feedback:
#   1. Which emulator(s) to patch -- asked up front and passed through
#      as --emulator flags, instead of always patching every EmuDeck
#      install found.
#   2. The terminal window must NOT vanish the instant the tool exits
#      (whether it succeeds or fails) -- the whole point of running in
#      a visible terminal is so download/install progress and prompts can
#      be read, which a window that closes itself immediately defeats.
#      Building a small wrapper script that pauses on a "Press Enter to
#      close" prompt afterward is what actually fixes this, since a bare
#      `konsole -e sometool` still closes konsole the moment sometool
#      exits regardless of what sometool itself printed.
#
# If this script is already running with a visible terminal attached
# (e.g. launched from a terminal, or a file manager that runs .sh files
# "in a terminal"), that terminal is reused directly. Otherwise
# (typically: launched from Steam/Gaming Mode, which attaches no
# terminal at all) this re-launches inside a terminal emulator window,
# falling back to a kdialog message with the exact command to run
# manually if no terminal emulator can be found.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
host_root="$(cd .. && pwd)"

have_kdialog() { command -v kdialog >/dev/null 2>&1; }

tool="${host_root}/emudeck-integration/scripts/emudeck-replace-in-place.sh"
if [[ ! -x "${tool}" ]]; then
    msg="The EmuDeck integration tool is missing from this install (host/emudeck-integration/scripts/emudeck-replace-in-place.sh) -- re-download the release archive."
    if have_kdialog; then
        kdialog --title "DualDeck Host" --error "${msg}" 2>/dev/null || true
    else
        echo "error: ${msg}" >&2
    fi
    exit 1
fi

# ---- Which emulators? ----
choose_emulators() {
    if have_kdialog; then
        kdialog --title "DualDeck Host" --separate-output --checklist \
            "Which emulators should be patched?" \
            melonds "Nintendo DS (melonDS)" on \
            azahar "Nintendo 3DS (Azahar)" on \
            cemu "Nintendo Wii U (Cemu)" on \
            2>/dev/null || true
    else
        {
            echo "Which emulators should be patched?"
            echo "  1) All (default)"
            echo "  2) Nintendo DS (melonDS) only"
            echo "  3) Nintendo 3DS (Azahar) only"
            echo "  4) Nintendo Wii U (Cemu) only"
        } >&2
        local choice=""
        # < /dev/tty: this script can itself be reached via a chain
        # that started as a piped/non-interactive invocation -- see
        # DualDeck-Installer.sh's identical fix for the same reason.
        # Note the pre-assigned "" above: under `set -u`, `local choice`
        # alone leaves it genuinely unbound (not just empty) until
        # read succeeds, so a failed/no-op read (e.g. no /dev/tty at
        # all) would otherwise crash the case statement below instead
        # of falling through to its default.
        read -rp "Choice [1-4, Enter for all]: " choice < /dev/tty || true
        case "${choice}" in
            2) echo "melonds" ;;
            3) echo "azahar" ;;
            4) echo "cemu" ;;
            ""|1) printf '%s\n' melonds azahar cemu ;;
            *) ;;
        esac
    fi
}

mapfile -t selected_emulators < <(choose_emulators)
if [[ "${#selected_emulators[@]}" -eq 0 ]]; then
    exit 0 # cancelled / nothing selected
fi

emulator_args=()
for e in "${selected_emulators[@]}"; do
    emulator_args+=(--emulator "${e}")
done

# ---- Keep the window open afterward? ----
keep_open=1
if have_kdialog; then
    kdialog --title "DualDeck Host" --yesno "Keep this window open after finishing so you can read the output? (Recommended)" 2>/dev/null || keep_open=0
else
    reply=""
    read -rp "Keep this window open after finishing so you can read the output? [Y/n] " reply < /dev/tty || true
    [[ "${reply}" =~ ^[Nn]$ ]] && keep_open=0
fi

# ---- Wrapper: runs the tool, then (if requested) pauses on a "Press
# Enter" prompt regardless of success/failure before the window closes.
# Deliberately NOT `set -e` in this generated wrapper -- with it, a
# failing tool invocation would skip straight past the pause below
# instead of leaving the error visible, defeating the entire point.
run_wrapper="$(mktemp --suffix=.sh)"
{
    echo '#!/usr/bin/env bash'
    printf '%q ' "${tool}" "${emulator_args[@]}"
    echo
    echo 'status=$?'
    if [[ "${keep_open}" -eq 1 ]]; then
        echo 'echo'
        echo 'if [ "${status}" -eq 0 ]; then echo "Finished."; else echo "Finished with an error (exit code ${status}) -- see the output above."; fi'
        echo 'echo "Press Enter to close this window..."'
        echo 'read -r'
    fi
    echo 'rm -f "$0"'
    echo 'exit "${status}"'
} > "${run_wrapper}"
chmod +x "${run_wrapper}"

# Already has a real terminal (stdout is a tty) -- just run it here,
# same as every other menu choice in this script.
if [[ -t 1 ]]; then
    exec "${run_wrapper}"
fi

# No terminal attached -- re-launch inside whichever terminal emulator
# is available, trying the ones most likely to exist on SteamOS/Bazzite
# (both KDE Plasma, so konsole) first.
for term_cmd in "konsole -e" "xterm -e" "x-terminal-emulator -e" "gnome-terminal --"; do
    # shellcheck disable=SC2086
    set -- ${term_cmd}
    term_bin="$1"
    if command -v "${term_bin}" >/dev/null 2>&1; then
        exec "$@" "${run_wrapper}"
    fi
done

rm -f "${run_wrapper}"
manual_cmd="${tool}"
for a in "${emulator_args[@]}"; do
    manual_cmd="${manual_cmd} $(printf '%q' "${a}")"
done
msg="This needs a terminal window to show download progress and ask y/N
before replacing each emulator, but no terminal emulator (konsole,
xterm, gnome-terminal) was found to open one automatically.

Open a terminal yourself and run:
${manual_cmd}"
if have_kdialog; then
    kdialog --title "DualDeck Host" --error "${msg}" 2>/dev/null || true
else
    echo "error: ${msg}" >&2
fi
exit 1
WRAP
chmod +x "${pkg_dir}/host/internal/launch-emudeck-integration.sh"

cat > "${pkg_dir}/host/internal/install-host-distrobox.sh" <<'WRAP'
#!/usr/bin/env bash
# Runs the DualDeck host inside a Distrobox container, for
# immutable-filesystem systems (Bazzite, other rpm-ostree/Fedora Atomic
# derivatives) where run-host.sh can't install missing runtime libraries
# directly -- see docs/bazzite-host-setup.md. On a regular (non-immutable)
# Linux system, just use run-host.sh instead; this refuses to run there
# rather than needlessly creating a container. Normally launched via
# ../../dualdeck-host.sh's "Launch now"/"Add to Steam" menu
# choices, not directly.
#
# Safe to re-run any time, including from a newer release's extracted
# archive: it always re-syncs the whole host/ directory (this script's
# grandparent -- the entry-point script and binary alongside it, plus
# this internal/ directory) into a fixed location
# (~/.config/dualdeck/install/) and re-installs into the same
# Distrobox container (dnf skips packages already present), so updating
# is just "download the new release, run this again" -- no need to
# recreate the container or redo any setup by hand. After the first run
# you can also launch/update straight from that central directory
# instead of keeping the original download around. See
# uninstall-host-distrobox.sh to remove everything this creates.
#
# Update failures leave the previous install usable (GitHub issue #10):
# the new files are staged in a separate directory first and only
# swapped into place once staging AND the container package install both
# succeed -- if either fails partway, the working install from before
# this run is untouched, not deleted first and then possibly left
# missing. The swapped-out previous install is kept as one backup
# generation (*.previous) rather than deleted outright, in case you need
# to manually go back to it.
#
# Pass --install-only to prepare everything (central directory, Distrobox
# container, packages) without actually launching melonDS -- used by
# install-steam-shortcut.sh so registering the Steam shortcut doesn't
# also immediately start the emulator.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
host_root="$(cd .. && pwd)"
# shellcheck source=scripts/lib/host_firewall.sh
source ./host_firewall.sh

if [[ ! -f /run/ostree-booted ]] && ! command -v rpm-ostree >/dev/null 2>&1; then
    echo "This doesn't look like an immutable (rpm-ostree) system -- just run" >&2
    echo "./run-host.sh directly instead, no container needed." >&2
    exit 1
fi

# Host-control mode (GitHub issue #4) doesn't need a Distrobox container
# at all -- dualdeck-host-service links no Qt/SDL, only libturbojpeg,
# now bundled alongside it (see build-release.sh's
# bundle_library_dependencies() call and its own comment on the real
# Bazzite bug this fixes). This used to fail loudly here instead, on the
# now-outdated assumption that it needed the same GUI-library-heavy
# container melonDS itself does; launch-host.sh no longer even routes
# Host Control launches through this script, but delegate rather than
# error in case something still invokes this directly (e.g. a stale
# doc, or a manual run) -- ./run-host.sh handles it correctly on its
# own, container or not.
if [[ "${DUALDECK_HOST_CONTROL:-0}" == "1" ]]; then
    exec ./run-host.sh "$@"
fi

if ! command -v distrobox >/dev/null 2>&1; then
    echo "error: 'distrobox' not found. Bazzite ships it by default; on other" >&2
    echo "rpm-ostree systems, install it first -- see" >&2
    echo "https://github.com/89luca89/distrobox" >&2
    exit 1
fi

# Real user report, 2026-08-01 (Bazzite HTPC, launched via Steam
# shortcut): the client never saw this host at all -- not "discovered
# but refused," just nothing. Root cause: this script is normally
# launched from a Steam shortcut, so Steam's own LD_PRELOAD (overlay-
# injection libs, e.g. gameoverlayrenderer.so) is present in this
# process's environment and distrobox enter forwards it into the
# container by default. That crashes `env MELONDS_REMOTE_ENABLE=1 ...`
# below outright (exit 127, "error while loading shared libraries:
# libGL.so.1") before melonDS ever starts -- identical root cause,
# same fix, as scripts/emudeck-replace-in-place.sh's
# run_in_distrobox_build_container() (see docs/known-limitations.md's
# 2026-08-01 entry on that one). melonDS silently never launching
# explains the symptom exactly: no host process means no listening
# ports, which means nothing for the client to discover no matter how
# correct the firewall setup is.
unset LD_PRELOAD LD_LIBRARY_PATH

install_only=0
if [[ "${1:-}" == "--install-only" ]]; then
    install_only=1
    shift
fi

# Keep in sync with the same paths in uninstall-host-distrobox.sh,
# install-steam-shortcut.sh, uninstall-steam-shortcut.sh, and
# docs/bazzite-host-setup.md's description of this path.
central_install_dir="${HOME}/.config/dualdeck/install"
staging_dir="${central_install_dir}.new"
previous_dir="${central_install_dir}.previous"
container_name="dualdeck-host"

# One-time melonDS-Remote -> DualDeck rebrand cleanup: remove a leftover
# container from before the rename so this doesn't end up with two
# (the old one orphaned, plus a freshly created "dualdeck-host"). No-op
# if it doesn't exist.
if command -v distrobox >/dev/null 2>&1 && distrobox list 2>/dev/null | grep -qw "melonds-remote-host"; then
    echo "Removing old Distrobox container \"melonds-remote-host\" (renamed to \"${container_name}\") ..."
    distrobox rm "melonds-remote-host" --force 2>/dev/null || true
fi

already_central=0
if [[ "${host_root}" == "${central_install_dir}" ]]; then
    # Already running from the central directory itself -- e.g.
    # launch-host.sh re-invoking this script on every Steam-shortcut
    # launch, or a user double-clicking this exact copy a second time.
    # Re-copying the directory into itself on every single launch would
    # just waste time (and disk churn copying the melonDS binary) for no
    # benefit, so skip straight to the container/package step below --
    # there's nothing new to stage or swap in.
    already_central=1
    echo "Already installed at ${central_install_dir} -- skipping the re-copy step."
else
    echo "Staging host files at ${staging_dir} ..."
    rm -rf "${staging_dir}"
    mkdir -p "${staging_dir}"
    cp -a "${host_root}/." "${staging_dir}/"

    # So check-for-updates.sh keeps working via dualdeck-host.sh's
    # "../check-for-updates.sh" reference even when a copy of that menu
    # script is later run from inside the central directory itself
    # (e.g. after the original downloaded archive has been deleted) --
    # these two live one level up from host_root, as siblings of
    # install/, so they survive the install/install.previous swap below
    # untouched. Best-effort: a missing source (e.g. this got invoked
    # some other way) shouldn't fail the whole install over a
    # convenience file.
    cp "$(dirname "${host_root}")/check-for-updates.sh" "$(dirname "${central_install_dir}")/check-for-updates.sh" 2>/dev/null || true
    cp "$(dirname "${host_root}")/VERSION" "$(dirname "${central_install_dir}")/VERSION" 2>/dev/null || true
fi

echo "Creating/reusing Distrobox container \"${container_name}\" (Fedora-based) ..."
distrobox create --name "${container_name}" --image fedora:latest --yes

echo "Installing runtime libraries inside the container ..."
distrobox enter "${container_name}" -- sudo dnf install -y \
    libcurl-devel libpcap-devel SDL2-devel libarchive-devel enet-devel libzstd-devel faad2-devel \
    qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtmultimedia-devel qt6-qtsvg-devel \
    libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXfixes-devel libXi-devel libXScrnSaver-devel \
    wayland-devel libxkbcommon-devel libdrm-devel mesa-libgbm-devel libdecor-devel

# Firewalld runs at the host OS level, not per-container -- opening
# these ports here (not just in install-steam-shortcut.sh, which
# already calls this script as a sub-step on immutable systems) also
# covers anyone running this script standalone. Best-effort, never
# fatal, and idempotent if install-steam-shortcut.sh's own call already
# did this a moment ago -- see scripts/lib/host_firewall.sh.
ensure_host_firewall_ports || true

# Only reached if everything above succeeded (set -e) -- safe to swap in
# the new install now (nothing to swap if already_central -- the
# directory already IS the install). Keeps just one backup generation,
# not unbounded.
if [[ "${already_central}" -eq 0 ]]; then
    echo "Activating the new install ..."
    rm -rf "${previous_dir}"
    if [[ -d "${central_install_dir}" ]]; then
        mv "${central_install_dir}" "${previous_dir}"
    fi
    mv "${staging_dir}" "${central_install_dir}"
fi

if [[ "${install_only}" -eq 1 ]]; then
    echo "Install complete (not launching melonDS -- run this script again without" \
         "--install-only, or use the Steam shortcut, to launch it)."
    exit 0
fi

echo "Launching the host inside the container ..."
# See run-host.sh's identical MELONDS_REMOTE_VERSION comment -- read from
# the same central-directory sibling copy of VERSION staged above.
host_app_version="$(cat "$(dirname "${central_install_dir}")/VERSION" 2>/dev/null || true)"
exec distrobox enter "${container_name}" -- env MELONDS_REMOTE_ENABLE=1 \
    "MELONDS_REMOTE_VERSION=${host_app_version}" "${central_install_dir}/melonDS" "$@"
WRAP
chmod +x "${pkg_dir}/host/internal/install-host-distrobox.sh"

# Real user request, 2026-08-01: "Host control should be a constant
# server from the host, being toggled via the eventual decky menu
# plugin or the DualDeck Host GUI. Steam should not keep registering it
# as a game running in the background." Today's "Host control only"
# menu choice (run-host.sh's DUALDECK_HOST_CONTROL branch) execs
# straight into dualdeck-host-service as the literal foreground process
# of whatever launched it (a Steam shortcut, or this menu script) --
# Steam's own "is this game still running" tracking watches that exact
# process, with no separate PID/session layer to decouple from. A
# systemd --user service is a completely independent process tree Steam
# never launches and never tracks at all, closing that gap directly
# rather than trying to make Steam stop noticing a process it's already
# watching.
cat > "${pkg_dir}/host/internal/host-control-daemon.sh" <<'WRAP'
#!/usr/bin/env bash
# The actual command the persistent Host Control systemd --user service
# (dualdeck-host-control.service, installed by
# install-host-control-daemon.sh) runs. A thin wrapper, not the unit's
# ExecStart directly, because dualdeck-host-service takes CLI flags
# (--auth-token, --app-version, --state-dir), not environment variables,
# and the unit file itself stays static/trivial.
#
# Deliberately never passes --adapter-socket: dualdeck-host-service then
# binds adapter-sdk/ipc/src/socket_path.cpp's defaultAdapterSocketPath()
# (the shared, well-known path) -- the exact socket every emulator
# launch path (run-host.sh, run-host-azahar.sh, run-host-cemu.sh, and
# the prebuilt AppImages' own AppRun) now probes first via
# scripts/lib/adapter_socket_probe.sh, so any of them connects to this
# daemon automatically and ModeCoordinator switches this same session
# from HostControl to Emulation mode with no re-launch/re-config needed
# anywhere. This is the one detail tying the persistent daemon to
# genuinely emulator-agnostic mode-switching.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
host_root="$(cd .. && pwd)"

export DUALDECK_HOST_CONTROL_VERSION="$(cat "$(dirname "${host_root}")/VERSION" 2>/dev/null || true)"

auth_token_args=()
if [[ -n "${DUALDECK_HOST_CONTROL_AUTH_TOKEN:-}" ]]; then
    auth_token_args=(--auth-token "${DUALDECK_HOST_CONTROL_AUTH_TOKEN}")
fi

# See build-release.sh's bundle_library_dependencies() call for this
# binary -- host/internal/lib ships its runtime deps (libturbojpeg) so
# it runs under systemd --user the same way on any host, immutable or
# not (the real Bazzite bug this fixes: this daemon previously couldn't
# even be installed on rpm-ostree systems because of it).
exec env LD_LIBRARY_PATH="${host_root}/internal/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
    "${host_root}/internal/dualdeck-host-service" --adapter-ipc \
    --state-dir "${HOME}/.config/melonds-remote" "${auth_token_args[@]}" \
    --app-version "${DUALDECK_HOST_CONTROL_VERSION}"
WRAP
chmod +x "${pkg_dir}/host/internal/host-control-daemon.sh"

cat > "${pkg_dir}/host/internal/install-host-control-daemon.sh" <<'WRAP'
#!/usr/bin/env bash
# Installs and enables the persistent Host Control systemd --user
# service (dualdeck-host-control.service) -- see host-control-
# daemon.sh's own comment for why this exists at all. Idempotent: safe
# to re-run on every install/update (matching install-steam-shortcut.sh's
# own convention). Normally invoked from ../../dualdeck-host.sh's "Host
# Control daemon" menu choice, not directly.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

error_log="${HOME}/.config/dualdeck/install.log"
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") install-host-control-daemon.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
}
trap 'ec=$?; on_error "${ec}" "${LINENO}" "${BASH_COMMAND}"' ERR

# Real Bazzite hardware report, 2026-08-02: this daemon used to refuse
# to install at all on immutable/rpm-ostree systems here, on the same
# now-outdated "needs a Distrobox container" assumption
# install-host-distrobox.sh's own DUALDECK_HOST_CONTROL check used to
# make (see that check's own updated comment) -- dualdeck-host-service
# links no Qt/SDL, only libturbojpeg, now bundled alongside it (see
# build-release.sh's bundle_library_dependencies() call), so it needs no
# container on any system. What this unit actually writes to
# (${unit_dir} below, under $HOME) and execs
# (host-control-daemon.sh -> the now-self-contained dualdeck-host-service)
# both work identically whether the base OS is immutable or not -- the
# only real prerequisite is a working systemd --user manager, checked
# next.
#
# Checks a *working* --user manager is actually reachable, not just that
# the systemctl binary exists -- a machine can have systemd installed
# without a lingering/active --user instance for this account (e.g. no
# graphical login has ever happened yet).
if ! command -v systemctl >/dev/null 2>&1 || ! systemctl --user status >/dev/null 2>&1; then
    echo "systemd --user isn't available on this system -- the persistent Host" >&2
    echo "Control daemon can't be installed here. Use the manual 'Host control" >&2
    echo "only' launch option from the menu instead." >&2
    exit 0
fi

unit_dir="${HOME}/.config/systemd/user"
mkdir -p "${unit_dir}"

# %h is systemd's own home-directory expansion (not a bash variable --
# this heredoc is deliberately quoted so nothing here is bash-expanded),
# so this unit keeps working correctly even if $HOME ever changes.
cat > "${unit_dir}/dualdeck-host-control.service" <<'EOF'
[Unit]
Description=DualDeck Host Control (persistent background service)
After=network.target

[Service]
Type=simple
ExecStart=%h/.config/dualdeck/install/internal/host-control-daemon.sh
Restart=on-failure
RestartSec=2

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload
echo "Host Control daemon unit installed. Enable it with:"
echo "  systemctl --user enable --now dualdeck-host-control.service"
WRAP
chmod +x "${pkg_dir}/host/internal/install-host-control-daemon.sh"

cat > "${pkg_dir}/host/internal/uninstall-host-control-daemon.sh" <<'WRAP'
#!/usr/bin/env bash
# Undoes install-host-control-daemon.sh: stops, disables, and removes
# the persistent Host Control systemd --user service. Safe to run even
# if it was never installed (every step degrades to a harmless no-op).
# Called both standalone (../../dualdeck-host.sh's "Stop & disable"
# menu choice) and from the same script's full "Remove from Steam /
# uninstall" flow, so a full uninstall doesn't leave this running
# against a now-deleted install.
set -euo pipefail

if command -v systemctl >/dev/null 2>&1; then
    systemctl --user disable --now dualdeck-host-control.service >/dev/null 2>&1 || true
fi
rm -f "${HOME}/.config/systemd/user/dualdeck-host-control.service"
if command -v systemctl >/dev/null 2>&1; then
    systemctl --user daemon-reload >/dev/null 2>&1 || true
fi
WRAP
chmod +x "${pkg_dir}/host/internal/uninstall-host-control-daemon.sh"

cat > "${pkg_dir}/host/internal/uninstall-host-distrobox.sh" <<'WRAP'
#!/usr/bin/env bash
# Undoes install-host-distrobox.sh: removes the Distrobox container it
# created and the central install directory it copied files into. Only
# ever touches things this project itself created -- ROMs, saves,
# firmware, and any other melonDS data all live in your normal shared
# home directory (Distrobox mounts it into the container automatically),
# never inside the container or the central install directory, so none
# of that is affected either way (GitHub issue #10: "uninstall removes
# only DualDeck-installed files... without deleting ROMs, saves,
# firmware, or unrelated melonDS data").
#
# Safe to re-run -- does nothing (not an error) if already uninstalled,
# or if install-host-distrobox.sh was never run at all.
set -uo pipefail

# Keep in sync with the same paths in install-host-distrobox.sh,
# install-steam-shortcut.sh, and uninstall-steam-shortcut.sh.
central_install_dir="${HOME}/.config/dualdeck/install"
container_name="dualdeck-host"

removed_anything=0

if command -v distrobox >/dev/null 2>&1 && distrobox list 2>/dev/null | grep -qw "${container_name}"; then
    echo "Removing Distrobox container \"${container_name}\" ..."
    distrobox rm "${container_name}" --force
    removed_anything=1
fi

for dir in "${central_install_dir}" "${central_install_dir}.new" "${central_install_dir}.previous"; do
    if [[ -d "${dir}" ]]; then
        echo "Removing ${dir} ..."
        rm -rf -- "${dir}"
        removed_anything=1
    fi
done

# check-for-updates.sh/VERSION are staged as siblings of install/ (see
# install-host-distrobox.sh/install-steam-shortcut.sh), not inside any
# of the three directories just removed above -- clean those up too.
for file in "$(dirname "${central_install_dir}")/check-for-updates.sh" "$(dirname "${central_install_dir}")/VERSION"; do
    if [[ -f "${file}" ]]; then
        rm -f -- "${file}"
        removed_anything=1
    fi
done

if [[ "${removed_anything}" -eq 0 ]]; then
    echo "Nothing installed -- already uninstalled, or install-host-distrobox.sh was never run."
fi
WRAP
chmod +x "${pkg_dir}/host/internal/uninstall-host-distrobox.sh"

cat > "${pkg_dir}/host/internal/launch-host.sh" <<'WRAP'
#!/usr/bin/env bash
# Single entry point for the host Steam Big Picture/Gaming Mode shortcut
# (GitHub issue #10: "the installed host can be launched from Steam Big
# Picture or Gaming Mode and accept a client connection"). Picks the
# right launch path depending on whether this is an immutable
# (rpm-ostree, e.g. Bazzite) system or a regular one, so the same
# shortcut works either way without the user needing to know which
# applies to their system. install-steam-shortcut.sh points the Steam
# shortcut's Exe at this script, never at melonDS or run-host.sh
# directly.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

# Real Bazzite hardware report, 2026-08-02: Host Control mode
# (DUALDECK_HOST_CONTROL=1) never worked on immutable systems at all --
# install-host-distrobox.sh flatly refused it (see that script's own
# comment), even though its own actual reason (needing Qt6/SDL2 that
# only a Distrobox container can provide) doesn't apply to it:
# dualdeck-host-service links no Qt/SDL at all, only libturbojpeg, which
# is now bundled alongside it (see build-release.sh's
# bundle_library_dependencies() call) precisely so it runs the same way
# on any host regardless of package-manager mutability. Route Host
# Control mode straight to run-host.sh even here -- it doesn't need a
# container -- and reserve the Distrobox path for an actual melonDS GUI
# launch, which still does.
if [[ "${DUALDECK_HOST_CONTROL:-0}" == "1" ]]; then
    exec ./run-host.sh "$@"
elif [[ -f /run/ostree-booted ]] || command -v rpm-ostree >/dev/null 2>&1; then
    exec ./install-host-distrobox.sh "$@"
else
    exec ./run-host.sh "$@"
fi
WRAP
chmod +x "${pkg_dir}/host/internal/launch-host.sh"

cat > "${pkg_dir}/host/dualdeck-host.sh" <<'WRAP'
#!/usr/bin/env bash
# The one thing to double-click to set up or run the DualDeck
# host -- shows a simple menu and delegates to whichever of the
# internal/ scripts actually applies, so a user never has to figure out
# which one they need (GitHub issue #10: "the normal path requires no
# terminal commands"). Those scripts still exist and still work
# standalone (e.g. for scripting or troubleshooting) -- this is just
# the single entry point a human actually needs to know about.
#
# Uses a graphical kdialog menu when available (SteamOS Desktop Mode
# and Bazzite are both KDE Plasma, where kdialog is standard) and falls
# back to a plain numbered prompt in a terminal otherwise.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

error_log="${HOME}/.config/dualdeck/install.log"
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") dualdeck-host.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "DualDeck Host" --error "Something went wrong: ${failing_cmd}
(exit code ${exit_code})

Details logged to:
${error_log}" 2>/dev/null || true
    fi
}
trap 'ec=$?; on_error "${ec}" "${LINENO}" "${BASH_COMMAND}"' ERR

have_kdialog() { command -v kdialog >/dev/null 2>&1; }

info() {
    if have_kdialog; then
        kdialog --title "DualDeck Host" --msgbox "$1" 2>/dev/null
    else
        echo
        echo "$1"
        echo
    fi
}

confirm() {
    if have_kdialog; then
        kdialog --title "DualDeck Host" --yesno "$1" 2>/dev/null
    else
        read -rp "$1 [y/N] " reply
        [[ "${reply}" =~ ^[Yy]$ ]]
    fi
}

choose_action() {
    # Real user request, 2026-08-01: "Host control should be a constant
    # server from the host, being toggled via the eventual decky menu
    # plugin or the DualDeck Host GUI. Steam should not keep registering
    # it as a game running in the background." -- the label reflects
    # current systemd --user state (mirrors choose_emulator()'s own
    # dynamic custom_label precedent below) so this menu doubles as a
    # status check, not just an action picker.
    local daemon_label="Host Control daemon (persistent, not running)"
    if command -v systemctl >/dev/null 2>&1 && \
       systemctl --user is-active --quiet dualdeck-host-control.service 2>/dev/null; then
        daemon_label="Host Control daemon (persistent, RUNNING)"
    fi

    if have_kdialog; then
        kdialog --title "DualDeck Host" --menu "What would you like to do?" \
            launch "Launch..." \
            hostcontrol-daemon "${daemon_label}" \
            steam-add "Add to Steam (Big Picture / Gaming Mode)" \
            steam-remove "Remove from Steam / uninstall" \
            update "Check for updates / update" \
            emudeck "Patch my EmuDeck-installed emulators (experimental)" \
            2>/dev/null || echo "cancel"
    else
        # All of this goes to stderr, not stdout -- the caller captures
        # this function's stdout as the actual selection
        # ("action=\"\$(choose_action)\"" below), so any of the menu
        # display text leaking onto stdout would get appended to that
        # and break the case match entirely.
        {
            echo "DualDeck Host"
            echo "  1) Launch..."
            echo "  2) ${daemon_label}"
            echo "  3) Add to Steam (Big Picture / Gaming Mode)"
            echo "  4) Remove from Steam / uninstall"
            echo "  5) Check for updates / update"
            echo "  6) Patch my EmuDeck-installed emulators (experimental)"
            echo "  7) Exit"
        } >&2
        read -rp "Choice [1-7]: " choice
        case "${choice}" in
            1) echo "launch" ;;
            2) echo "hostcontrol-daemon" ;;
            3) echo "steam-add" ;;
            4) echo "steam-remove" ;;
            5) echo "update" ;;
            6) echo "emudeck" ;;
            *) echo "cancel" ;;
        esac
    fi
}

# Real user request, 2026-08-01 (see choose_action()'s own comment): a
# persistent Host Control daemon, toggled via a real GUI now rather than
# only terminal systemctl commands, built so a future Decky Loader
# plugin has an obvious, trivial pair of commands to shell out to
# instead (systemctl --user start/stop dualdeck-host-control.service).
choose_host_control_daemon_action() {
    local active=0
    if command -v systemctl >/dev/null 2>&1 && \
       systemctl --user is-active --quiet dualdeck-host-control.service 2>/dev/null; then
        active=1
    fi

    if have_kdialog; then
        if [[ "${active}" -eq 1 ]]; then
            kdialog --title "DualDeck Host" --menu "Host Control daemon is RUNNING" \
                stop "Stop && disable" \
                status "Status" \
                2>/dev/null || echo "cancel"
        else
            kdialog --title "DualDeck Host" --menu "Host Control daemon is not running" \
                start "Enable && start now" \
                status "Status" \
                2>/dev/null || echo "cancel"
        fi
    else
        {
            if [[ "${active}" -eq 1 ]]; then
                echo "Host Control daemon is RUNNING"
                echo "  1) Stop & disable"
            else
                echo "Host Control daemon is not running"
                echo "  1) Enable & start now"
            fi
            echo "  2) Status"
            echo "  3) Back"
        } >&2
        read -rp "Choice [1-3]: " choice
        case "${choice}" in
            1) [[ "${active}" -eq 1 ]] && echo "stop" || echo "start" ;;
            2) echo "status" ;;
            *) echo "cancel" ;;
        esac
    fi
}

# GitHub issue "rework the host launcher so it does not boot into
# melonDS": picks which system/emulator to actually launch, instead of
# always going straight to melonDS. Azahar (3DS), Cemu (Wii U), and
# "host control only" all use the standalone Host Service's own
# kdialog-based device-approval popup (see
# internal/run-host-azahar.sh's/run-host-cemu.sh's comments and
# kdialog_approval_prompt.h) -- the same zero-typing flow melonDS's own
# in-process dialog already has.
choose_emulator() {
    local custom_conf="${HOME}/.config/dualdeck/custom-emulator.conf"
    local custom_label="Custom (patch my own emulator)"
    if [[ -f "${custom_conf}" ]]; then
        local custom_path
        custom_path="$(sed -n 's/^path=//p' "${custom_conf}")"
        [[ -n "${custom_path}" ]] && custom_label="Custom (${custom_path})"
    fi

    if have_kdialog; then
        kdialog --title "DualDeck Host" --menu "Which system?" \
            ds "Nintendo DS (melonDS)" \
            n3ds "Nintendo 3DS (Azahar, experimental)" \
            wiiu "Nintendo Wii U (Cemu, experimental)" \
            hostcontrol "Host control only -- no emulator (experimental)" \
            custom "${custom_label}" \
            2>/dev/null || echo "cancel"
    else
        {
            echo "Which system?"
            echo "  1) Nintendo DS (melonDS)"
            echo "  2) Nintendo 3DS (Azahar, experimental)"
            echo "  3) Nintendo Wii U (Cemu, experimental)"
            echo "  4) Host control only -- no emulator (experimental)"
            echo "  5) ${custom_label}"
            echo "  6) Back"
        } >&2
        read -rp "Choice [1-6]: " choice
        case "${choice}" in
            1) echo "ds" ;;
            2) echo "n3ds" ;;
            3) echo "wiiu" ;;
            4) echo "hostcontrol" ;;
            5) echo "custom" ;;
            *) echo "cancel" ;;
        esac
    fi
}

action="$(choose_action)"

case "${action}" in
    launch)
        emulator="$(choose_emulator)"
        case "${emulator}" in
            ds)
                # exec, not a plain call: this becomes the foreground
                # process, same as launching melonDS any other way
                # (Steam shortcut, double-clicking internal/run-host.sh
                # directly, etc.) -- no menu process left hanging
                # around behind it.
                exec ./internal/launch-host.sh
                ;;
            n3ds)
                # No auth token exported: an unrecognized device pops a
                # kdialog Yes/No approval prompt on this desktop instead
                # (see internal/run-host-azahar.sh's comment and
                # kdialog_approval_prompt.h) -- the same zero-typing flow
                # melonDS's own in-process dialog already has. Azahar has
                # no Distrobox launch path yet (see
                # internal/run-host-azahar.sh's own comment) -- called
                # directly, not through launch-host.sh's melonDS-specific
                # Distrobox-vs-plain dispatch.
                exec ./internal/run-host-azahar.sh
                ;;
            wiiu)
                # Same out-of-process-only shape as n3ds above -- see
                # internal/run-host-cemu.sh's comment and
                # host/cemu-patches/README.md for why Cemu has no
                # in-process device-approval path either. No Distrobox
                # launch path yet -- called directly, not through
                # launch-host.sh's melonDS-specific dispatch.
                exec ./internal/run-host-cemu.sh
                ;;
            hostcontrol)
                # Exported before the same launch-host.sh dispatch the
                # "ds" case above uses -- DUALDECK_HOST_CONTROL is
                # an environment variable, so run-host.sh (or
                # install-host-distrobox.sh's rejection check, on an
                # immutable system) sees it either way without
                # launch-host.sh itself needing to know this mode
                # exists. No auth token exported here either, same
                # zero-typing kdialog approval as the n3ds case above.
                export DUALDECK_HOST_CONTROL=1
                exec ./internal/launch-host.sh
                ;;
            custom)
                exec ./internal/launch-custom-emulator.sh
                ;;
            *)
                exit 0 # cancelled/back
                ;;
        esac
        ;;
    hostcontrol-daemon)
        daemon_action="$(choose_host_control_daemon_action)"
        case "${daemon_action}" in
            start)
                # Real Bazzite hardware report, 2026-08-02: this used to
                # always show the same generic "check install.log"
                # message on any failure, even when
                # install-host-control-daemon.sh had already printed the
                # real, specific reason to stderr (e.g. its old
                # immutable-system refusal) -- install.log never
                # contained that reason either, since a clean, expected
                # refusal isn't an ERR-trap failure. Capture both
                # commands' combined output and show it directly instead
                # of guessing, falling back to the old generic message
                # only if nothing was actually captured.
                if daemon_start_output="$(./internal/install-host-control-daemon.sh 2>&1 && \
                    systemctl --user enable --now dualdeck-host-control.service 2>&1)"; then
                    info "Host Control daemon started -- it now runs independently of Steam and stays up across reboots (once your desktop session's systemd --user manager comes up). Connect a client any time; launching a real emulator elsewhere still works exactly as before and switches this session to Emulation mode automatically."
                elif [[ -n "${daemon_start_output}" ]]; then
                    info "Could not start the Host Control daemon:

${daemon_start_output}"
                else
                    info "Could not start the Host Control daemon -- see ${error_log} for details, or check whether systemd --user is available on this system."
                fi
                ;;
            stop)
                if systemctl --user disable --now dualdeck-host-control.service 2>/dev/null; then
                    info "Host Control daemon stopped and disabled."
                else
                    info "Could not stop the Host Control daemon (it may not have been installed yet)."
                fi
                ;;
            status)
                status_output="$(systemctl --user status dualdeck-host-control.service --no-pager 2>&1 || true)"
                if have_kdialog; then
                    status_file="$(mktemp)"
                    echo "${status_output}" > "${status_file}"
                    kdialog --title "DualDeck Host Control daemon status" --textbox "${status_file}" 600 400 2>/dev/null || true
                    rm -f "${status_file}"
                else
                    echo "${status_output}"
                fi
                ;;
            *)
                exit 0 # cancelled/back
                ;;
        esac
        ;;
    steam-add)
        if ./internal/install-steam-shortcut.sh; then
            info "Added DualDeck Host to Steam. Restart Steam (or switch to Gaming Mode) to see it, and set its Controller Layout to a plain Gamepad template once it's there."
        fi
        # A failure here already logged and showed its own error dialog
        # (install-steam-shortcut.sh has the same error-trap pattern as
        # this script) -- nothing more to do.
        ;;
    steam-remove)
        if confirm "This removes the Steam shortcut, the installed files, and the Distrobox container if one was created. Your ROMs, saves, and firmware are never touched. Continue?"; then
            # Best-effort, before the files it depends on are removed
            # below -- a full uninstall should also tear down the
            # persistent Host Control daemon if one was ever installed,
            # not leave it running against a now-deleted install.
            ./internal/uninstall-host-control-daemon.sh 2>/dev/null || true
            if ./internal/uninstall-steam-shortcut.sh; then
                info "Removed."
            fi
        fi
        ;;
    update)
        update_report="$(../check-for-updates.sh)"
        if echo "${update_report}" | grep -q "update available:"; then
            latest_version="$(echo "${update_report}" | sed -n 's/.*update available: //p' | head -1)"
            if confirm "${update_report}

Install ${latest_version} now? This downloads it from GitHub and also adds/updates the Steam shortcut."; then
                # Captured (not just checked for success) so the message
                # below can tell whether the Steam shortcut's Exe/AppName/
                # LaunchOptions actually changed -- for a routine update
                # they never do (see steam_shortcut.py's shortcut_up_to_date()),
                # so apply-update.sh doesn't touch shortcuts.vdf at all and
                # there's nothing for a running Steam to need reloading.
                # Only mention restarting Steam when apply-update.sh's own
                # output says it actually wrote the file.
                if update_output="$(./internal/apply-update.sh 2>&1)"; then
                    if echo "${update_output}" | grep -q "Restart Steam"; then
                        info "Updated to ${latest_version}. Restart Steam (or switch to Gaming Mode) to see the change."
                    else
                        info "Updated to ${latest_version}."
                    fi
                fi
                # A failure here already logged and showed its own error
                # dialog (apply-update.sh has the same error-trap pattern
                # as this script) -- nothing more to do.
            fi
        else
            info "${update_report}"
        fi
        ;;
    emudeck)
        exec ./internal/launch-emudeck-integration.sh
        ;;
    *)
        exit 0
        ;;
esac
WRAP
chmod +x "${pkg_dir}/host/dualdeck-host.sh"

cat > "${pkg_dir}/host/internal/apply-update.sh" <<'WRAP'
#!/usr/bin/env bash
# Downloads the latest DualDeck release and installs it, by
# handing off to that release's own
# host/internal/install-steam-shortcut.sh --force -- reusing its
# already-verified stage-then-swap file safety and Distrobox/dnf-gated
# activation on immutable systems, rather than duplicating any of that
# logic here. This script's only job is fetching and extracting the
# new release archive. Normally invoked from
# ../../dualdeck-host.sh's "Check for updates" menu choice after
# the user confirms; also runnable standalone.
#
# Passes --force through to install-steam-shortcut.sh so an update
# doesn't silently do nothing just because Steam happens to be open --
# the confirmation prompt in the menu is the actual "are you sure" gate
# here, not that check. In practice --force rarely matters: since GitHub
# issue "atomic updates -- Steam shouldn't need to be closed to update",
# install-steam-shortcut.sh only writes shortcuts.vdf when the
# shortcut's Exe/AppName/StartDir/LaunchOptions actually differ from
# what's already there, which never happens for a routine version bump
# (see steam_shortcut.py's shortcut_up_to_date()) -- so a normal update
# never touches shortcuts.vdf at all and Steam never needs to be closed
# or restarted for it. If Steam genuinely was never set up on this
# machine, installing the Steam shortcut part fails visibly (its own
# error-trap logs and shows a dialog) while the files themselves are
# still updated either way, since that copy step doesn't depend on
# Steam at all.
#
# Only ever downloads from this exact, hardcoded GitHub Releases URL
# over HTTPS -- never anything derived from user input, an environment
# variable, or a config file.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

error_log="${HOME}/.config/dualdeck/install.log"
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") apply-update.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "DualDeck Host" --error "Updating failed: ${failing_cmd}
(exit code ${exit_code})

Details logged to:
${error_log}" 2>/dev/null || true
    fi
}
trap 'ec=$?; on_error "${ec}" "${LINENO}" "${BASH_COMMAND}"' ERR

if ! command -v curl >/dev/null 2>&1; then
    echo "error: curl is required to download updates -- install it, or download" >&2
    echo "the latest release manually from https://github.com/Crimson3076/DualDeck/releases/latest" >&2
    exit 1
fi

repo="Crimson3076/DualDeck"
download_url="https://github.com/${repo}/releases/latest/download/melonds-remote-linux-x86_64.tar.gz"

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

echo "Downloading the latest release..."
curl -fsSL --max-time 180 -o "${work_dir}/release.tar.gz" "${download_url}"

echo "Extracting..."
tar xzf "${work_dir}/release.tar.gz" -C "${work_dir}"

extracted_dir=""
for candidate in "${work_dir}"/melonds-remote-*; do
    [[ -d "${candidate}" ]] && extracted_dir="${candidate}" && break
done
if [[ -z "${extracted_dir}" ]]; then
    echo "error: couldn't find the extracted release directory" >&2
    exit 1
fi

# Real user request: "it should also reset on updates, so toggle off and
# back on if it was already running" -- checked *before* the install
# step below, since that step only replaces files on disk, it doesn't
# touch whatever copy of dualdeck-host-service the persistent daemon
# (if running) already has open. `systemctl --user restart` after the
# install is a single atomic "toggle off and back on," matching the
# request exactly. Guarded with `|| true`/an explicit warning rather
# than left to the ERR trap above: a failed restart here is real but
# distinct from an update itself failing (the update has already fully
# succeeded by this point), so it shouldn't produce the same "Updating
# failed" kdialog error as an actual download/install failure would.
daemon_was_active=0
if command -v systemctl >/dev/null 2>&1 && \
   systemctl --user is-active --quiet dualdeck-host-control.service 2>/dev/null; then
    daemon_was_active=1
fi

echo "Installing..."
# Not exec'd: the work_dir EXIT trap above must still fire to clean up
# the download afterward, which exec'ing over this process would skip.
"${extracted_dir}/host/internal/install-steam-shortcut.sh" --force

if [[ "${daemon_was_active}" -eq 1 ]]; then
    echo "Restarting the Host Control daemon to pick up the update..."
    if ! systemctl --user restart dualdeck-host-control.service 2>/dev/null; then
        echo "warning: could not restart dualdeck-host-control.service -- restart it manually" >&2
        echo "(systemctl --user restart dualdeck-host-control.service)" >&2
    fi
fi
WRAP
chmod +x "${pkg_dir}/host/internal/apply-update.sh"

cat > "${pkg_dir}/host/internal/install-steam-shortcut.sh" <<'WRAP'
#!/usr/bin/env bash
# Registers the DualDeck host as a Steam non-Steam-game shortcut,
# so it can be launched from Steam Big Picture/Gaming Mode with only a
# controller (GitHub issue #10). Mirrors
# ../../client/internal/install-steam-shortcut.sh's approach, reusing
# the same layout-agnostic steam_shortcut.py -- bundled flat alongside
# this script rather than shared from a top-level scripts/ directory,
# since host/ is fully self-contained (same reasoning as client/lib/
# bundling SDL3). Normally launched via ../../dualdeck-host.sh's
# "Add to Steam" menu choice, not directly.
#
# Copies the whole host/ directory (this script's grandparent -- the
# entry-point script and binary, plus this internal/ directory) into a
# fixed central location (~/.config/dualdeck/install/ -- the same
# directory install-host-distrobox.sh already uses on immutable
# systems) and points the shortcut at dualdeck-host.sh there --
# the same single entry point a double-click normally runs, showing its
# "Which system?" picker (DS/melonDS, 3DS/Azahar, host-control-only, or
# a custom emulator) -- not at melonDS, run-host.sh, or
# internal/launch-host.sh directly, so a Steam-launched session gets
# the same choice a manually-launched one does instead of always
# booting straight into melonDS. Re-running this from a newer release's
# extracted archive updates the same shortcut in place instead of
# leaving a stale duplicate pointing at a since-deleted download folder
# (same reasoning as the client's cross-release-directory fix).
#
# A failed update can't break a working install: on an immutable system,
# staging the files AND installing the container's packages is entirely
# delegated to install-host-distrobox.sh --install-only, whose own
# stage-then-swap logic only activates the new files once the package
# install actually succeeds (see that script) -- duplicating a separate
# file copy/swap here, ahead of the package-install step, would let a
# dnf failure leave new, not-yet-verified files active anyway, which is
# exactly the bug this is meant to prevent. On a regular (non-immutable)
# system there's no separate package-install step to gate on, so a
# plain stage-then-swap file copy here is already safe on its own.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
host_root="$(cd .. && pwd)"
# shellcheck source=scripts/lib/steam_restart_helper.sh
source ./steam_restart_helper.sh
# shellcheck source=scripts/lib/host_firewall.sh
source ./host_firewall.sh

# Surfaces failures visibly instead of just closing silently when
# double-clicked with no visible terminal attached -- logs to a
# persistent file and, when available (SteamOS Desktop Mode/Bazzite are
# both KDE Plasma), pops up a graphical error dialog via kdialog. Same
# pattern as client/internal/install-steam-shortcut.sh.
error_log="${HOME}/.config/dualdeck/install.log"
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") install-steam-shortcut.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "DualDeck Host" \
            --error "Installing the Steam shortcut failed: ${failing_cmd}
(exit code ${exit_code})

Details logged to:
${error_log}" 2>/dev/null || true
    fi
}
trap 'ec=$?; on_error "${ec}" "${LINENO}" "${BASH_COMMAND}"' ERR

launch_options=""
extra_args=()
dry_run=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) extra_args+=("$1"); dry_run=1; shift ;;
        --force) extra_args+=("$1"); shift ;;
        --user) extra_args+=("$1" "$2"); shift 2 ;;
        --user=*) extra_args+=("$1"); shift ;;
        *) launch_options+="${launch_options:+ }$1"; shift ;;
    esac
done

# Keep in sync with the same constant in install-host-distrobox.sh,
# uninstall-host-distrobox.sh, and uninstall-steam-shortcut.sh.
central_install_dir="${HOME}/.config/dualdeck/install"
staging_dir="${central_install_dir}.new"
previous_dir="${central_install_dir}.previous"

# One-time melonDS-Remote -> DualDeck rebrand migration: this central
# install dir moved (old: ~/.config/melonds-remote/install) and the
# Steam AppName changed ("melonDS Remote Host" -> "DualDeck Host")
# simultaneously, so steam_shortcut.py's Exe-OR-AppName matching can't
# reliably find-and-update the old entry on its own. Explicitly remove
# the old entry by its old identity first, best-effort.
old_central_install_dir="${HOME}/.config/melonds-remote/install"
if [[ -d "${old_central_install_dir}" && "${dry_run}" -eq 0 ]]; then
    python3 ./steam_shortcut.py \
        --exe "${old_central_install_dir}/melonds-remote-host.sh" \
        --name "melonDS Remote Host" \
        --remove "${extra_args[@]}" >/dev/null 2>&1 || true
fi

if [[ "${dry_run}" -eq 0 ]]; then
    if [[ -f /run/ostree-booted ]] || command -v rpm-ostree >/dev/null 2>&1; then
        echo "Preparing the Distrobox container (this can take a few minutes the first time) ..."
        ./install-host-distrobox.sh --install-only
    else
        rm -rf "${staging_dir}"
        mkdir -p "${staging_dir}"
        cp -a "${host_root}/." "${staging_dir}/"
        find "${staging_dir}" -name '*.sh' -exec chmod +x {} +

        rm -rf "${previous_dir}"
        if [[ -d "${central_install_dir}" ]]; then
            mv "${central_install_dir}" "${previous_dir}"
        fi
        mv "${staging_dir}" "${central_install_dir}"

        # Same reasoning as install-host-distrobox.sh's equivalent copy:
        # keeps "../check-for-updates.sh" resolvable from a copy of
        # dualdeck-host.sh later run from inside the central
        # directory itself.
        cp "$(dirname "${host_root}")/check-for-updates.sh" "$(dirname "${central_install_dir}")/check-for-updates.sh" 2>/dev/null || true
        cp "$(dirname "${host_root}")/VERSION" "$(dirname "${central_install_dir}")/VERSION" 2>/dev/null || true
    fi

    # Best-effort, never fatal to the install itself -- see
    # scripts/lib/host_firewall.sh's own header for exactly what this
    # opens and why it's safe to just log-and-continue on failure
    # (real user report: the client couldn't reach the host at all,
    # even via a direct DualDeck Host GUI launch, and firewalld -- the
    # Bazzite/Fedora default -- blocking these ports was the likely
    # cause; this used to be a manual docs/bazzite-host-setup.md step).
    ensure_host_firewall_ports || true
fi

run_steam_shortcut_with_restart ./steam_shortcut.py "${error_log}" \
    --exe "${central_install_dir}/dualdeck-host.sh" \
    --name "DualDeck Host" \
    --launch-options "${launch_options}" \
    "${extra_args[@]}" && shortcut_exit=0 || shortcut_exit=$?
if [[ "${shortcut_exit}" -ne 0 ]]; then
    on_error "${shortcut_exit}" "${LINENO}" "steam_shortcut.py"
    exit "${shortcut_exit}"
fi
WRAP
chmod +x "${pkg_dir}/host/internal/install-steam-shortcut.sh"

# Compatibility shim: every release before this host/internal/
# restructuring had install-steam-shortcut.sh directly at host/, and
# those releases' own host/apply-update.sh hardcodes exactly that path
# when it downloads and invokes a newer release's copy (see
# docs/known-limitations.md). That already-installed
# old script can't be changed retroactively, so a real user on one of
# those versions hit exactly this: "Check for updates" downloads this
# new, restructured release fine, then fails with exit 127 trying to
# run a file that no longer exists at the old flat path. This shim
# forwards to the real (current) location so updating *from* one of
# those older releases keeps working. New installs/updates never
# reach this file directly -- dualdeck-host.sh and apply-update.sh
# both already call internal/install-steam-shortcut.sh -- so this exists
# purely for that one-time upgrade path and is safe to delete once no
# supported release still depends on it.
cat > "${pkg_dir}/host/install-steam-shortcut.sh" <<'WRAP'
#!/usr/bin/env bash
# Compatibility shim for releases before the host/internal/
# restructuring -- see the comment above this heredoc in
# scripts/build-release.sh for why this exists. Just forwards to the
# real script's current location.
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/internal/install-steam-shortcut.sh" "$@"
WRAP
chmod +x "${pkg_dir}/host/install-steam-shortcut.sh"

cat > "${pkg_dir}/host/internal/uninstall-steam-shortcut.sh" <<'WRAP'
#!/usr/bin/env bash
# Removes the "DualDeck Host" Steam non-Steam-game shortcut added
# by install-steam-shortcut.sh, the central install directory
# (~/.config/dualdeck/install) it copies everything into, and the
# Distrobox container if one was created -- i.e. this is the complete
# host uninstall once a Steam shortcut has been set up. (If you only
# ever used install-host-distrobox.sh directly and never installed the
# Steam shortcut, uninstall-host-distrobox.sh alone is equivalent.)
# Normally launched via ../../dualdeck-host.sh's "Remove from
# Steam" menu choice, not directly.
#
# Always targets the fixed central install directory below, regardless
# of where this script itself is run from -- this exact file is also
# the copy install-steam-shortcut.sh placed there, and must keep
# removing the same shortcut from there indefinitely, even after the
# original archive has been deleted (same reasoning as
# client/internal/uninstall-steam-shortcut.sh).
set -euo pipefail

error_log="${HOME}/.config/dualdeck/install.log"
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") uninstall-steam-shortcut.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "DualDeck Host" \
            --error "Removing the Steam shortcut failed: ${failing_cmd}
(exit code ${exit_code})

Details logged to:
${error_log}" 2>/dev/null || true
    fi
}
trap 'ec=$?; on_error "${ec}" "${LINENO}" "${BASH_COMMAND}"' ERR

# Keep in sync with the same constant in install-steam-shortcut.sh,
# install-host-distrobox.sh, and uninstall-host-distrobox.sh.
central_install_dir="${HOME}/.config/dualdeck/install"
self_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
container_name="dualdeck-host"

steam_shortcut_py=""
for candidate in \
    "${central_install_dir}/internal/steam_shortcut.py" \
    "${self_dir}/steam_shortcut.py"
do
    if [[ -f "${candidate}" ]]; then
        steam_shortcut_py="${candidate}"
        break
    fi
done

if [[ -z "${steam_shortcut_py}" ]]; then
    echo "Nothing installed at ${central_install_dir}, and no local copy of" \
         "steam_shortcut.py found either -- nothing to remove."
    exit 0
fi

# Same two-candidate resolution as steam_shortcut_py above -- see
# client/internal/uninstall-steam-shortcut.sh's identical comment.
steam_restart_helper=""
for candidate in \
    "${central_install_dir}/internal/steam_restart_helper.sh" \
    "${self_dir}/steam_restart_helper.sh"
do
    if [[ -f "${candidate}" ]]; then
        steam_restart_helper="${candidate}"
        break
    fi
done
if [[ -n "${steam_restart_helper}" ]]; then
    # shellcheck source=scripts/lib/steam_restart_helper.sh
    source "${steam_restart_helper}"
fi

# One-time melonDS-Remote -> DualDeck rebrand cleanup: also try removing
# under the old identity (old central dir, old Exe, old AppName), since
# the Exe-OR-AppName fallback alone can't bridge a compound change of
# both fields at once. No-op if it was never installed there or was
# already migrated.
old_central_install_dir="${HOME}/.config/melonds-remote/install"
python3 "${steam_shortcut_py}" \
    --exe "${old_central_install_dir}/melonds-remote-host.sh" \
    --name "melonDS Remote Host" \
    --remove "$@" >/dev/null 2>&1 || true

if [[ -n "${steam_restart_helper}" ]]; then
    run_steam_shortcut_with_restart "${steam_shortcut_py}" "${error_log}" \
        --exe "${central_install_dir}/dualdeck-host.sh" \
        --name "DualDeck Host" \
        --remove \
        "$@"
else
    python3 "${steam_shortcut_py}" \
        --exe "${central_install_dir}/dualdeck-host.sh" \
        --name "DualDeck Host" \
        --remove \
        "$@"
fi

dry_run=0
for arg in "$@"; do
    [[ "${arg}" == "--dry-run" ]] && dry_run=1
done

if [[ "${dry_run}" -eq 0 ]]; then
    if command -v distrobox >/dev/null 2>&1 && distrobox list 2>/dev/null | grep -qw "${container_name}"; then
        echo "Removing Distrobox container \"${container_name}\" ..."
        distrobox rm "${container_name}" --force
    fi
    # Same melonDS-Remote -> DualDeck rebrand cleanup as the shortcut
    # removal above, for the container this project itself created
    # under the old name before the rename.
    if command -v distrobox >/dev/null 2>&1 && distrobox list 2>/dev/null | grep -qw "melonds-remote-host"; then
        echo "Removing old Distrobox container \"melonds-remote-host\" ..."
        distrobox rm "melonds-remote-host" --force
    fi

    # cd out first -- this script may itself be running from inside
    # central_install_dir (it's the copy install-steam-shortcut.sh made
    # there), so deleting the shell's own current directory out from
    # under it is avoided by leaving before removing anything.
    cd /
    for dir in "${central_install_dir}" "${central_install_dir}.new" "${central_install_dir}.previous"; do
        if [[ -d "${dir}" ]]; then
            echo "Removing ${dir}"
            rm -rf -- "${dir}"
        fi
    done

    # check-for-updates.sh/VERSION are staged as siblings of install/
    # (see install-steam-shortcut.sh/install-host-distrobox.sh), not
    # inside any of the three directories just removed above.
    rm -f -- "$(dirname "${central_install_dir}")/check-for-updates.sh" \
             "$(dirname "${central_install_dir}")/VERSION"
fi
WRAP
chmod +x "${pkg_dir}/host/internal/uninstall-steam-shortcut.sh"

cp "${repo_root}/docs/building.md" "${repo_root}/docs/steam-deck-setup.md" \
   "${repo_root}/docs/bazzite-host-setup.md" "${repo_root}/docs/troubleshooting.md" \
   "${repo_root}/docs/known-limitations.md" "${repo_root}/docs/protocol.md" \
   "${pkg_dir}/docs/"
cp "${repo_root}/LICENSE" "${pkg_dir}/"
cp "${repo_root}/docs/release-readme.md" "${pkg_dir}/README.md"

cat > "${pkg_dir}/check-for-updates.sh" <<'WRAP'
#!/usr/bin/env bash
# Checks whether a newer DualDeck release is published on GitHub --
# read-only, no download or install of anything happens here. Both
# dualdeck-host.sh and dualdeck-client.sh's "Check for
# updates" menu choice call this first and only offer to actually
# install if it reports one available. Capped at 5s network time and
# never exits non-zero on a reachability/parse failure, specifically so
# nothing that calls this automatically -- run-client.sh does, on every
# launch, applying an update silently if one's found -- ever blocks or
# fails a launch waiting on it. run-host.sh does not call this
# automatically (the host has no equivalent auto-update yet); run this
# yourself (or use the host menu) to check there.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

current_version="$(cat VERSION 2>/dev/null || echo "unknown")"
repo="Crimson3076/DualDeck"

if ! command -v curl >/dev/null 2>&1; then
    echo "DualDeck ${current_version} -- install 'curl' to enable update checks."
    exit 0
fi

api_response="$(curl -fsSL --max-time 5 \
    "https://api.github.com/repos/${repo}/releases/latest" 2>/dev/null)"
if [[ -z "${api_response}" ]]; then
    echo "DualDeck ${current_version} -- couldn't reach GitHub to check for updates (offline?)."
    exit 0
fi

latest_version="$(echo "${api_response}" | grep -o '"tag_name" *: *"[^"]*"' | head -1 | sed -E 's/.*"([^"]+)"$/\1/')"
if [[ -z "${latest_version}" ]]; then
    echo "DualDeck ${current_version} -- couldn't parse GitHub's response to check for updates."
    exit 0
fi

if [[ "${latest_version}" == "${current_version}" ]]; then
    echo "DualDeck ${current_version} -- you're on the latest version."
else
    echo "DualDeck ${current_version} -- update available: ${latest_version}"
    echo "  https://github.com/${repo}/releases/tag/${latest_version}"
fi
WRAP
chmod +x "${pkg_dir}/check-for-updates.sh"

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
# EmuDeck integration (experimental)

Installs DualDeck's patched melonDS/Azahar/Cemu directly at the same
path your existing EmuDeck installation's Steam shortcuts and launcher
scripts already point to (\`~/Applications/*.AppImage\`), so they keep
working unedited -- no need to change any EmuDeck shortcut, no separate
DualDeck-managed install directory for these emulators.

This downloads already-built, already-patched AppImages from this same
GitHub release (built once in CI, not compiled on your machine) and
verifies them against the release's \`SHA256SUMS\` before installing
anything -- no build toolchain, no Distrobox, nothing to compile.

Confirmed working end to end on real hardware (all three emulators
launching via their existing EmuDeck/Steam shortcuts) -- see
\`docs/known-limitations.md\`'s 2026-08-01 entries for the full
verification history and what's still outstanding.

## Usage

    cd emudeck-integration/scripts
    ./emudeck-replace-in-place.sh --dry-run

Start with \`--dry-run\` -- it only detects your EmuDeck installs and
reports what it would do, with zero side effects (it won't even
download anything). Once you're happy with the plan:

    ./emudeck-replace-in-place.sh

Add \`--emulator melonds\` / \`--emulator azahar\` / \`--emulator cemu\`
(repeatable) to target specific emulators instead of every one EmuDeck
has installed. Add \`--yes\` to skip the confirmation prompt.

This also opens this host's firewall for DualDeck's ports (via
\`firewall-cmd\`/\`ufw\`, whichever is present -- may prompt for your
password) the first time it actually installs something, so the client
can reach the private host-service each patched AppImage starts on its
own when launched -- see \`docs/known-limitations.md\`'s "client hangs
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
