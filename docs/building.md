# Building

## Requirements

- CMake >= 3.20
- A C++20 compiler (GCC 13 / Clang 16 or newer verified)
- For `client/` only: SDL3 development headers/libraries. Not required to
  build `protocol/` or `host/remote-server/`.
- For a real melonDS-integrated host (future work, not yet in this repo):
  Qt6, SDL2, and OpenGL development packages, per melonDS's own
  `BUILD.md`. Not required for anything currently in this repository.

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

Without `--auth-token`, the server runs in **pairing mode** (spec section
13's "six-digit pairing code"): an unrecognized client is shown a
6-digit code to enter once, after which the host remembers it (in
`--state-dir`, if given) and reconnects don't need a code again. Pass
`--auth-token SECRET` instead for a static pre-shared token that bypasses
pairing entirely (useful for scripting/CI). Run `--help` for the full
flag list, including `--pairing-code-ttl-s`.

See `scripts/run-host.sh` for a convenience wrapper, and
`docs/testing.md` for a smoke-test script that exercises the handshake
(including the auth-token and rate-limiting paths), UDP input path, and
video path against a running server without needing the SDL3 client
built. (The smoke test uses `--auth-token`, the static/legacy path, since
it doesn't simulate a human entering a pairing code.)

## Running the SDL3 client locally

```sh
./build/client/melonds-remote-client --host 192.168.1.50
```

Also accepts a bare positional host address (`melonds-remote-client
192.168.1.50`) for `scripts/run-client.sh`'s convenience form. If the
host is in pairing mode (the default) and this is the first connection to
it, the client shows a 6-digit code-entry screen (SDL text input, so
Steam's on-screen keyboard drives it in Gaming Mode) -- enter the code
the host just displayed. The client saves the token it's issued to
`~/.config/melonds-remote-client/pairing_tokens.txt` and reuses it
silently on every future run against that host address. Pass
`--auth-token SECRET` only if the host was started with a static token
instead. The client retries the connection with capped exponential
backoff (1s up to 5s) on a background thread whenever it isn't currently
connected (paused while awaiting pairing-code entry, so it doesn't burn
through the host's connection-attempt rate limit with a code that hasn't
changed), so it recovers automatically after the host
restarts or a network interruption (spec section 7.2). This reconnect
behavior, and the pairing flow above, have been exercised against a real
host process with the real client binary in this sandbox (see
`docs/known-limitations.md`) but not yet against real Steam Deck
hardware/gamepad.

## melonDS itself

This repository does not vendor melonDS. To inspect or build upstream
melonDS for reference (e.g. to verify the call sites in
`docs/melonds-integration-analysis.md` still match current `master`):

```sh
git clone https://github.com/melonDS-emu/melonDS.git
cd melonDS
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

See melonDS's own `BUILD.md` for its Qt6/SDL2/OpenGL dependency list --
building melonDS itself is out of scope for this repository until the
Phase 1 host/client skeleton (this repo's current content) is validated
and a melonDS fork integration begins.
