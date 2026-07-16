# Building

**Don't want to build anything?** A manually-triggered workflow
publishes ready-to-run builds (host + client binaries) as versioned
GitHub Releases, each kept around permanently -- see the top-level
`README.md`'s "Download a build" section. The instructions below are for
building from source yourself (e.g. to modify the code).

## Requirements

- CMake >= 3.20
- A C++20 compiler (GCC 13 / Clang 16 or newer verified)
- For `client/` only: SDL3 development headers/libraries. Not required to
  build `protocol/` or `host/remote-server/`.
- For the real melonDS-integrated host
  (`host/melonds-patches/0001-remote-server-integration.patch`): Qt6,
  SDL2, and OpenGL development packages, per melonDS's own `BUILD.md` --
  see `host/melonds-patches/README.md` for the exact package list this
  was verified against. Not required for `protocol/`, `host/remote-server/`
  (the standalone prototype), or `client/`.

## Building the protocol library and standalone host server

```sh
cmake -S . -B build \
    -DMELONDS_REMOTE_BUILD_CLIENT=OFF \
    -DMELONDS_REMOTE_BUILD_HOST=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

This builds and tests everything that has no external dependencies:
`protocol/` (static library + unit tests) and
`host/remote-server/melonds-remote-server` (a standalone binary that
serves a synthetic 256x192 test-pattern bottom screen and logs received
controller/touch input -- see `docs/architecture.md` for why it doesn't
depend on melonDS yet).

Development builds should also enable strict warnings:

```sh
cmake -S . -B build \
    -DMELONDS_REMOTE_BUILD_CLIENT=OFF \
    -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror"
```

## Building the SDL3 client

```sh
cmake -S . -B build -DMELONDS_REMOTE_BUILD_CLIENT=ON
cmake --build build -j"$(nproc)"
```

This requires `SDL3` to be discoverable by `find_package(SDL3)` (e.g. via
your distribution's `sdl3-devel`/`libsdl3-dev` package, or a local install
with `SDL3Config.cmake` on `CMAKE_PREFIX_PATH`). Steam Deck / Bazzite
development machines are the primary target; if your distro doesn't
package SDL3 yet (e.g. Ubuntu 24.04 doesn't), build it from source first:

```sh
git clone --depth 1 --branch release-3.2.16 https://github.com/libsdl-org/SDL.git
cmake -S SDL -B SDL/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/path/to/sdl3-install
cmake --build SDL/build -j"$(nproc)"
cmake --install SDL/build
cmake -S . -B build -DMELONDS_REMOTE_BUILD_CLIENT=ON -DCMAKE_PREFIX_PATH=/path/to/sdl3-install
```

This has been verified to work: building SDL3 3.2.16 from source this way
and configuring against it produces a `melonds-remote-client` that
compiles cleanly and has been run successfully (real handshake, real
sustained video/input traffic) against both the standalone host prototype
and the actual patched melonDS host -- see `docs/known-limitations.md`.
Not yet tested: real Steam Deck hardware/gamepad (this was verified in a
headless, gamepad-less environment).

## Running the standalone host server locally

```sh
./build/host/remote-server/melonds-remote-server --bind 127.0.0.1 \
    --control-port 8760 --input-port 8761 --video-port 8762 --timeout-ms 500 \
    --state-dir ~/.config/melonds-remote
```

`--bind 127.0.0.1` above is specific to same-machine testing (host and
client both running here). For real use across two machines, omit
`--bind` entirely -- it now defaults to `0.0.0.0` (all interfaces), which
is what makes the client's LAN discovery actually able to reach the host
it just found; see `docs/bazzite-host-setup.md`.

Without `--auth-token`, the server runs in **device-approval mode** (spec
section 13, adapted): an unrecognized client's connection request is
queued and logged here, naming the client and its address -- type
`approve <device-id-prefix>` and press Enter to let it connect. Once
approved (in `--state-dir`, if given), reconnects need no re-approval.
Pass `--auth-token SECRET` instead for a static pre-shared token that
bypasses device approval entirely (useful for scripting/CI). Run
`--help` for the full flag list, including `--pending-request-ttl-s`.

See `scripts/run-host.sh` for a convenience wrapper, and
`docs/testing.md` for a smoke-test script that exercises the handshake
(including the auth-token and rate-limiting paths), UDP input path, and
video path against a running server without needing the SDL3 client
built. (The smoke test uses `--auth-token`, the static/legacy path, since
it doesn't simulate a human approving a device.)

## Running the SDL3 client locally

```sh
./build/client/melonds-remote-client --host 192.168.1.50
```

Also accepts a bare positional host address (`melonds-remote-client
192.168.1.50`) for `scripts/run-client.sh`'s convenience form. If the
host is in device-approval mode (the default) and this client hasn't
connected to it before, the client shows "WAITING FOR APPROVAL ON HOST
..." -- no typing needed on the client side; approve it from the host's
console (or the melonDS-integrated host's popup dialog) instead. The
client's own persistent device identity is saved to
`~/.config/melonds-remote-client/device_id.txt` and reused for every
host, forever. Pass `--auth-token SECRET` only if the host was started
with a static token instead. The client retries the connection with
capped exponential backoff (1s up to 5s) on a background thread whenever
it isn't currently connected -- the same retry loop naturally covers both
"host temporarily down" and "not yet approved", since there's nothing
client-side to pause for -- so it recovers automatically once the host
approves it, restarts, or a network interruption clears (spec section
7.2). This reconnect behavior, and the device-approval flow above, have
been exercised against a real host process with the real client binary
in this sandbox (see `docs/known-limitations.md`) but not yet against
real Steam Deck hardware/gamepad.

## Building the full release package yourself

`scripts/build-release.sh` is exactly what `.github/workflows/release.yml`
runs to produce the downloadable build mentioned at the top of this
document: it builds SDL3 from source, clones+patches+builds melonDS, then
builds this repo's client/host against that SDL3, and packages everything
into a tar.gz identical in structure to what the Releases page publishes.
Useful if you want to reproduce that exact build locally, e.g. to test a
local change before pushing:

```sh
./scripts/build-release.sh
# writes ./release-out/melonds-remote-<commit>-linux-x86_64.tar.gz
```

Build-time dependencies (melonDS's own Qt6/SDL2/OpenGL requirements, plus
the X11/Wayland headers SDL3 needs) are detected and installed
automatically -- `scripts/lib/ensure-packages.sh` checks what's already
present and only installs what's missing, via whichever of apt/dnf/pacman
it finds (asking for `sudo` if needed). No manual `apt install` step
required. On an immutable-filesystem distro (Bazzite, SteamOS in Game
Mode) it won't guess at an unattended install -- it'll tell you to use a
Distrobox instead (see `docs/bazzite-host-setup.md`/`docs/steam-deck-setup.md`)
or disable read-only mode yourself first.

The **packaged `run-host.sh`/`run-client.sh`** inside the resulting
archive do the same check for *runtime* libraries when the end user runs
them, so downloading a release and running it shouldn't require any
manual dependency installation either, on a normal desktop Linux distro.

Takes 15-20 minutes since both melonDS and SDL3 are built from source
every time -- set `BUILD_RELEASE_WORKDIR` to a persistent directory to
skip rebuilding SDL3 on a second run (it's cached by cmake-install-prefix
existence, not by any correctness-sensitive input, since the SDL3 version
built is a fixed pinned tag independent of this repo's own commits).

## melonDS itself

This repository does not vendor melonDS; `host/melonds-patches/` patches
a normal upstream clone at a pinned commit instead (see that directory's
`README.md`). To inspect or build unpatched upstream melonDS for
reference (e.g. to verify the call sites in
`docs/melonds-integration-analysis.md` still match current `master`):

```sh
git clone https://github.com/melonDS-emu/melonDS.git
cd melonDS
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

See melonDS's own `BUILD.md` for its Qt6/SDL2/OpenGL dependency list.
