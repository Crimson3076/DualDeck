# DualDeck

A Linux-focused system for running Nintendo DS games through melonDS on an
HTPC while using a Steam Deck as the handheld controller and bottom
screen — like a Wii U GamePad, with the TV showing the DS top screen and
the Steam Deck showing the bottom screen plus all controls. Experimental
support also exists for the Nintendo 3DS (Azahar) and Wii U (Cemu).

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

The easiest way to install -- paste this into a terminal on the machine
you're installing to (no download, no `chmod +x`, nothing to navigate
with a controller):

```sh
curl -fsSL https://github.com/Crimson3076/DualDeck/releases/latest/download/DualDeck-Installer.sh | bash
```

That fetches and runs the same `DualDeck-Installer.sh` published on the
[**Releases**](../../releases) page (GitHub's `releases/latest/download/`
URL always resolves to the newest release's copy of a named asset) --
it downloads and verifies (via `SHA256SUMS`) the actual install archive
for you and offers Install Client / Install Host / Install Both /
Repair / Uninstall through a graphical `kdialog`/`zenity` menu on
SteamOS/Bazzite/KDE desktops, or a plain terminal prompt otherwise.
Skip the menu entirely by appending `-s -- --host` or `-s -- --client`
(also `--both`/`--repair`/`--uninstall`) to the command above -- useful
if a controller-driven menu is awkward to navigate, or for scripting.
If you'd rather review the script before running it, download
**`DualDeck-Installer.sh`** from the Releases page and run it locally
instead -- functionally identical. See `docs/known-limitations.md`'s
installer section for exactly what it does and doesn't cover yet.

Everything you need to run is one script per side, no typing required:
double-click `host/dualdeck-host.sh` on the machine running the
emulator, and `client/dualdeck-client.sh` on your Steam Deck (or
any Linux machine with a gamepad). Each opens a small menu; the host's
"Launch..." choice picks which system to run -- Nintendo DS (melonDS,
the default/most-supported path), Nintendo 3DS (Azahar, experimental --
see `docs/known-limitations.md`'s AzaharAdapter entry), Nintendo Wii U
(Cemu, experimental -- see `host/cemu-patches/README.md` for what's
verified), host-control mode only (no emulator, browse the host's own
UI from the client instead -- also experimental), or a custom emulator
you've already patched yourself with `scripts/patch-existing-emulator.sh`,
for anyone who doesn't want a separate DualDeck-managed copy. The 3DS,
Wii U, and host-control-only choices all get the same zero-typing
device-approval prompt as melonDS's own in-process dialog -- a `kdialog`
Yes/No popup on the host's own desktop the first time an unrecognized
device connects, no shared secret to type or copy anywhere. Add to Steam
(Big Picture/Gaming Mode), Remove from Steam, and Check for updates
(which offers to install one automatically if it finds one) round out
the rest of the menu; an "Advanced..." entry on both host and client
menus additionally offers an Installation branch selector -- pick a
branch (paginated, live from GitHub, cached with a Refresh), then a
separate "Install selected branch" resolves it to the exact commit the
newest release for that branch was built from and installs from that
verified build on both sides, so host and client always end up on the
same commit; see `docs/known-limitations.md`'s entry on this for the one
real constraint (a branch is only installable once someone has actually
published a release from its current tip). So there's nothing else in
either `host/` or
`client/` you need to open directly; the rest of what's in there (under
each directory's `internal/` subfolder) is what the menu calls on your
behalf. The client additionally checks for and installs updates
automatically on every launch (Steam shortcut included), no menu or
confirmation needed. On SteamOS Desktop Mode or Bazzite (both KDE
Plasma/Dolphin), double-clicking an executable `.sh` file offers to run
it directly -- see `docs/steam-deck-setup.md`/`docs/bazzite-host-setup.md`
for the quick-start section at the top of each, and the archive's own
bundled `README.md` for the full rundown.

## Features

- Stream the DS bottom screen (or, experimentally, the 3DS/Wii U
  GamePad screen) to a Steam Deck or any Linux x86_64 machine with a
  gamepad, while the TV shows the top screen -- full D-pad, face
  buttons, shoulders, Start/Select, and direct touchscreen input,
  injected straight into the emulator's own input system, not a
  keyboard/mouse/virtual-controller stand-in.
- **Device-approval authentication** -- the first time a client
  connects, a human approves it once (a popup on the host, or a
  name/address prompt in its console), nothing typed on the client
  side. Approval persists across restarts. A static pre-shared token is
  also supported for scripting/CI use.
- **Automatic LAN discovery** -- the client scans and shows a
  pick-a-host list instead of requiring a typed IP address.
- **Automatic reconnect** with capped exponential backoff, and
  fail-safe input release (every button, stick, and touch state
  cleared) the moment a client disconnects or times out.
- A checkbox in melonDS's own Emu Settings to turn remote streaming on
  or off, plus a lightweight management listener for toggling it live
  (used by the bundled Decky Loader plugin's Quick Access Menu panel)
  without restarting the emulator.
- Video capture works with both the Software and OpenGL/OpenGLCompute
  3D renderers.

## Planned features & fixes

- Real-game verification for the Nintendo 3DS (Azahar) path -- currently
  build-verified only, not yet run against an actual 3DS game.
- Real-hardware confirmation that GamePad touchscreen input for the
  Nintendo Wii U (Cemu) path actually registers in-game -- implemented
  and patch-verified (`host/cemu-patches/README.md`'s "GamePad
  touchscreen input implemented" entry), not yet confirmed against a
  real touch-sensitive Wii U title.
- Save state, load state, and the rest of the emulator-action set --
  only pause/resume, fast-forward, and screen-swap are wired up so far.
- A settings UI for bind address, ports, auth token, state directory,
  and discovery options -- currently config-file/env-var only.
- A way to list and revoke individual approved devices, instead of
  only clearing the whole approved-devices file at once.
- Session-ID validation on packets after the initial handshake.
- Independent verification of the OpenGLCompute renderer capture path
  (same code as OpenGL, but not yet exercised in a compute-capable
  environment).
- Testing on real Steam Deck hardware for the client -- verified so far
  against a standalone host and patched melonDS, not on-device.

See `docs/known-limitations.md` for the complete, per-platform list.

## First connection

The first time a client connects to a host, a human needs to approve it
at the host (shown as a popup if you're at the emulator's window, or in
its terminal output otherwise) -- nothing to type on the client side.
After that, it's remembered automatically.

## Quick start (running from source)

```sh
# Build and test everything that doesn't need SDL3 or melonDS:
./scripts/install-dev.sh

# Run the standalone host prototype (device-approval mode by default):
./scripts/run-host.sh --state-dir ~/.config/dualdeck

# In another terminal, exercise it end-to-end without needing the client built
# (uses --auth-token, since it doesn't simulate a human approving a device):
python3 tests/smoke_test.py build/host/remote-server/dualdeck-host-service

# If you have SDL3 installed, build and run the client -- first connection to
# a host will show "WAITING FOR APPROVAL ON HOST..." until you approve it
# there (type 'approve <id>' at the host's console, shown in its log):
./scripts/install-dev.sh --with-client
./scripts/run-client.sh 127.0.0.1
```

## Documentation

- [`docs/melonds-integration-analysis.md`](docs/melonds-integration-analysis.md) -- how melonDS exposes bottom-screen frames and accepts input
- [`docs/azahar-integration-analysis.md`](docs/azahar-integration-analysis.md) -- the same investigation for Azahar/3DS
- [`host/melonds-patches/README.md`](host/melonds-patches/README.md) -- the melonDS patch and what's verified
- [`host/azahar-patches/README.md`](host/azahar-patches/README.md) -- the Azahar/3DS patch and what's verified
- [`host/cemu-patches/README.md`](host/cemu-patches/README.md) -- the Cemu/Wii U patch and what's verified
- [`decky-plugin/README.md`](decky-plugin/README.md) -- the Decky Loader plugin for live start/stop from Gaming Mode
- [`tests/homebrew-test-rom/README.md`](tests/homebrew-test-rom/README.md) -- the original homebrew ROM used to verify the DS video/input path
- [`docs/architecture.md`](docs/architecture.md) -- component overview and threading model
- [`docs/protocol.md`](docs/protocol.md) -- wire format reference
- [`docs/building.md`](docs/building.md) -- build instructions
- [`docs/testing.md`](docs/testing.md) -- unit tests and the integration smoke test
- [`docs/bazzite-host-setup.md`](docs/bazzite-host-setup.md) -- Bazzite-specific host build/run notes (Distrobox, firewalld)
- [`docs/steam-deck-setup.md`](docs/steam-deck-setup.md) -- Steam Deck client setup (Desktop Mode + Gaming Mode shortcut)
- [`docs/troubleshooting.md`](docs/troubleshooting.md) -- fixes for problems you're likely to hit
- [`docs/known-limitations.md`](docs/known-limitations.md) -- consolidated list of what isn't done yet

## License

GPLv3 (see [`LICENSE`](LICENSE)), matching melonDS's own license, since this
project is designed to become a melonDS fork/patch. See
`docs/melonds-integration-analysis.md` section 0 for details.
