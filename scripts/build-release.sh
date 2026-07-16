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
pkg_name="melonds-remote-${commit_short}-linux-x86_64"
pkg_dir="${work_dir}/${pkg_name}"
rm -rf "${pkg_dir}"
mkdir -p "${pkg_dir}/host" "${pkg_dir}/client/lib" "${pkg_dir}/docs" "${pkg_dir}/scripts/lib"

cp "${melonds_src}/build/melonDS" "${pkg_dir}/host/melonDS"
chmod +x "${pkg_dir}/host/melonDS"
ldd "${melonds_src}/build/melonDS" | awk '{print $1}' | sort -u \
    > "${pkg_dir}/host/host-shared-library-dependencies.txt"

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
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

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

if [[ "${dry_run}" -eq 0 ]]; then
    rm -rf "${central_install_dir}"
    mkdir -p "${central_install_dir}/client/lib" "${central_install_dir}/scripts/lib"

    cp melonds-remote-client "${central_install_dir}/client/melonds-remote-client"
    chmod +x "${central_install_dir}/client/melonds-remote-client"
    cp -a lib/. "${central_install_dir}/client/lib/"
    cp run-client.sh "${central_install_dir}/client/run-client.sh"
    chmod +x "${central_install_dir}/client/run-client.sh"
    cp uninstall-steam-shortcut.sh "${central_install_dir}/client/uninstall-steam-shortcut.sh"
    chmod +x "${central_install_dir}/client/uninstall-steam-shortcut.sh"
    cp ../scripts/lib/ensure-packages.sh "${central_install_dir}/scripts/lib/ensure-packages.sh"
    cp ../scripts/lib/steam_shortcut.py "${central_install_dir}/scripts/lib/steam_shortcut.py"
fi

exec python3 ../scripts/lib/steam_shortcut.py \
    --exe "${central_install_dir}/client/run-client.sh" \
    --launch-options "${launch_options}" \
    "${extra_args[@]}"
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

if [[ "${dry_run}" -eq 0 && -d "${central_install_dir}" ]]; then
    echo "Removing ${central_install_dir}"
    cd /
    rm -rf -- "${central_install_dir}"
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
source ../scripts/lib/ensure-packages.sh

ensure_packages "host runtime" \
    "libcurl4-gnutls-dev libpcap0.8-dev libsdl2-dev libarchive-dev libenet-dev libzstd-dev libfaad-dev qt6-base-dev qt6-base-private-dev qt6-multimedia-dev qt6-svg-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libwayland-dev libxkbcommon-dev libdrm-dev libgbm-dev libdecor-0-dev" \
    "libcurl-devel libpcap-devel SDL2-devel libarchive-devel enet-devel libzstd-devel faad2-devel qt6-qtbase-devel qt6-qtmultimedia-devel qt6-qtsvg-devel libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXfixes-devel libXi-devel libXScrnSaver-devel wayland-devel libxkbcommon-devel libdrm-devel mesa-libgbm-devel libdecor-devel" \
    "curl libpcap sdl2 libarchive enet zstd faad2 qt6-base qt6-multimedia qt6-svg libx11 libxext libxrandr libxcursor libxfixes libxi libxss wayland libxkbcommon libdrm mesa libdecor" \
    || echo "warning: could not verify/install host runtime libraries automatically; see host-shared-library-dependencies.txt and docs/troubleshooting.md" >&2

export MELONDS_REMOTE_ENABLE=1
exec ./melonDS "$@"
WRAP
chmod +x "${pkg_dir}/host/run-host.sh"

cp "${repo_root}/docs/building.md" "${repo_root}/docs/steam-deck-setup.md" \
   "${repo_root}/docs/bazzite-host-setup.md" "${repo_root}/docs/troubleshooting.md" \
   "${repo_root}/docs/known-limitations.md" "${repo_root}/docs/protocol.md" \
   "${pkg_dir}/docs/"
cp "${repo_root}/LICENSE" "${pkg_dir}/"

commit_full="$(cd "${repo_root}" && git rev-parse HEAD)"
built_at="$(date -u +"%Y-%m-%d %H:%M UTC")"
cat > "${pkg_dir}/RELEASE_NOTES.md" <<NOTES
# melonDS Remote

Built from commit \`${commit_full}\` on branch \`${branch_name}\`,
${built_at}. This is a distinct, permanently-retained release -- it will
not be overwritten by a later build, so if this version has a problem,
just grab an earlier one from the Releases page instead.

Contains \`host/\` (melonDS patched with
\`host/melonds-patches/0001-remote-server-integration.patch\`, built
against upstream commit \`${MELONDS_COMMIT}\`) and \`client/\` (SDL3
Steam Deck client, SDL3 ${SDL3_TAG} bundled in \`client/lib/\`). Both are
Linux x86_64 binaries built on Ubuntu (GitHub Actions \`ubuntu-latest\` or
equivalent) -- see \`docs/known-limitations.md\` (bundled here) for what's
verified and portability notes.

## Running it

Every script here (\`run-host.sh\`, \`run-client.sh\`,
\`install-steam-shortcut.sh\`, \`uninstall-steam-shortcut.sh\`) is
directly double-click-runnable from a file manager -- no arguments or
typing required for any of them. On
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
