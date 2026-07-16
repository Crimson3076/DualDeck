#!/usr/bin/env bash
# Builds a complete downloadable release package from scratch: the
# patched melonDS host binary and the SDL3 client binary, packaged
# together with docs and wrapper scripts into one tar.gz. Used by
# .github/workflows/release.yml to publish a rolling "latest" GitHub
# Release on every push, and safe to run locally the same way.
#
# Requires (Ubuntu 24.04 package names -- see host/melonds-patches/README.md
# for melonDS's own list, and docs/building.md for the SDL3-from-source
# recipe; both are mirrored here since neither is packaged for every distro):
#   cmake extra-cmake-modules ninja-build build-essential git python3 \
#   libcurl4-gnutls-dev libpcap0.8-dev libsdl2-dev libarchive-dev \
#   libenet-dev libzstd-dev libfaad-dev \
#   qt6-base-dev qt6-base-private-dev qt6-multimedia-dev qt6-svg-dev \
#   libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev \
#   libxi-dev libxss-dev libwayland-dev libxkbcommon-dev libdrm-dev \
#   libgbm-dev libdecor-0-dev
set -euo pipefail

MELONDS_COMMIT="10a173b5536fc75cd93f8a3868349dad963542ef"
SDL3_TAG="release-3.2.16"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work_dir="${BUILD_RELEASE_WORKDIR:-$(mktemp -d)}"
out_dir="${BUILD_RELEASE_OUTPUT_DIR:-${repo_root}/release-out}"
mkdir -p "${work_dir}" "${out_dir}"

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
mkdir -p "${pkg_dir}/host" "${pkg_dir}/client/lib" "${pkg_dir}/docs"

cp "${melonds_src}/build/melonDS" "${pkg_dir}/host/melonDS"
chmod +x "${pkg_dir}/host/melonDS"
ldd "${melonds_src}/build/melonDS" | awk '{print $1}' | sort -u \
    > "${pkg_dir}/host/host-shared-library-dependencies.txt"

cp "${repo_build}/client/melonds-remote-client" "${pkg_dir}/client/melonds-remote-client"
chmod +x "${pkg_dir}/client/melonds-remote-client"
cp -a "${sdl3_install}"/lib/libSDL3.so* "${pkg_dir}/client/lib/"

cat > "${pkg_dir}/client/run-client.sh" <<'WRAP'
#!/usr/bin/env bash
# Runs melonds-remote-client with the bundled SDL3 shared library.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
LD_LIBRARY_PATH="$(pwd)/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec ./melonds-remote-client "$@"
WRAP
chmod +x "${pkg_dir}/client/run-client.sh"

cat > "${pkg_dir}/host/run-host.sh" <<'WRAP'
#!/usr/bin/env bash
# Runs the patched melonDS binary with the remote server enabled.
# Usage: ./run-host.sh --boot always /path/to/your/game.nds
# Omit MELONDS_REMOTE_AUTH_TOKEN to use the 6-digit pairing-code flow
# instead of a static shared secret.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
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
# melonDS Remote -- rolling build

Built automatically from commit \`${commit_full}\` on branch
\`${branch_name}\`, ${built_at}. This is a rolling release: it always
reflects the newest push, so double check the commit hash above against
what you expect if timing matters.

Contains \`host/\` (melonDS patched with
\`host/melonds-patches/0001-remote-server-integration.patch\`, built
against upstream commit \`${MELONDS_COMMIT}\`) and \`client/\` (SDL3
Steam Deck client, SDL3 ${SDL3_TAG} bundled in \`client/lib/\`). Both are
Linux x86_64 binaries built on Ubuntu (GitHub Actions \`ubuntu-latest\` or
equivalent) -- see \`docs/known-limitations.md\` (bundled here) for what's
verified and portability notes.

## Running it

Host (on your Linux HTPC):
\`\`\`sh
cd host
./run-host.sh --boot always /path/to/your/game.nds
\`\`\`
No \`MELONDS_REMOTE_AUTH_TOKEN\` needed -- the host will show a 6-digit
pairing code on first connection (spec section 13). Set
\`MELONDS_REMOTE_AUTH_TOKEN\` instead if you'd rather use a static shared
secret. See \`docs/bazzite-host-setup.md\` for Bazzite/immutable-distro
dependency installation.

Client (on your Steam Deck, or any Linux x86_64 machine with a gamepad):
\`\`\`sh
cd client
./run-client.sh --host <your-htpc-ip>
\`\`\`
First connection to a new host prompts for the pairing code; it's
remembered after that. See \`docs/steam-deck-setup.md\` for Gaming Mode
shortcut setup.
NOTES

tar czf "${out_dir}/${pkg_name}.tar.gz" -C "${work_dir}" "${pkg_name}"
echo "Wrote ${out_dir}/${pkg_name}.tar.gz"
