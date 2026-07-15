# melonDS Remote

A Linux-focused system for running Nintendo DS games through melonDS on an
HTPC while using a Steam Deck as the handheld controller and bottom
screen — like a Wii U GamePad, with the TV showing the DS top screen and
the Steam Deck showing the bottom screen plus all controls.

See [`SPEC.md`](SPEC.md) for the full project scope and requirements.

## Status

**Phase 0, a Phase 1 skeleton, Phase 2 network-robustness work, and a
first melonDS integration patch are all implemented.** The patch builds,
its control-channel handshake/authentication is verified against a real
melonDS binary, and its video-capture path has been confirmed to deliver
real, non-static frames from an actual running (homebrew) ROM -- see
below for exactly what is and isn't verified yet.

- [`docs/melonds-integration-analysis.md`](docs/melonds-integration-analysis.md) —
  where melonDS exposes bottom-screen frames and accepts input, verified
  by building and patching real melonDS, not just reading source.
- [`protocol/`](protocol/) — versioned wire format (including the Hello/
  HelloAck handshake payloads), touch-coordinate mapping, fail-safe
  input-state tracking, and connection-attempt rate limiting. Fully unit
  tested, no external dependencies.
- [`host/remote-server/`](host/remote-server/) — a standalone host binary
  implementing the full network/threading model (TCP control, UDP input,
  TCP video) against a synthetic test-pattern frame source and a logging
  input sink, so it can be built and tested without melonDS or a display.
  Supports an optional pre-shared auth token (`--auth-token`); UDP input
  and the video channel are both gated on a completed, authenticated
  handshake from the same source address.
- [`host/melonds-patches/`](host/melonds-patches/) — a real patch against
  upstream melonDS (`0001-remote-server-integration.patch`) that vendors
  the protocol/host code above into melonDS's own build and wires it to
  `GPU::GetFramebuffers()` and the input/hotkey system. Confirmed to
  build from a fresh clone, its handshake/auth verified against the
  running patched binary, and its video path confirmed to deliver a real,
  non-static frame from a minimal original homebrew ROM
  ([`tests/homebrew-test-rom/`](tests/homebrew-test-rom/)) direct-booted
  in the patched binary. **Still open**: the exact RGBA/BGRA pixel order
  wasn't conclusively pinned down, and input injection into a game that
  reads input hasn't been exercised — see
  `host/melonds-patches/README.md` for the full verification account.
- [`client/`](client/) — an SDL3 Steam Deck client with automatic
  reconnect (capped exponential backoff). Written but **not
  build-verified** in the environment this was developed in (no SDL3
  package available there) — see [`docs/building.md`](docs/building.md).

## Quick start

```sh
# Build and test everything that doesn't need SDL3 or melonDS:
./scripts/install-dev.sh

# Run the standalone host prototype:
./scripts/run-host.sh --auth-token some-shared-secret

# In another terminal, exercise it end-to-end without needing the client built:
python3 tests/smoke_test.py build/host/remote-server/melonds-remote-server

# If you have SDL3 installed, build and run the client:
./scripts/install-dev.sh --with-client
./scripts/run-client.sh 127.0.0.1  # or: melonds-remote-client --host 127.0.0.1 --auth-token some-shared-secret
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
