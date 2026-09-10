#!/usr/bin/env bash
# Builds a standalone RetroDECK-Cemu-component-shaped artifact from the
# exact same pinned Cemu source commit and DualDeck patch that
# scripts/build-release.sh's AppImage step uses -- see
# docs/retrodeck-compatibility.md for the full design writeup (why this
# exists, what it isn't, and what's actually verified so far) and
# scripts/lib/appimage_pack.sh's pack_retrodeck_component_tarball() for
# why the output isn't (yet) byte-for-byte what RetroDECK's own
# automation produces.
#
# Deliberately kept out of scripts/build-release.sh and
# .github/workflows/release.yml: this is a separate, opt-in artifact for
# an experimental compatibility target, not part of the already-working
# release pipeline those files own -- see docs/retrodeck-compatibility.md's
# "Why a separate script" note.
#
# Usage: ./scripts/build-retrodeck-cemu-component.sh
# Output: <out_dir>/dualdeck-cemu-retrodeck-component-linux-x86_64.tar.gz,
#         a matching SHA256SUMS, and a BUILD_MANIFEST.json recording the
#         exact commit/patch/build-flag/runtime provenance (item 8 of the
#         RetroDECK-compatibility task: "record the runtime, SDK,
#         dependencies, build flags and artifact hash").
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/lib/pinned_commits.sh
source "${repo_root}/scripts/lib/pinned_commits.sh"
work_dir="${BUILD_RETRODECK_COMPONENT_WORKDIR:-$(mktemp -d)}"
out_dir="${BUILD_RETRODECK_COMPONENT_OUTPUT_DIR:-${repo_root}/retrodeck-component-out}"
mkdir -p "${work_dir}" "${out_dir}"

# shellcheck source=scripts/lib/ensure-packages.sh
source "${repo_root}/scripts/lib/ensure-packages.sh"
# shellcheck source=scripts/lib/build_emulator.sh
source "${repo_root}/scripts/lib/build_emulator.sh"
# shellcheck source=scripts/lib/appimage_pack.sh
source "${repo_root}/scripts/lib/appimage_pack.sh"
# shellcheck source=scripts/lib/apprun_templates.sh
source "${repo_root}/scripts/lib/apprun_templates.sh"

echo "== [0/4] Checking build dependencies =="
# Same "build" + "cemu build" lists scripts/build-release.sh uses for its
# own Cemu step -- kept as two ensure_packages() calls (not a third,
# narrower list) so this script can never silently drift onto a
# different set of Cemu build dependencies than the release pipeline
# already verified. See build-release.sh's own comments on each package
# for why it's there.
ensure_packages "build" \
    "cmake extra-cmake-modules ninja-build build-essential git python3 libcurl4-gnutls-dev libpcap0.8-dev libsdl2-dev libarchive-dev libenet-dev libzstd-dev libfaad-dev qt6-base-dev qt6-base-private-dev qt6-multimedia-dev qt6-svg-dev qt6-wayland libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev libwayland-dev libxkbcommon-dev libdrm-dev libgbm-dev libdecor-0-dev libturbojpeg0-dev libdbus-1-dev libpipewire-0.3-dev libopenh264-dev libyuv-dev" \
    "cmake extra-cmake-modules ninja-build gcc-c++ git python3 libcurl-devel libpcap-devel SDL2-devel libarchive-devel enet-devel libzstd-devel faad2-devel qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtmultimedia-devel qt6-qtsvg-devel qt6-qtwayland libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXfixes-devel libXi-devel libXScrnSaver-devel libXtst-devel wayland-devel libxkbcommon-devel libdrm-devel mesa-libgbm-devel libdecor-devel turbojpeg-devel dbus-devel pipewire-devel openh264-devel libyuv-devel perl-FindBin perl-IPC-Cmd" \
    "cmake extra-cmake-modules ninja base-devel git python curl libpcap sdl2 libarchive enet zstd faad2 qt6-base qt6-multimedia qt6-svg qt6-wayland libx11 libxext libxrandr libxcursor libxfixes libxi libxss libxtst wayland libxkbcommon libdrm mesa libdecor libjpeg-turbo dbus libpipewire openh264 libyuv"
ensure_packages "cemu build" \
    "freeglut3-dev libbluetooth-dev libgcrypt20-dev libglm-dev libgtk-3-dev libpulse-dev libsecret-1-dev libsystemd-dev libtool nasm libusb-1.0-0-dev" \
    "freeglut-devel bluez-libs-devel libgcrypt-devel glm-devel gtk3-devel pulseaudio-libs-devel libsecret-devel systemd-devel libtool nasm libusb1-devel perl-IPC-Cmd" \
    "freeglut bluez-libs libgcrypt glm gtk3 libpulse libsecret systemd libtool nasm libusb"

cmake_launcher_args=()
if command -v sccache >/dev/null 2>&1; then
    echo "sccache found on PATH, enabling as CMake compiler launcher"
    cmake_launcher_args=(-DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache)
fi

echo "== [1/4] Patched Cemu (Nintendo Wii U, commit ${CEMU_COMMIT}) =="
build_cemu cemu_bin "${work_dir}" "${repo_root}" "${CEMU_COMMIT}" "${CEMU_VERSION_MAJOR}" "${CEMU_VERSION_MINOR}"
cemu_bin_dir="$(dirname "${cemu_bin}")"

echo "== [2/4] DualDeck Host Service (this repo, host-only build) =="
# DUALDECK_BUILD_CLIENT stays OFF (its default) -- the component tarball
# only ever runs on the machine acting as DualDeck host; no SDL3/client
# build needed here at all.
repo_build="${work_dir}/repo-build"
cmake -S "${repo_root}" -B "${repo_build}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DDUALDECK_BUILD_HOST=ON \
    "${cmake_launcher_args[@]}"
cmake --build "${repo_build}" -j"$(nproc)"

echo "== [3/4] Packaging =="
# pack_retrodeck_component_tarball() names the file inside
# <component>/usr/bin/ after basename(binary_path), and the launcher
# script execs "usr/bin/cemu" literally (same convention
# build-release.sh's own AppImage step uses) -- Cemu's real build output
# is named Cemu_release, not "cemu", so it's staged under the right name
# first, exactly like build-release.sh does for its own AppImage.
cemu_staged="${work_dir}/staged-cemu/cemu"
mkdir -p "$(dirname "${cemu_staged}")"
cp "${cemu_bin}" "${cemu_staged}"

# Reuses the exact same out-of-process launcher logic (probe the shared
# $XDG_RUNTIME_DIR/dualdeck/adapter.sock daemon socket, else spawn a
# private dualdeck-host-service, set CEMU_REMOTE_ENABLE/
# CEMU_REMOTE_ADAPTER_SOCKET, then run Cemu) as the AppImage build --
# see pack_retrodeck_component_tarball()'s own comment for why reusing
# it (rather than writing a second, RetroDECK-specific launcher) matters.
component_launcher="${work_dir}/component_launcher-cemu.sh"
generate_apprun_out_of_process CEMU cemu cemu-component-adapter.sock "${component_launcher}"

component_version="${RETRODECK_COMPONENT_VERSION_TAG:-dev-$(cd "${repo_root}" && git rev-parse --short HEAD)}"
archive_name="dualdeck-cemu-retrodeck-component-linux-x86_64.tar.gz"
pack_retrodeck_component_tarball "${cemu_staged}" "${out_dir}/${archive_name}" cemu \
    "${component_launcher}" "${repo_build}/host/remote-server/dualdeck-host-service" \
    "${cemu_bin_dir}/resources:${cemu_bin_dir}/gameProfiles"
echo "Wrote ${out_dir}/${archive_name}"

echo "== [4/4] Provenance manifest + checksum =="
# Item 8 of the RetroDECK-compatibility task: record the runtime, SDK,
# dependencies, build flags and artifact hash. "Runtime/SDK" here means
# the Flatpak runtime this artifact is built to be *compatible with*
# (its libraries are bundled, not built against RetroDECK's own
# org.kde.Sdk sysroot) -- see docs/retrodeck-compatibility.md for why a
# real flatpak-builder-based build is the subject of the upstream
# proposal, not this script.
cemu_patch_file="${repo_root}/host/cemu-patches/0001-remote-server-integration.patch"
cemu_patch_sha256="$(sha256sum "${cemu_patch_file}" | awk '{print $1}')"
dualdeck_commit="$(cd "${repo_root}" && git rev-parse HEAD)"
cat > "${out_dir}/BUILD_MANIFEST.json" <<MANIFEST
{
  "artifact": "${archive_name}",
  "component": "cemu",
  "component_version_tag": "${component_version}",
  "dualdeck_repo_commit": "${dualdeck_commit}",
  "cemu_upstream_commit": "${CEMU_COMMIT}",
  "cemu_upstream_tag": "v${CEMU_VERSION_MAJOR}.${CEMU_VERSION_MINOR}",
  "cemu_patch_file": "host/cemu-patches/0001-remote-server-integration.patch",
  "cemu_patch_sha256": "${cemu_patch_sha256}",
  "cmake_build_type": "release",
  "cmake_flags": ["-DEMULATOR_VERSION_MAJOR=${CEMU_VERSION_MAJOR}", "-DEMULATOR_VERSION_MINOR=${CEMU_VERSION_MINOR}"],
  "vcpkg_overlay_ports": ["sdl2", "glslang"],
  "target_flatpak_runtime": "org.kde.Platform//6.10",
  "target_flatpak_sdk": "org.kde.Sdk//6.10",
  "built_on": "$(uname -srm)",
  "built_at_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
MANIFEST
echo "Wrote ${out_dir}/BUILD_MANIFEST.json"

(cd "${out_dir}" && sha256sum "${archive_name}" BUILD_MANIFEST.json > SHA256SUMS)
echo "Wrote ${out_dir}/SHA256SUMS"

echo "Done. See docs/retrodeck-compatibility.md for how to install, roll back, and remove this artifact."
