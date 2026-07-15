# melonDS Remote

A Linux-focused system for running Nintendo DS games through melonDS on an
HTPC while using a Steam Deck as the handheld controller and bottom
screen — like a Wii U GamePad, with the TV showing the DS top screen and
the Steam Deck showing the bottom screen plus all controls.

See [`SPEC.md`](SPEC.md) for the full project scope and requirements.

## Status

**Phase 0 (repository investigation) and a minimal Phase 1 skeleton are
implemented.** There is no melonDS integration yet.

- [`docs/melonds-integration-analysis.md`](docs/melonds-integration-analysis.md) —
  where melonDS exposes bottom-screen frames and accepts input, and the
  proposed patch boundary for the future melonDS fork integration.
- [`protocol/`](protocol/) — versioned wire format, touch-coordinate
  mapping, and fail-safe input-state tracking. Fully unit tested, no
  external dependencies.
- [`host/remote-server/`](host/remote-server/) — a standalone host binary
  implementing the full network/threading model (TCP control, UDP input,
  TCP video) against a synthetic test-pattern frame source and a logging
  input sink, so it can be built and tested without melonDS or a display.
- [`client/`](client/) — an SDL3 Steam Deck client. Written but **not
  build-verified** in the environment this was developed in (no SDL3
  package available there) — see [`docs/building.md`](docs/building.md).
- [`host/melonds-patches/`](host/melonds-patches/) — empty. The actual
  melonDS fork integration is the next milestone, gated on review of the
  patch boundary proposed in the analysis doc above.

## Quick start

```sh
# Build and test everything that doesn't need SDL3 or melonDS:
./scripts/install-dev.sh

# Run the standalone host prototype:
./scripts/run-host.sh

# In another terminal, exercise it end-to-end without needing the client built:
python3 tests/smoke_test.py build/host/remote-server/melonds-remote-server

# If you have SDL3 installed, build and run the client:
./scripts/install-dev.sh --with-client
./scripts/run-client.sh 127.0.0.1
```

## Documentation

- [`docs/melonds-integration-analysis.md`](docs/melonds-integration-analysis.md) — Phase 0 findings
- [`docs/architecture.md`](docs/architecture.md) — component overview and threading model
- [`docs/protocol.md`](docs/protocol.md) — wire format reference
- [`docs/building.md`](docs/building.md) — build instructions
- [`docs/testing.md`](docs/testing.md) — unit tests and the integration smoke test

## License

GPLv3 (see [`LICENSE`](LICENSE)), matching melonDS's own license, since this
project is designed to become a melonDS fork/patch. See
`docs/melonds-integration-analysis.md` section 0 for details.
