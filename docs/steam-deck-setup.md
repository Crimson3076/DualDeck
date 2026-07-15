# Steam Deck Setup

This is the required "Steam Deck setup instructions" deliverable
(`SPEC.md` section 25). **Status: written from the SDL3/SteamOS API and
Steam documentation, not yet verified on real Steam Deck hardware** --
see `docs/known-limitations.md`. Treat this as a starting point and
correct it once someone runs it on a real Deck.

## Prerequisite: build the client

You need `melonds-remote-client` built with SDL3 for the Deck's
architecture. Two practical options:

1. **Build directly on the Deck in Desktop Mode.** SteamOS Desktop Mode
   is a normal (if partially read-only) Arch-based Linux; you can install
   a C++ toolchain and CMake via `pacman` (you may need to disable the
   read-only filesystem overlay, or use a Flatpak/container-based
   toolchain, depending on your SteamOS version -- consult current
   SteamOS documentation, since this changes across releases).
2. **Cross-build elsewhere and copy the binary over**, if you'd rather
   not install a toolchain on the Deck itself. Match the Deck's SDL3
   version/architecture (x86_64).

See `docs/building.md` for the actual CMake invocation either way.

## Desktop Mode: quick manual test

1. Switch the Deck to Desktop Mode (Steam button → Power → Switch to
   Desktop).
2. Open a terminal (Konsole).
3. Run the client directly:
   ```sh
   ./melonds-remote-client --host <htpc-ip-address> --auth-token <your-token>
   ```
4. Confirm: window opens fullscreen at 1280x800, the test pattern (or a
   real host's bottom-screen stream, once melonDS integration exists)
   renders letterboxed at 4:3, the Deck's built-in controller moves the
   test overlay/logs show button state, and the touchscreen registers
   only inside the rendered DS rectangle (spec section 7.4).
5. Exit with the window's quit control (currently `SDL_EVENT_QUIT`, i.e.
   closing the window -- a controller-navigable in-app exit action is
   listed as Phase 3 work in `SPEC.md` and not yet implemented; see
   `docs/known-limitations.md`).

## Gaming Mode: adding as a non-Steam shortcut

This is the standard way to run any non-Steam Linux binary in Gaming
Mode, applied to this client:

1. In Desktop Mode, open Steam.
2. **Games → Add a Non-Steam Game to My Shortcuts...**
3. **Browse...** to the built `melonds-remote-client` binary.
4. After adding, right-click it in your library → **Properties**:
   - **Launch Options**: add your host address/token, e.g.
     `--host 192.168.1.50 --auth-token your-token`
   - **Compatibility**: a Proton layer is not needed since this is a
     native Linux binary; leave "Force the use of a specific Steam Play
     compatibility tool" unchecked.
5. Switch to Gaming Mode; the shortcut appears in your library like any
   other game and can be launched fully controller-driven.

## Controller mapping expectations

Once launched, the physical Deck controls should behave per `SPEC.md`
section 7.3's table (D-pad → DS D-pad, A/B/X/Y → DS A/B/X/Y, L1/R1 → DS
L/R, Start/View → DS Start/Select, touchscreen → DS touchscreen, left
stick as an optional alternate D-pad). These are currently a fixed
mapping in `client/src/main.cpp` (`kButtonMappings`), not yet exposed
through Steam Input action sets -- that remapping-via-Steam-Input work is
Phase 3 (`SPEC.md`) and not implemented yet.

## If something doesn't work

See `docs/troubleshooting.md`. If you're the first person actually
running this on hardware, please fold your corrections back into this
file -- it was written without access to a physical Steam Deck.
