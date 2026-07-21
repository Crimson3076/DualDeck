# melonDS Remote

A Linux-focused system for running Nintendo DS games through melonDS on an
HTPC while using a Steam Deck as the handheld controller and bottom
screen — like a Wii U GamePad, with the TV showing the DS top screen and
the Steam Deck showing the bottom screen plus all controls.

See [`SPEC.md`](SPEC.md) for the full project scope and requirements.

## Download a build

`.github/workflows/release.yml` builds the host and client binaries and
publishes them as a GitHub Release -- grab one from the repo's
[**Releases**](../../releases) page rather than building from source,
unless you specifically want to modify the code. It's triggered manually
(not on every push), and each run gets its own version tag and release
rather than overwriting a shared one, so older builds stay downloadable
if a newer one turns out to be broken. See `RELEASE_NOTES.md` inside the
archive for exactly which commit a given release was built from.

The easiest way to install: download **`DualDeck-Installer.sh`** from
that same Releases page and run it -- it downloads and verifies (via
`SHA256SUMS`) the rest for you and offers Install Client / Install Host
/ Install Both / Repair / Uninstall, so you never need to manually
extract an archive yourself. See `docs/known-limitations.md`'s
installer section for exactly what it does and doesn't cover yet (it's
Phase 1 of GitHub issue #26's larger installer/auto-update rework).

Everything you need to run is one script per side, no typing required:
double-click `host/melonds-remote-host.sh` on the machine running the
emulator, and `client/melonds-remote-client.sh` on your Steam Deck (or
any Linux machine with a gamepad). Each opens a small menu; the host's
"Launch..." choice picks which system to run -- Nintendo DS (melonDS,
the default/most-supported path), Nintendo 3DS (Azahar, experimental --
see `docs/known-limitations.md`'s AzaharAdapter entry), host-control
mode only (no emulator, browse the host's own UI from the client
instead -- also experimental), or a custom emulator you've already
patched yourself with `scripts/patch-existing-emulator.sh`, for anyone
who doesn't want a separate DualDeck-managed copy. The 3DS and
host-control-only choices both need a shared secret instead of the
usual per-device approval prompt -- the menu asks for one when you pick
either. Add to Steam (Big Picture/Gaming Mode), Remove from Steam, and
Check for updates (which offers to install one automatically if it
finds one) round out the rest of the menu, so there's nothing else in
either `host/` or `client/` you need to open directly; the rest of
what's in there (under each directory's `internal/` subfolder) is what
the menu calls on your behalf. The client additionally checks for and
installs updates automatically on every launch (Steam shortcut
included), no menu or confirmation needed. On SteamOS Desktop Mode or
Bazzite (both KDE Plasma/Dolphin),
double-clicking an executable `.sh` file offers to run it directly --
see `docs/steam-deck-setup.md`/`docs/bazzite-host-setup.md` for the
quick-start section at the top of each, and the archive's own bundled
`README.md` for the full rundown.

## Status

**Phase 0, a Phase 1 skeleton, Phase 2 network-robustness work, and a
first melonDS integration patch are all implemented, and the SDL3 client
is now build- and run-verified.** The patch builds, its handshake/device-approval
and video-capture/input-injection paths are all confirmed end-to-end
against an actual running (homebrew) program driven through the real
network pipeline with the real client binary — see below for exactly
what is and isn't verified yet.

- [`docs/melonds-integration-analysis.md`](docs/melonds-integration-analysis.md) —
  where melonDS exposes bottom-screen frames and accepts input, verified
  by building and patching real melonDS, not just reading source.
- [`protocol/`](protocol/) — versioned wire format (including the Hello/
  HelloAck handshake payloads and LAN discovery), touch-coordinate
  mapping, fail-safe input-state tracking, and connection-attempt rate
  limiting. Fully unit tested, no external dependencies.
- [`host/remote-server/`](host/remote-server/) — a standalone host binary
  implementing the full network/threading model (TCP control, UDP input,
  TCP video) against a synthetic test-pattern frame source and a logging
  input sink, so it can be built and tested without melonDS or a display.
  Authenticates either via device approval (default -- a human at the
  host approves or denies each new client by name/address, no typing on
  either side; see `docs/protocol.md`'s "Authentication and device
  approval") or a static pre-shared token (`--auth-token`, opt-in); UDP
  input and the video channel are both gated on a completed,
  authenticated handshake from the same source address. Also broadcasts
  itself for LAN discovery so the client doesn't need to be told its
  address.
- [`host/melonds-patches/`](host/melonds-patches/) — a real patch against
  upstream melonDS (`0001-remote-server-integration.patch`) that vendors
  the protocol/host code above into melonDS's own build and wires it to
  `GPU::GetFramebuffers()` and the input/hotkey system, pops an
  Approve/Deny dialog for new connection requests, and defaults the "Open
  ROM" dialog to EmuDeck's NDS folder. Confirmed to build from a fresh
  clone; its video path was confirmed to deliver a real, non-static frame
  from a minimal original homebrew ROM
  ([`tests/homebrew-test-rom/`](tests/homebrew-test-rom/)) direct-booted
  in the patched binary — and, going further, that ROM was extended to
  read real DS button input and driven through the actual UDP-input →
  `SetKeyMask()` → CPU → framebuffer → network pipeline, confirming DS
  controls sent remotely genuinely affect a running program, and
  conclusively resolving the pixel format as **BGRA8888** with engine B
  as the bottom screen. See `host/melonds-patches/README.md` and
  `tests/homebrew-test-rom/README.md` for the full verification account,
  including the honest caveat that this is an original homebrew program,
  not a commercial game.
- [`client/`](client/) — an SDL3 Steam Deck client with automatic
  reconnect (capped exponential backoff), a LAN host-discovery/selection
  screen shown on every launch, and no typing required anywhere in the
  connection flow (device approval happens on the host). Built from
  source (SDL3 3.2.16, not packaged for the development environment used
  here) and **run successfully** against both the standalone host
  prototype and the actual patched melonDS host — real handshake, real
  device-approval flow, real sustained video/input traffic. Not yet
  tested on real Steam Deck hardware — see
  [`docs/building.md`](docs/building.md) and
  [`docs/known-limitations.md`](docs/known-limitations.md).

## Quick start

```sh
# Build and test everything that doesn't need SDL3 or melonDS:
./scripts/install-dev.sh

# Run the standalone host prototype (device-approval mode by default):
./scripts/run-host.sh --state-dir ~/.config/melonds-remote

# In another terminal, exercise it end-to-end without needing the client built
# (uses --auth-token, since it doesn't simulate a human approving a device):
python3 tests/smoke_test.py build/host/remote-server/melonds-remote-server

# If you have SDL3 installed, build and run the client -- first connection to
# a host will show "WAITING FOR APPROVAL ON HOST..." until you approve it
# there (type 'approve <id>' at the host's console, shown in its log):
./scripts/install-dev.sh --with-client
./scripts/run-client.sh 127.0.0.1
```

## Documentation

- [`docs/melonds-integration-analysis.md`](docs/melonds-integration-analysis.md) — Phase 0 findings
- [`host/melonds-patches/README.md`](host/melonds-patches/README.md) — the melonDS patch itself and what's verified
- [`tests/homebrew-test-rom/README.md`](tests/homebrew-test-rom/README.md) — the original homebrew ROM used to verify the patch's video path
- [`docs/architecture.md`](docs/architecture.md) — component overview and threading model
- [`docs/protocol.md`](docs/protocol.md) — wire format reference
- [`docs/building.md`](docs/building.md) — build instructions
- [`docs/testing.md`](docs/testing.md) — unit tests and the integration smoke test
- [`docs/bazzite-host-setup.md`](docs/bazzite-host-setup.md) — Bazzite-specific host build/run notes (Distrobox, firewalld)
- [`docs/steam-deck-setup.md`](docs/steam-deck-setup.md) — Steam Deck client setup (Desktop Mode + Gaming Mode shortcut)
- [`docs/troubleshooting.md`](docs/troubleshooting.md) — fixes for problems you're likely to hit
- [`docs/known-limitations.md`](docs/known-limitations.md) — consolidated list of what isn't done yet

## License

GPLv3 (see [`LICENSE`](LICENSE)), matching melonDS's own license, since this
project is designed to become a melonDS fork/patch. See
`docs/melonds-integration-analysis.md` section 0 for details.
