# Steam Deck Setup

This is the required "Steam Deck setup instructions" deliverable
(`SPEC.md` section 25). **Status: now tested on real Steam Deck
hardware once** -- controls and touch input worked, but that first pass
also surfaced two real bugs (the menu opening on a single button instead
of a deliberate hold, and no video reaching the client because the host
was using melonDS's OpenGL 3D renderer) which are now fixed; see
`docs/known-limitations.md` and `docs/troubleshooting.md` for the
specifics, and correct this file further as more real-hardware runs turn
up issues.

The host's 3D renderer (**Software**, **OpenGL**, or **OpenGLCompute** --
**Config > Emu Settings > Video Settings > Renderer**) no longer matters
for remote video to work; pick whichever gives you the best local
gameplay performance. If video still doesn't show up, see
`docs/troubleshooting.md`'s "screen stays blank" entry.

## Quickest path: prebuilt release, no terminal needed

If you'd rather not build anything, download the latest release archive
(see the top-level `README.md`'s "Download a build") and extract it in
Desktop Mode's file manager (Dolphin). Both scripts in `client/` are
directly double-clickable -- Dolphin offers to run an executable `.sh`
file when you double-click it, no terminal or typing needed:

- **`run-client.sh`** -- runs the client directly, for a quick test.
- **`install-steam-shortcut.sh`** -- one-time setup that registers it as
  a Steam library/Gaming Mode shortcut, applied to every local Steam
  user automatically (**close Steam first**, since it edits Steam's
  library file directly and Steam can silently undo the change if it's
  still running).

Everything below this section is the from-source/terminal path -- useful
if you're modifying the code, but not required for normal use.

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
3. Run the client. If you know your host's LAN address already, you can
   still pass it directly:
   ```sh
   ./melonds-remote-client --host <htpc-ip-address>
   ```
   but as of the LAN discovery feature, this is optional -- run it with no
   arguments and it scans the LAN for running `melonds-remote-server`
   hosts instead:
   ```sh
   ./melonds-remote-client
   ```
   You'll see a "SEARCHING FOR HOST..." screen while it scans, then a
   **SELECT A HOST** list -- shown every time the client launches, even
   if only one host answers, so switching to a different HTPC (e.g. one
   in the living room, one in a bedroom) is always available, not just
   remembered from last time. Use the D-pad (or arrow keys in Desktop
   Mode) to move and South/A (or Enter) to confirm; the previously-picked
   host is pre-highlighted, so reconnecting to the same one as usual is
   still just one button press. The list keeps rescanning live while
   shown, so a host that finishes booting a few seconds late still shows
   up. Discovery only works if the host has it enabled (the default --
   see `--no-discovery`/`--discovery-port` in the host's `--help`) and
   both machines are on the same LAN/subnet (it relies on UDP broadcast,
   which routers don't forward across subnets or over the internet).
4. **First connection to a given host**: the host has no idea who this
   client is yet, so it rejects the handshake and queues a pending
   connection request -- naming the client and its address -- for a human
   at the host to approve. No typing is needed anywhere (this
   deliberately doesn't rely on Steam Input's virtual keyboard, which
   doesn't reliably come up in Gaming Mode -- see
   `docs/known-limitations.md`): on the standalone host, type
   `approve <device-id-prefix>` at its console (the pending-request log
   line shows the exact command to use); on the melonDS-integrated host,
   a window pops up asking "Allow ... to connect?" with Approve/Deny
   buttons. While waiting, the client shows "WAITING FOR APPROVAL ON
   HOST ..." on screen and keeps retrying automatically -- no action
   needed on the client side. Once approved, this same client reconnects
   silently forever, no re-approval needed, unless the host's
   approved-device state is deleted. (If you'd rather skip device
   approval entirely, e.g. for scripted testing, start the host with
   `--auth-token`/`MELONDS_REMOTE_AUTH_TOKEN` and pass the same value
   here with `--auth-token`.)
5. Confirm: window opens fullscreen at 1280x800, the host's bottom-screen
   stream renders letterboxed at 4:3, the Deck's built-in controller
   moves the game/logs show button state, and the touchscreen registers
   only inside the rendered DS rectangle (spec section 7.4).
6. Hold **Start+Select together for about a third of a second** (or press
   Escape in Desktop Mode with no gamepad connected) to open the in-app
   menu -- fully controller-navigable (D-pad to move, South/A to select),
   with **Resume**, **Change Host** (jump back to the host-selection
   screen from step 3 without restarting the client), and **Exit**. This
   is the controller-navigable in-app exit action `SPEC.md` lists as
   Phase 3 work -- now implemented rather than only closable via the
   window's `SDL_EVENT_QUIT` (still works too, e.g. Alt+F4 in Desktop
   Mode). The same chord also works on the discovery/searching and
   host-selection screens from step 3, before you've connected to
   anything -- offering just **Resume**/**Exit** there -- so there's
   always a controller-only way out even if the client was opened by
   accident or no host is running yet (GitHub issues #8, #9). A reminder
   of this combo is shown on screen on the discovery/host-selection and
   connecting screens, so you don't need to remember it from this doc.
   It deliberately requires a genuine, deliberate hold
   (not just both buttons registering in the same instant) -- real
   hardware testing found Steam Input can synthesize a single-button
   keyboard shortcut that would otherwise open the menu unintentionally;
   see the Gaming Mode section below and `docs/troubleshooting.md` if a
   single button still opens it for you.

## Gaming Mode: adding as a non-Steam shortcut

**Quickest path**: with Steam closed, double-click
`client/install-steam-shortcut.sh` (prebuilt release archive -- Dolphin
offers to run it, no terminal needed) or run
`./scripts/install-steam-shortcut.sh` (from source; add
`--host 192.168.1.50` if you'd rather skip discovery and always connect
to one address, matching step 4's Launch Options note below) -- it
builds the client if needed and adds it directly to your
Steam library, no manual "Add a Non-Steam Game" clicking, and no typing
required. It applies to every local Steam user automatically (no prompt
to pick one -- there'd be no way to answer that with just a controller).
See `scripts/lib/steam_shortcut.py`'s module docstring for exactly what
it does and doesn't touch (it backs up `shortcuts.vdf` first and refuses
to run while Steam is open, since Steam caches that file in memory and
can overwrite the change on its next save). You still need to set its
**Controller Layout** by hand afterward -- that's a separate per-shortcut
Steam Input config this script doesn't touch, see step 4 below for why
it matters.

It copies the client binary (and, for the prebuilt-archive path, its
bundled SDL3 library and launcher wrapper) into a fixed central
directory, `~/.config/melonds-remote-client/install/`, and points the
Steam shortcut's `Exe` at that copy rather than at wherever you happen
to have extracted or built it -- so the extracted release folder (or a
`build/` directory) can be deleted right after this runs, and installing
again later from a newer release still updates the same shortcut instead
of leaving a stale duplicate behind. This also fixes an earlier bug
where a shortcut pointed straight at the raw client binary instead of
the wrapper that sets up its bundled SDL3 library, which could leave the
shortcut launching to nothing on a real Deck. A failed update can't lose
a working install either -- the new files are staged separately and only
swapped in once that succeeds, keeping the replaced version as a
one-generation backup. If either script ever fails, check
`~/.config/melonds-remote-client/install.log` (or the graphical error
box it pops up on KDE Plasma, via `kdialog`) -- see
`docs/troubleshooting.md` if Steam's shortcut list itself looks wrong
afterward.

**Removing it**: with Steam closed, double-click
`client/uninstall-steam-shortcut.sh` (or run
`./scripts/uninstall-steam-shortcut.sh` from source) -- same no-typing,
applies-to-every-local-user behavior as the install script, and is safe
to run again even if the shortcut is already gone (it just does
nothing). Also deletes the central install directory above, so nothing
is left behind. Works even if the original download/build is long gone,
since it only ever depends on that central directory, not on wherever it
was invoked from.

Or, the standard manual way any non-Steam Linux binary gets added to
Gaming Mode:

1. In Desktop Mode, open Steam.
2. **Games → Add a Non-Steam Game to My Shortcuts...**
3. **Browse...** to the built `melonds-remote-client` binary.
4. After adding, right-click it in your library → **Properties**:
   - **Launch Options**: leave blank to let the client discover hosts on
     the LAN automatically and show the selection screen every launch
     (see "Desktop Mode" step 3 above -- fully controller-navigable,
     D-pad + South/A, matching Gaming Mode's controller-only input
     model). Add `--host 192.168.1.50` here only if you'd rather skip
     discovery and always connect to one specific address (add
     `--auth-token your-token` too only if the host was started with a
     static token instead of device-approval mode). The first launch
     needs a human at the host to approve the connection request (see
     "Desktop Mode" step 4 above) -- no on-screen keyboard needed on the
     client, unlike the pairing-code flow this replaced; every launch
     after that reconnects silently once approved.
   - **Compatibility**: a Proton layer is not needed since this is a
     native Linux binary; leave "Force the use of a specific Steam Play
     compatibility tool" unchecked.
   - **Controller Layout**: real hardware testing found Steam's
     auto-picked default template for a freshly-added non-Steam shortcut
     can synthesize keyboard shortcuts (e.g. Escape) for individual
     buttons on top of their normal gamepad signal, which the client
     can't reliably distinguish from you actually pressing that key. Open
     **Controller Layout** for this shortcut (from its right-click menu,
     or from the Quick Access Menu once it's running) and pick a plain
     **Gamepad** template rather than whatever Steam guessed, so buttons
     pass through as raw gamepad input only. The client is defensive
     against this either way (see step 6 above and
     `docs/troubleshooting.md`), but a clean pass-through layout is the
     more correct fix.
5. Switch to Gaming Mode; the shortcut appears in your library like any
   other game and can be launched fully controller-driven.

## Controller mapping expectations

Once launched, the physical Deck controls should behave per `SPEC.md`
section 7.3's table (D-pad → DS D-pad, A/B/X/Y → DS A/B/X/Y, L1/R1 → DS
L/R, Start/View → DS Start/Select, touchscreen → DS touchscreen, left
stick as an optional alternate D-pad). Start and View are dual-purpose:
pressed individually they still go to the DS as Start/Select; **held
together** they instead open the in-app menu (see step 6 above) and stop
being forwarded to the DS for as long as the menu is open. These are
currently a fixed mapping in `client/src/main.cpp` (`kButtonMappings`),
not yet exposed through Steam Input action sets -- that remapping-via-Steam-Input work is
Phase 3 (`SPEC.md`) and not implemented yet.

## If something doesn't work

See `docs/troubleshooting.md`. If you're the first person actually
running this on hardware, please fold your corrections back into this
file -- it was written without access to a physical Steam Deck.
