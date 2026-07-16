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

MELONDS_COMMIT="10a173b5536fc75cd93f8a3868349dad963542ef"
SDL3_TAG="release-3.2.16"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work_dir="${BUILD_RELEASE_WORKDIR:-$(mktemp -d)}"
out_dir="${BUILD_RELEASE_OUTPUT_DIR:-${repo_root}/release-out}"
mkdir -p "${work_dir}" "${out_dir}"

# shellcheck source=scripts/lib/ensure-packages.sh
source "${repo_root}/scripts/lib/ensure-packages.sh"

echo "== [0/4] Checking build dependencies =="
ensure_packages "build" \
    "cmake extra-cmake-modules ninja-build build-essential git python3 libcurl4-gnutls-dev libpcap0.8-dev libsdl2-dev libarchive-dev libenet-dev libzstd-dev libfaad-dev qt6-base-dev qt6-base-private-dev qt6-multimedia-dev qt6-svg-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libwayland-dev libxkbcommon-dev libdrm-dev libgbm-dev libdecor-0-dev" \
    "cmake extra-cmake-modules ninja-build gcc-c++ git python3 libcurl-devel libpcap-devel SDL2-devel libarchive-devel enet-devel libzstd-devel faad2-devel qt6-qtbase-devel qt6-qtmultimedia-devel qt6-qtsvg-devel libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXfixes-devel libXi-devel libXScrnSaver-devel wayland-devel libxkbcommon-devel libdrm-devel mesa-libgbm-devel libdecor-devel" \
    "cmake extra-cmake-modules ninja base-devel git python curl libpcap sdl2 libarchive enet zstd faad2 qt6-base qt6-multimedia qt6-svg libx11 libxext libxrandr libxcursor libxfixes libxi libxss wayland libxkbcommon libdrm mesa libdecor"

sdl3_src="${work_dir}/sdl3-src"
sdl3_install="${work_dir}/sdl3-install"

echo "== [1/4] SDL3 (${SDL3_TAG}) =="
if [[ -f "${sdl3_install}/lib/cmake/SDL3/SDL3Config.cmake" ]]; then
    echo "already built at ${sdl3_install}, skipping (cache hit)"
else
    rm -rf "${sdl3_src}"
    git clone --depth 1 --branch "${SDL3_TAG}" https://github.com/libsdl-org/SDL.git "${sdl3_src}"
    cmake -S "${sdl3_src}" -B "${sdl3_src}/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${sdl3_install}" \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF
    cmake --build "${sdl3_src}/build" -j"$(nproc)"
    cmake --install "${sdl3_src}/build"
fi

echo "== [2/4] Patched melonDS host (commit ${MELONDS_COMMIT}) =="
melonds_src="${work_dir}/melonds-src"
rm -rf "${melonds_src}"
git clone https://github.com/melonDS-emu/melonDS.git "${melonds_src}"
(cd "${melonds_src}" && git checkout "${MELONDS_COMMIT}")
(cd "${melonds_src}" && git apply "${repo_root}/host/melonds-patches/0001-remote-server-integration.patch")
cmake -S "${melonds_src}" -B "${melonds_src}/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "${melonds_src}/build" -j"$(nproc)"

echo "== [3/4] Client + host prototype (this repo) =="
repo_build="${work_dir}/repo-build"
cmake -S "${repo_root}" -B "${repo_build}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DMELONDS_REMOTE_BUILD_CLIENT=ON \
    -DMELONDS_REMOTE_BUILD_HOST=ON -DCMAKE_PREFIX_PATH="${sdl3_install}"
cmake --build "${repo_build}" -j"$(nproc)"
ctest --test-dir "${repo_build}" --output-on-failure

echo "== [4/4] Packaging =="
commit_short="$(cd "${repo_root}" && git rev-parse --short HEAD)"
branch_name="$(cd "${repo_root}" && git rev-parse --abbrev-ref HEAD)"
# Set by .github/workflows/release.yml to the actual published tag
# (vX.Y.<run number>); falls back to a "dev-<commit>" placeholder for
# local runs outside CI, where there's no real release tag yet.
version_tag="${RELEASE_VERSION_TAG:-dev-${commit_short}}"
pkg_name="melonds-remote-${commit_short}-linux-x86_64"
pkg_dir="${work_dir}/${pkg_name}"
rm -rf "${pkg_dir}"
mkdir -p "${pkg_dir}/host" "${pkg_dir}/client/lib" "${pkg_dir}/docs" "${pkg_dir}/scripts/lib"

# Read by check-for-updates.sh below to compare against the latest
# published GitHub release.
echo "${version_tag}" > "${pkg_dir}/VERSION"

cp "${melonds_src}/build/melonDS" "${pkg_dir}/host/melonDS"
chmod +x "${pkg_dir}/host/melonDS"
ldd "${melonds_src}/build/melonDS" | awk '{print $1}' | sort -u \
    > "${pkg_dir}/host/host-shared-library-dependencies.txt"

# host/ is fully self-contained (same reasoning as client/lib/ bundling
# SDL3): these two are also needed once install-steam-shortcut.sh copies
# host/ into ~/.config/melonds-remote/install/ -- a flat layout with no
# scripts/ sibling of its own, unlike the client's central install
# directory -- so run-host.sh and install-steam-shortcut.sh must find
# them next to themselves rather than via a `../scripts/lib/` path.
cp "${repo_root}/scripts/lib/ensure-packages.sh" "${pkg_dir}/host/ensure-packages.sh"
cp "${repo_root}/scripts/lib/steam_shortcut.py" "${pkg_dir}/host/steam_shortcut.py"

cp "${repo_build}/client/melonds-remote-client" "${pkg_dir}/client/melonds-remote-client"
chmod +x "${pkg_dir}/client/melonds-remote-client"
cp -a "${sdl3_install}"/lib/libSDL3.so* "${pkg_dir}/client/lib/"

# Shared dependency-check-and-install helper, bundled so the standalone
# run-host.sh/run-client.sh below can source it on the *end user's*
# machine (which won't have the rest of this repo checked out).
cp "${repo_root}/scripts/lib/ensure-packages.sh" "${pkg_dir}/scripts/lib/ensure-packages.sh"

cat > "${pkg_dir}/client/run-client.sh" <<'WRAP'
#!/usr/bin/env bash
# Runs melonds-remote-client with the bundled SDL3 shared library,
# auto-installing any missing runtime system libraries first (X11/
# Wayland etc. -- SDL3 itself is bundled in lib/, no install needed for
# that part).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

# shellcheck source=scripts/lib/ensure-packages.sh
source ../scripts/lib/ensure-packages.sh

ensure_packages "client runtime" \
    "libx11-6 libxext6 libxrandr2 libxcursor1 libxfixes3 libxi6 libxss1 libwayland-client0 libwayland-cursor0 libwayland-egl1 libxkbcommon0 libdrm2 libgbm1 libdecor-0-0" \
    "libX11 libXext libXrandr libXcursor libXfixes libXi libXScrnSaver wayland-client wayland-cursor wayland-egl libxkbcommon libdrm mesa-libgbm libdecor" \
    "libx11 libxext libxrandr libxcursor libxfixes libxi libxss wayland libxkbcommon libdrm mesa libdecor" \
    || echo "warning: could not verify/install client runtime libraries automatically; continuing anyway in case they're already present" >&2

LD_LIBRARY_PATH="$(pwd)/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec ./melonds-remote-client "$@"
WRAP
chmod +x "${pkg_dir}/client/run-client.sh"

# scripts/lib/steam_shortcut.py is layout-agnostic (takes --exe
# explicitly), so it's bundled as-is; only the wrapper around it needs to
# know this archive's layout (client binary next to the wrapper, not
# under a separate build/ directory like the repo's own
# scripts/install-steam-shortcut.sh assumes).
cp "${repo_root}/scripts/lib/steam_shortcut.py" "${pkg_dir}/scripts/lib/steam_shortcut.py"

cat > "${pkg_dir}/client/install-steam-shortcut.sh" <<'WRAP'
#!/usr/bin/env bash
# Registers melonds-remote-client as a Steam non-Steam-game shortcut --
# see ../scripts/lib/steam_shortcut.py for exactly what this does and why
# it's careful about it (backs up shortcuts.vdf first, refuses to run
# while Steam is open unless --force). Any arguments given here are
# passed through as the shortcut's launch options, e.g.:
#   ./install-steam-shortcut.sh --host 192.168.1.50
#
# Copies this whole client/ + scripts/lib/ tree into a fixed central
# directory -- see central_install_dir below -- and points the Steam
# shortcut at the copy there (specifically at run-client.sh, not the
# raw binary, so the bundled SDL3 library is found via LD_LIBRARY_PATH).
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

# Surfaces failures visibly instead of just closing silently when
# double-clicked with no visible terminal attached (GitHub issue #11) --
# logs to a persistent file and, when available (SteamOS Desktop Mode/
# Bazzite are both KDE Plasma), pops up a graphical error dialog via
# kdialog.
error_log="${HOME}/.config/melonds-remote-client/install.log"
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") install-steam-shortcut.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "melonDS Remote" \
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

# Keep in sync with the same constant in scripts/install-steam-shortcut.sh,
# scripts/uninstall-steam-shortcut.sh, and this archive's own
# uninstall-steam-shortcut.sh.
central_install_dir="${HOME}/.config/melonds-remote-client/install"
staging_dir="${central_install_dir}.new"
previous_dir="${central_install_dir}.previous"

if [[ "${dry_run}" -eq 0 ]]; then
    rm -rf "${staging_dir}"
    mkdir -p "${staging_dir}/client/lib" "${staging_dir}/scripts/lib"

    cp melonds-remote-client "${staging_dir}/client/melonds-remote-client"
    chmod +x "${staging_dir}/client/melonds-remote-client"
    cp -a lib/. "${staging_dir}/client/lib/"
    cp run-client.sh "${staging_dir}/client/run-client.sh"
    chmod +x "${staging_dir}/client/run-client.sh"
    cp uninstall-steam-shortcut.sh "${staging_dir}/client/uninstall-steam-shortcut.sh"
    chmod +x "${staging_dir}/client/uninstall-steam-shortcut.sh"
    cp ../scripts/lib/ensure-packages.sh "${staging_dir}/scripts/lib/ensure-packages.sh"
    cp ../scripts/lib/steam_shortcut.py "${staging_dir}/scripts/lib/steam_shortcut.py"

    # Only reached if staging succeeded -- safe to activate now. Keeps
    # just one backup generation, not unbounded.
    rm -rf "${previous_dir}"
    if [[ -d "${central_install_dir}" ]]; then
        mv "${central_install_dir}" "${previous_dir}"
    fi
    mv "${staging_dir}" "${central_install_dir}"
fi

python3 ../scripts/lib/steam_shortcut.py \
    --exe "${central_install_dir}/client/run-client.sh" \
    --launch-options "${launch_options}" \
    "${extra_args[@]}" && shortcut_exit=0 || shortcut_exit=$?
if [[ "${shortcut_exit}" -ne 0 ]]; then
    on_error "${shortcut_exit}" "${LINENO}" "steam_shortcut.py"
    exit "${shortcut_exit}"
fi
WRAP
chmod +x "${pkg_dir}/client/install-steam-shortcut.sh"

cat > "${pkg_dir}/client/uninstall-steam-shortcut.sh" <<'WRAP'
#!/usr/bin/env bash
# Removes the melonDS Remote Steam non-Steam-game shortcut added by
# install-steam-shortcut.sh, and deletes the central install directory
# that script copies everything into. See ../scripts/lib/steam_shortcut.py
# for exactly what the shortcut-removal part does and why it's careful
# about it (backs up shortcuts.vdf first, refuses to run while Steam is
# open unless --force).
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
error_log="${HOME}/.config/melonds-remote-client/install.log"
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") uninstall-steam-shortcut.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "melonDS Remote" \
            --error "Removing the Steam shortcut failed: ${failing_cmd}
(exit code ${exit_code})

Details logged to:
${error_log}" 2>/dev/null || true
    fi
}
trap 'ec=$?; on_error "${ec}" "${LINENO}" "${BASH_COMMAND}"' ERR

# Keep in sync with the same constant in install-steam-shortcut.sh above,
# and in scripts/install-steam-shortcut.sh / scripts/uninstall-steam-shortcut.sh.
central_install_dir="${HOME}/.config/melonds-remote-client/install"
self_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

steam_shortcut_py=""
for candidate in \
    "${central_install_dir}/scripts/lib/steam_shortcut.py" \
    "${self_dir}/../scripts/lib/steam_shortcut.py"
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

python3 "${steam_shortcut_py}" \
    --exe "${central_install_dir}/client/run-client.sh" \
    --remove \
    "$@"

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
fi
WRAP
chmod +x "${pkg_dir}/client/uninstall-steam-shortcut.sh"

cat > "${pkg_dir}/host/run-host.sh" <<'WRAP'
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
# Omit MELONDS_REMOTE_AUTH_TOKEN to use device-approval authentication
# instead of a static shared secret.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

# shellcheck source=scripts/lib/ensure-packages.sh
source ./ensure-packages.sh

ensure_packages "host runtime" \
    "libcurl4-gnutls-dev libpcap0.8-dev libsdl2-dev libarchive-dev libenet-dev libzstd-dev libfaad-dev qt6-base-dev qt6-base-private-dev qt6-multimedia-dev qt6-svg-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libwayland-dev libxkbcommon-dev libdrm-dev libgbm-dev libdecor-0-dev" \
    "libcurl-devel libpcap-devel SDL2-devel libarchive-devel enet-devel libzstd-devel faad2-devel qt6-qtbase-devel qt6-qtmultimedia-devel qt6-qtsvg-devel libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXfixes-devel libXi-devel libXScrnSaver-devel wayland-devel libxkbcommon-devel libdrm-devel mesa-libgbm-devel libdecor-devel" \
    "curl libpcap sdl2 libarchive enet zstd faad2 qt6-base qt6-multimedia qt6-svg libx11 libxext libxrandr libxcursor libxfixes libxi libxss wayland libxkbcommon libdrm mesa libdecor" \
    || echo "warning: could not verify/install host runtime libraries automatically; see host-shared-library-dependencies.txt and docs/troubleshooting.md" >&2

if [[ -f /run/ostree-booted ]] || command -v rpm-ostree >/dev/null 2>&1; then
    echo "Note: this looks like an immutable (rpm-ostree) system, e.g. Bazzite --" >&2
    echo "if melonDS fails to start below over a missing shared library, use" >&2
    echo "./install-host-distrobox.sh instead, which runs it inside a Distrobox" >&2
    echo "container with everything it needs already installed. See" >&2
    echo "docs/bazzite-host-setup.md." >&2
fi

export MELONDS_REMOTE_ENABLE=1
exec ./melonDS "$@"
WRAP
chmod +x "${pkg_dir}/host/run-host.sh"

cat > "${pkg_dir}/host/install-host-distrobox.sh" <<'WRAP'
#!/usr/bin/env bash
# Runs the melonDS Remote host inside a Distrobox container, for
# immutable-filesystem systems (Bazzite, other rpm-ostree/Fedora Atomic
# derivatives) where run-host.sh can't install missing runtime libraries
# directly -- see docs/bazzite-host-setup.md. On a regular (non-immutable)
# Linux system, just use run-host.sh instead; this refuses to run there
# rather than needlessly creating a container.
#
# Safe to re-run any time, including from a newer release's extracted
# archive: it always re-syncs this host/ directory into a fixed location
# (~/.config/melonds-remote/install/) and re-installs into the same
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

if [[ ! -f /run/ostree-booted ]] && ! command -v rpm-ostree >/dev/null 2>&1; then
    echo "This doesn't look like an immutable (rpm-ostree) system -- just run" >&2
    echo "./run-host.sh directly instead, no container needed." >&2
    exit 1
fi

if ! command -v distrobox >/dev/null 2>&1; then
    echo "error: 'distrobox' not found. Bazzite ships it by default; on other" >&2
    echo "rpm-ostree systems, install it first -- see" >&2
    echo "https://github.com/89luca89/distrobox" >&2
    exit 1
fi

install_only=0
if [[ "${1:-}" == "--install-only" ]]; then
    install_only=1
    shift
fi

# Keep in sync with the same paths in uninstall-host-distrobox.sh,
# install-steam-shortcut.sh, uninstall-steam-shortcut.sh, and
# docs/bazzite-host-setup.md's description of this path.
central_install_dir="${HOME}/.config/melonds-remote/install"
staging_dir="${central_install_dir}.new"
previous_dir="${central_install_dir}.previous"
container_name="melonds-remote-host"

already_central=0
if [[ "$(pwd)" == "${central_install_dir}" ]]; then
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
    cp -a . "${staging_dir}/"

    # So check-for-updates.sh keeps working via melonds-remote-host.sh's
    # "../check-for-updates.sh" reference even when a copy of that menu
    # script is later run from inside the central directory itself
    # (e.g. after the original downloaded archive has been deleted) --
    # these two live one level up, as siblings of install/, so they
    # survive the install/install.previous swap below untouched.
    # Best-effort: a missing source (e.g. this got invoked some other
    # way) shouldn't fail the whole install over a convenience file.
    cp "../check-for-updates.sh" "$(dirname "${central_install_dir}")/check-for-updates.sh" 2>/dev/null || true
    cp "../VERSION" "$(dirname "${central_install_dir}")/VERSION" 2>/dev/null || true
fi

echo "Creating/reusing Distrobox container \"${container_name}\" (Fedora-based) ..."
distrobox create --name "${container_name}" --image fedora:latest --yes

echo "Installing runtime libraries inside the container ..."
distrobox enter "${container_name}" -- sudo dnf install -y \
    libcurl-devel libpcap-devel SDL2-devel libarchive-devel enet-devel libzstd-devel faad2-devel \
    qt6-qtbase-devel qt6-qtmultimedia-devel qt6-qtsvg-devel \
    libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXfixes-devel libXi-devel libXScrnSaver-devel \
    wayland-devel libxkbcommon-devel libdrm-devel mesa-libgbm-devel libdecor-devel

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
exec distrobox enter "${container_name}" -- env MELONDS_REMOTE_ENABLE=1 "${central_install_dir}/melonDS" "$@"
WRAP
chmod +x "${pkg_dir}/host/install-host-distrobox.sh"

cat > "${pkg_dir}/host/uninstall-host-distrobox.sh" <<'WRAP'
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
cd "$(dirname "${BASH_SOURCE[0]}")"

# Keep in sync with the same paths in install-host-distrobox.sh,
# install-steam-shortcut.sh, and uninstall-steam-shortcut.sh.
central_install_dir="${HOME}/.config/melonds-remote/install"
container_name="melonds-remote-host"

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
chmod +x "${pkg_dir}/host/uninstall-host-distrobox.sh"

cat > "${pkg_dir}/host/launch-host.sh" <<'WRAP'
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

if [[ -f /run/ostree-booted ]] || command -v rpm-ostree >/dev/null 2>&1; then
    exec ./install-host-distrobox.sh "$@"
else
    exec ./run-host.sh "$@"
fi
WRAP
chmod +x "${pkg_dir}/host/launch-host.sh"

cat > "${pkg_dir}/host/melonds-remote-host.sh" <<'WRAP'
#!/usr/bin/env bash
# The one thing to double-click to set up or run the melonDS Remote
# host -- shows a simple menu and delegates to whichever of
# run-host.sh / install-host-distrobox.sh / install-steam-shortcut.sh /
# uninstall-steam-shortcut.sh / check-for-updates.sh actually applies,
# so a user never has to figure out which of those five scripts they
# need (GitHub issue #10: "the normal path requires no terminal
# commands"). Those scripts still exist and still work standalone (e.g.
# for scripting or troubleshooting) -- this is just the single entry
# point a human actually needs to know about.
#
# Uses a graphical kdialog menu when available (SteamOS Desktop Mode
# and Bazzite are both KDE Plasma, where kdialog is standard) and falls
# back to a plain numbered prompt in a terminal otherwise.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

error_log="${HOME}/.config/melonds-remote/install.log"
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") melonds-remote-host.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "melonDS Remote Host" --error "Something went wrong: ${failing_cmd}
(exit code ${exit_code})

Details logged to:
${error_log}" 2>/dev/null || true
    fi
}
trap 'ec=$?; on_error "${ec}" "${LINENO}" "${BASH_COMMAND}"' ERR

have_kdialog() { command -v kdialog >/dev/null 2>&1; }

info() {
    if have_kdialog; then
        kdialog --title "melonDS Remote Host" --msgbox "$1" 2>/dev/null
    else
        echo
        echo "$1"
        echo
    fi
}

confirm() {
    if have_kdialog; then
        kdialog --title "melonDS Remote Host" --yesno "$1" 2>/dev/null
    else
        read -rp "$1 [y/N] " reply
        [[ "${reply}" =~ ^[Yy]$ ]]
    fi
}

choose_action() {
    if have_kdialog; then
        kdialog --title "melonDS Remote Host" --menu "What would you like to do?" \
            launch "Launch melonDS now" \
            steam-add "Add to Steam (Big Picture / Gaming Mode)" \
            steam-remove "Remove from Steam / uninstall" \
            update "Check for updates / update" \
            2>/dev/null || echo "cancel"
    else
        # All of this goes to stderr, not stdout -- the caller captures
        # this function's stdout as the actual selection
        # ("action=\"\$(choose_action)\"" below), so any of the menu
        # display text leaking onto stdout would get appended to that
        # and break the case match entirely.
        {
            echo "melonDS Remote Host"
            echo "  1) Launch melonDS now"
            echo "  2) Add to Steam (Big Picture / Gaming Mode)"
            echo "  3) Remove from Steam / uninstall"
            echo "  4) Check for updates / update"
            echo "  5) Exit"
        } >&2
        read -rp "Choice [1-5]: " choice
        case "${choice}" in
            1) echo "launch" ;;
            2) echo "steam-add" ;;
            3) echo "steam-remove" ;;
            4) echo "update" ;;
            *) echo "cancel" ;;
        esac
    fi
}

action="$(choose_action)"

case "${action}" in
    launch)
        # exec, not a plain call: this becomes the foreground process,
        # same as launching melonDS any other way (Steam shortcut,
        # double-clicking run-host.sh directly, etc.) -- no menu process
        # left hanging around behind it.
        exec ./launch-host.sh
        ;;
    steam-add)
        if ./install-steam-shortcut.sh; then
            info "Added melonDS Remote Host to Steam. Restart Steam (or switch to Gaming Mode) to see it, and set its Controller Layout to a plain Gamepad template once it's there."
        fi
        # A failure here already logged and showed its own error dialog
        # (install-steam-shortcut.sh has the same error-trap pattern as
        # this script) -- nothing more to do.
        ;;
    steam-remove)
        if confirm "This removes the Steam shortcut, the installed files, and the Distrobox container if one was created. Your ROMs, saves, and firmware are never touched. Continue?"; then
            if ./uninstall-steam-shortcut.sh; then
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
                if ./apply-update.sh; then
                    info "Updated to ${latest_version}. Restart Steam (or switch to Gaming Mode) to see the change."
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
chmod +x "${pkg_dir}/host/melonds-remote-host.sh"

cat > "${pkg_dir}/host/apply-update.sh" <<'WRAP'
#!/usr/bin/env bash
# Downloads the latest melonDS Remote release and installs it, by
# handing off to that release's own install-steam-shortcut.sh --
# reusing its already-verified stage-then-swap file safety and
# Distrobox/dnf-gated activation on immutable systems, rather than
# duplicating any of that logic here. This script's only job is
# fetching and extracting the new release archive. Normally invoked
# from melonds-remote-host.sh's "Check for updates" menu choice after
# the user confirms; also runnable standalone.
#
# Passes --force through to install-steam-shortcut.sh so an update
# doesn't silently do nothing just because Steam happens to be open --
# the confirmation prompt in the menu is the actual "are you sure" gate
# here, not that check. If Steam genuinely was never set up on this
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

error_log="${HOME}/.config/melonds-remote/install.log"
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") apply-update.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "melonDS Remote Host" --error "Updating failed: ${failing_cmd}
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
"${extracted_dir}/host/install-steam-shortcut.sh" --force
WRAP
chmod +x "${pkg_dir}/host/apply-update.sh"

cat > "${pkg_dir}/host/install-steam-shortcut.sh" <<'WRAP'
#!/usr/bin/env bash
# Registers the melonDS Remote host as a Steam non-Steam-game shortcut,
# so it can be launched from Steam Big Picture/Gaming Mode with only a
# controller (GitHub issue #10). Mirrors
# ../client/install-steam-shortcut.sh's approach, reusing the same
# layout-agnostic steam_shortcut.py -- bundled flat alongside this
# script rather than shared from a top-level scripts/ directory, since
# host/ is fully self-contained (same reasoning as client/lib/ bundling
# SDL3).
#
# Copies this whole host/ directory into a fixed central location
# (~/.config/melonds-remote/install/ -- the same directory
# install-host-distrobox.sh already uses on immutable systems) and
# points the shortcut at launch-host.sh there, not at melonDS or
# run-host.sh directly: that one entry point picks Distrobox or a direct
# launch depending on whether this is an immutable system, so the same
# shortcut works either way. Re-running this from a newer release's
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

# Surfaces failures visibly instead of just closing silently when
# double-clicked with no visible terminal attached -- logs to a
# persistent file and, when available (SteamOS Desktop Mode/Bazzite are
# both KDE Plasma), pops up a graphical error dialog via kdialog. Same
# pattern as client/install-steam-shortcut.sh.
error_log="${HOME}/.config/melonds-remote/install.log"
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") install-steam-shortcut.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "melonDS Remote Host" \
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
central_install_dir="${HOME}/.config/melonds-remote/install"
staging_dir="${central_install_dir}.new"
previous_dir="${central_install_dir}.previous"

if [[ "${dry_run}" -eq 0 ]]; then
    if [[ -f /run/ostree-booted ]] || command -v rpm-ostree >/dev/null 2>&1; then
        echo "Preparing the Distrobox container (this can take a few minutes the first time) ..."
        ./install-host-distrobox.sh --install-only
    else
        rm -rf "${staging_dir}"
        mkdir -p "${staging_dir}"
        cp -a . "${staging_dir}/"
        chmod +x "${staging_dir}"/*.sh

        rm -rf "${previous_dir}"
        if [[ -d "${central_install_dir}" ]]; then
            mv "${central_install_dir}" "${previous_dir}"
        fi
        mv "${staging_dir}" "${central_install_dir}"

        # Same reasoning as install-host-distrobox.sh's equivalent copy:
        # keeps "../check-for-updates.sh" resolvable from a copy of
        # melonds-remote-host.sh later run from inside the central
        # directory itself.
        cp "../check-for-updates.sh" "$(dirname "${central_install_dir}")/check-for-updates.sh" 2>/dev/null || true
        cp "../VERSION" "$(dirname "${central_install_dir}")/VERSION" 2>/dev/null || true
    fi
fi

python3 ./steam_shortcut.py \
    --exe "${central_install_dir}/launch-host.sh" \
    --name "melonDS Remote Host" \
    --launch-options "${launch_options}" \
    "${extra_args[@]}" && shortcut_exit=0 || shortcut_exit=$?
if [[ "${shortcut_exit}" -ne 0 ]]; then
    on_error "${shortcut_exit}" "${LINENO}" "steam_shortcut.py"
    exit "${shortcut_exit}"
fi
WRAP
chmod +x "${pkg_dir}/host/install-steam-shortcut.sh"

cat > "${pkg_dir}/host/uninstall-steam-shortcut.sh" <<'WRAP'
#!/usr/bin/env bash
# Removes the "melonDS Remote Host" Steam non-Steam-game shortcut added
# by install-steam-shortcut.sh, the central install directory
# (~/.config/melonds-remote/install) it copies everything into, and the
# Distrobox container if one was created -- i.e. this is the complete
# host uninstall once a Steam shortcut has been set up. (If you only
# ever used install-host-distrobox.sh directly and never installed the
# Steam shortcut, uninstall-host-distrobox.sh alone is equivalent.)
#
# Always targets the fixed central install directory below, regardless
# of where this script itself is run from -- this exact file is also
# the copy install-steam-shortcut.sh placed there, and must keep
# removing the same shortcut from there indefinitely, even after the
# original archive has been deleted (same reasoning as
# client/uninstall-steam-shortcut.sh).
set -euo pipefail

error_log="${HOME}/.config/melonds-remote/install.log"
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") uninstall-steam-shortcut.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "melonDS Remote Host" \
            --error "Removing the Steam shortcut failed: ${failing_cmd}
(exit code ${exit_code})

Details logged to:
${error_log}" 2>/dev/null || true
    fi
}
trap 'ec=$?; on_error "${ec}" "${LINENO}" "${BASH_COMMAND}"' ERR

# Keep in sync with the same constant in install-steam-shortcut.sh,
# install-host-distrobox.sh, and uninstall-host-distrobox.sh.
central_install_dir="${HOME}/.config/melonds-remote/install"
self_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
container_name="melonds-remote-host"

steam_shortcut_py=""
for candidate in \
    "${central_install_dir}/steam_shortcut.py" \
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

python3 "${steam_shortcut_py}" \
    --exe "${central_install_dir}/launch-host.sh" \
    --name "melonDS Remote Host" \
    --remove \
    "$@"

dry_run=0
for arg in "$@"; do
    [[ "${arg}" == "--dry-run" ]] && dry_run=1
done

if [[ "${dry_run}" -eq 0 ]]; then
    if command -v distrobox >/dev/null 2>&1 && distrobox list 2>/dev/null | grep -qw "${container_name}"; then
        echo "Removing Distrobox container \"${container_name}\" ..."
        distrobox rm "${container_name}" --force
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
chmod +x "${pkg_dir}/host/uninstall-steam-shortcut.sh"

cp "${repo_root}/docs/building.md" "${repo_root}/docs/steam-deck-setup.md" \
   "${repo_root}/docs/bazzite-host-setup.md" "${repo_root}/docs/troubleshooting.md" \
   "${repo_root}/docs/known-limitations.md" "${repo_root}/docs/protocol.md" \
   "${pkg_dir}/docs/"
cp "${repo_root}/LICENSE" "${pkg_dir}/"

cat > "${pkg_dir}/check-for-updates.sh" <<'WRAP'
#!/usr/bin/env bash
# Checks whether a newer melonDS Remote release is published on GitHub --
# read-only, no download or install of anything happens here (GitHub
# issue #10: "simplify... updates"). Never blocks or fails a real host/
# client launch on this -- run-host.sh/run-client.sh don't call this
# automatically, precisely so a slow/unreachable network never adds
# delay to actually starting the app; run this yourself whenever you
# want to check.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

current_version="$(cat VERSION 2>/dev/null || echo "unknown")"
repo="Crimson3076/DualDeck"

if ! command -v curl >/dev/null 2>&1; then
    echo "melonDS Remote ${current_version} -- install 'curl' to enable update checks."
    exit 0
fi

api_response="$(curl -fsSL --max-time 5 \
    "https://api.github.com/repos/${repo}/releases/latest" 2>/dev/null)"
if [[ -z "${api_response}" ]]; then
    echo "melonDS Remote ${current_version} -- couldn't reach GitHub to check for updates (offline?)."
    exit 0
fi

latest_version="$(echo "${api_response}" | grep -o '"tag_name" *: *"[^"]*"' | head -1 | sed -E 's/.*"([^"]+)"$/\1/')"
if [[ -z "${latest_version}" ]]; then
    echo "melonDS Remote ${current_version} -- couldn't parse GitHub's response to check for updates."
    exit 0
fi

if [[ "${latest_version}" == "${current_version}" ]]; then
    echo "melonDS Remote ${current_version} -- you're on the latest version."
else
    echo "melonDS Remote ${current_version} -- update available: ${latest_version}"
    echo "  https://github.com/${repo}/releases/tag/${latest_version}"
fi
WRAP
chmod +x "${pkg_dir}/check-for-updates.sh"

commit_full="$(cd "${repo_root}" && git rev-parse HEAD)"
built_at="$(date -u +"%Y-%m-%d %H:%M UTC")"
cat > "${pkg_dir}/RELEASE_NOTES.md" <<NOTES
# melonDS Remote (\`${version_tag}\`)

Built from commit \`${commit_full}\` on branch \`${branch_name}\`,
${built_at}. This is a distinct, permanently-retained release -- it will
not be overwritten by a later build, so if this version has a problem,
just grab an earlier one from the Releases page instead. Double-click
\`check-for-updates.sh\` any time to check whether a newer version has
been published since (read-only -- it only checks and reports, it
doesn't download or change anything).

Contains \`host/\` (melonDS patched with
\`host/melonds-patches/0001-remote-server-integration.patch\`, built
against upstream commit \`${MELONDS_COMMIT}\`) and \`client/\` (SDL3
Steam Deck client, SDL3 ${SDL3_TAG} bundled in \`client/lib/\`). Both are
Linux x86_64 binaries built on Ubuntu (GitHub Actions \`ubuntu-latest\` or
equivalent) -- see \`docs/known-limitations.md\` (bundled here) for what's
verified and portability notes.

## Running it

Every script here (\`run-host.sh\`, \`run-client.sh\`,
\`install-steam-shortcut.sh\`, \`uninstall-steam-shortcut.sh\`,
\`check-for-updates.sh\`, and on immutable systems
\`install-host-distrobox.sh\`/\`uninstall-host-distrobox.sh\`) is directly
double-click-runnable from a file manager -- no arguments or typing
required for any of them. On
SteamOS Desktop Mode / Bazzite (both KDE Plasma/Dolphin), double-clicking
an executable \`.sh\` file offers to run it directly, no terminal needed.
(On a GNOME-based file manager instead, you may need to enable
"Executable Text Files: Run" in Nautilus's preferences first, or
right-click > Open Terminal Here as a fallback.) Both \`run-host.sh\` and
\`run-client.sh\` also check for missing runtime system libraries and try
to install them automatically (apt/dnf/pacman) before launching -- you
shouldn't need to install anything by hand on a normal desktop Linux
distro. On an immutable-filesystem distro (Bazzite, SteamOS in Game
Mode), they'll tell you that instead of guessing, since auto-installing
onto those needs a reboot or a Distrobox -- see
\`docs/bazzite-host-setup.md\` / \`docs/steam-deck-setup.md\`.

**Host** (on your Linux HTPC): double-click \`host/run-host.sh\`, or from
a terminal:
\`\`\`sh
cd host
./run-host.sh
\`\`\`
No ROM path needed -- melonDS opens normally and you pick one through
its own File > Open ROM (defaults to EmuDeck's ROM folder the first
time, remembers your last one after that). No
\`MELONDS_REMOTE_AUTH_TOKEN\` needed either -- on a new client's first
connection attempt, the host logs a pending request for you to approve
(type \`approve <id>\`, shown in the log, or click Approve on the popup
if you're running the melonDS GUI). Set \`MELONDS_REMOTE_AUTH_TOKEN\`
instead if you'd rather use a static shared secret.

**Client** (on your Steam Deck, or any Linux x86_64 machine with a
gamepad): double-click \`client/run-client.sh\`, or from a terminal:
\`\`\`sh
cd client
./run-client.sh
\`\`\`
No arguments needed -- it scans the LAN and shows a pick-a-host list
every launch. First connection to a new host needs a human to approve it
at the host (see above); no typing is needed on the client.

To add it as a Steam Gaming Mode shortcut without the manual "Add a
Non-Steam Game" steps: close Steam, then double-click
\`client/install-steam-shortcut.sh\`, or from a terminal (from the
\`client/\` directory):
\`\`\`sh
./install-steam-shortcut.sh
\`\`\`
Applies to every local Steam user automatically -- no prompt to pick one,
since the Deck's controller-only input can't type an answer to that kind
of question. Copies everything the shortcut needs into a fixed central
directory (\`~/.config/melonds-remote-client/install/\`), so this
extracted folder can be deleted afterward -- re-running install from a
newer release later still updates the same shortcut rather than
duplicating it. See \`docs/steam-deck-setup.md\` for what this does and
the Controller Layout step it doesn't automate. To undo it later, close
Steam and double-click \`client/uninstall-steam-shortcut.sh\` the same
way (works even from this same disposable folder, or later straight from
the central directory) -- also applies to every local Steam user
automatically, and deletes that central directory too.
NOTES

# The archive itself gets a *constant* filename (unlike the internal
# directory name above, which embeds the commit for clarity once
# extracted) so that re-uploading it to the same GitHub Release asset
# name replaces the previous build instead of accumulating a new,
# differently-named asset on every push (softprops/action-gh-release
# matches on asset filename to decide what to replace).
archive_name="melonds-remote-linux-x86_64.tar.gz"
tar czf "${out_dir}/${archive_name}" -C "${work_dir}" "${pkg_name}"
echo "Wrote ${out_dir}/${archive_name} (contains ${pkg_name}/)"
