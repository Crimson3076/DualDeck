# DualDeck

A Linux-focused system for running Nintendo DS games through melonDS on an
HTPC while using a Steam Deck as the handheld controller and bottom
screen — like a Wii U GamePad, with the TV showing the DS top screen and
the Steam Deck showing the bottom screen plus all controls. Experimental
support also exists for the Nintendo 3DS (Azahar) and Wii U (Cemu).

See [`SPEC.md`](SPEC.md) for the full project scope and requirements.

## Install

On each machine (the HTPC host and the Steam Deck or other Linux client),
paste this into a terminal:

```sh
curl -fsSL https://github.com/Crimson3076/DualDeck/releases/latest/download/DualDeck-Installer.sh | bash
```

It downloads the newest [release](../../releases), checks it against
`SHA256SUMS`, and asks whether to install the client, the host, or both
(also Repair and Uninstall). It uses a `kdialog`/`zenity` menu on
SteamOS, Bazzite and KDE, or a terminal prompt elsewhere. To skip the
menu, add `-s -- --host` or `-s -- --client` (also `--both`, `--repair`,
`--uninstall`) after `bash`. If you'd rather read the script first,
download `DualDeck-Installer.sh` from the Releases page and run it
yourself.

Step-by-step setup guides:
[Steam Deck client](docs/steam-deck-setup.md) and
[Bazzite host](docs/bazzite-host-setup.md).

## Use it

Each side has one menu script:

- **Host:** `host/dualdeck-host.sh`. Pick **Launch...** and a system:
  Nintendo DS (melonDS, the main path), Nintendo 3DS (Azahar,
  experimental), Wii U (Cemu, experimental), Host Control only (drive the
  host's own desktop from the Deck, experimental), or an emulator you
  patched yourself with `scripts/patch-existing-emulator.sh`.
- **Client:** `client/dualdeck-client.sh`. It finds hosts on your
  network and lists them; pick one.

Both menus also offer **Add to Steam** (so you can launch from Gaming
Mode), **Remove from Steam**, **Check for updates**, and under
**Advanced...** an installation branch selector that installs the same
commit on host and client. The client also updates itself on every
launch. Everything under each `internal/` folder is called by the menus;
you shouldn't need to open it.

The first time a client connects, the host shows an Approve/Deny popup
(or a prompt in its terminal). Nothing is typed on the Deck, and the
approval is remembered.

## What works

- Stream the DS bottom screen (or the 3DS bottom screen or Wii U
  GamePad screen) to the Deck while the TV shows the top screen, with
  buttons, sticks and touch injected straight into the emulator.
- JPEG video by default, with opt-in H.264 and PyroWave (low-latency,
  needs a Vulkan GPU on both machines) from the client's Settings.
- LAN discovery, automatic reconnect, and every input released the
  moment a client drops.
- DS microphone input from the Deck.

[`docs/known-limitations.md`](docs/known-limitations.md) lists what
doesn't work yet or hasn't been confirmed on real hardware.

## Build from source

```sh
# Build and test everything that doesn't need SDL3 or an emulator:
./scripts/install-dev.sh

# Run the standalone host (device-approval mode by default):
./scripts/run-host.sh --state-dir ~/.config/dualdeck

# Drive it end to end without the client (uses --auth-token):
python3 tests/smoke_test.py build/host/remote-server/dualdeck-host-service

# With SDL3 installed, build and run the client. Approve it at the
# host's console with 'approve <id>':
./scripts/install-dev.sh --with-client
./scripts/run-client.sh 127.0.0.1
```

See [`docs/building.md`](docs/building.md) for dependencies and
[`docs/testing.md`](docs/testing.md) for the test suites.

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
- [`docs/known-limitations.md`](docs/known-limitations.md) -- what doesn't work yet
- [`docs/history.md`](docs/history.md) -- dated development log, the background behind most code comments

## License

GPLv3 (see [`LICENSE`](LICENSE)), matching melonDS's own license, since this
project is designed to become a melonDS fork/patch. See
`docs/melonds-integration-analysis.md` section 0 for details.

The host window (`host/ui/`) compiles in the Chakra Petch and Atkinson
Hyperlegible fonts, both under the SIL Open Font License 1.1 (see
`host/ui/fonts/`), and uses stb_truetype (public domain).
