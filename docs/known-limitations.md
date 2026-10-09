# Known limitations

What doesn't work yet, or hasn't been confirmed on real hardware. This
is the list `SPEC.md` section 25 asks for. Each item links to the entry
in [`history.md`](history.md) with the full story; update this page when
an item is fixed or a new gap is found.

## Emulators

- **Nintendo DS (melonDS)** is the most complete path. melonDS now runs
  out-of-process through the shared host service by default; that
  launcher was tested with stub binaries but not yet against a real
  melonDS session (history: "PyroWave verified on real hardware, plus
  three gaps it exposed", Gap 2). `DUALDECK_MELONDS_IN_PROCESS=1` forces
  the older in-process server, which only speaks JPEG.
- **Nintendo 3DS (Azahar)** is experimental. It has not been tested
  against a real 3DS game end to end (history: "AzaharAdapter: a second
  emulator"), and the "Load File" crash fix was never confirmed on the
  affected machine (history: "Azahar crashing when browsing files").
- **Nintendo Wii U (Cemu)** is experimental. Wii U system overlays (the
  software keyboard, error popups) are not captured unless Cemu's own
  GamePad View window is open (history: "Video frames are now
  JPEG-compressed", the 2D-menu-freeze note). GamePad touch input is
  implemented but not confirmed in a real touch title.
- Only pause/resume, fast-forward and screen swap are wired up as
  emulator actions; save and load state are not.
- `emudeck-replace-in-place.sh --refresh-installed`, which re-patches
  emulators after a host update, has only run against a fake
  `~/Applications`, not a real EmuDeck or RetroDECK install.

## Video

- JPEG is the default codec. H.264 (OpenH264) and PyroWave are opt-in
  from the client's Settings screen; PyroWave needs a Vulkan GPU on both
  machines and has been verified on one Fedora host with a Radeon GPU.
- CI runs PyroWave's decoder on software Vulkan (lavapipe) but skips its
  encoder tests, which need subgroup sizes lavapipe lacks. Run those on
  a real GPU when touching the encoder.
- The latency figure in the debug overlay assumes the client and host
  clocks are in sync (NTP). With skewed clocks it reads `0US` or is
  dropped from the average (history: "LATENCY reads exactly 0US").

## Connecting and pairing

- One client at a time, IPv4 only, by design (`SPEC.md` sections 7.1
  and 21). Discovery is a small custom UDP broadcast, not mDNS.
- Device approval is the only pairing method (plus `--auth-token` for
  scripts). There are no QR codes or certificates, no session-ID check
  after the handshake, and no way to revoke a single approved device;
  deleting the approved-devices file forgets all of them.
- The host's approval prompt is a `kdialog` popup on the host's desktop.
  It is unlikely to show over a running game in Steam Gaming Mode
  (inferred, not tested). The host service's local control socket
  (`dualdeck-host-service --help`) can list and approve requests
  instead, and the Decky plugin does so when Decky is installed on the
  host PC. Without Decky on the host, the popup is still the only prompt.

## Host Control mode

- Experimental. Its virtual gamepad needs `/dev/uinput` access on the
  host.
- The screen mirror works on X11 and on Wayland through the desktop
  portal (which asks for permission the first time). Large desktops are
  downscaled to the client's display before encoding (history:
  "streaming a 4K desktop to a Steam Deck-sized display is sluggish").
- The touchpad only reaches the host cursor when Steam Input is
  disabled for the DualDeck Client shortcut, or while holding the STEAM
  button (see [`troubleshooting.md`](troubleshooting.md)).

## Installing and updating

- Adding or removing the Steam shortcut edits `shortcuts.vdf`, which
  needs Steam closed; the scripts restart Steam for you when needed.
- The Installation branch selector can only install a branch that has
  had a release published from its current tip.
- There is no shipped Steam Input binding for the client; the
  workaround for the Deck's face-button layout is to disable Steam
  Input for the shortcut (history: "Azahar X and Y buttons are
  flipped").

## Decky plugin

- [`decky-plugin/`](../decky-plugin/README.md) has never been loaded in
  a real Decky Loader. Its backend is tested against the real host
  service, with a stand-in for Decky's `decky` module.
- On the Deck, it only toggles melonDS's in-process management listener,
  which is now the fallback path, so it does nothing for Azahar, Cemu or
  out-of-process melonDS sessions.
- Its host-side panel (approve devices, Host Control switch) needs Decky
  on the host PC itself. It finds the host service's socket through
  `$XDG_RUNTIME_DIR`, `/run/user/<uid>` or `~/.cache`. If Decky's backend
  runs as a different user than the service, the socket's 0600 mode
  keeps it out (root aside).

## Testing gaps

- CI builds the SDL3 client on every push (headless SDL3) and renders
  its picker and menu screens to images (the `ui-preview` artifact), but
  nothing drives the screens, wizard or stream loop with real input.
- No real Steam Deck or Bazzite hardware runs in CI. Most fixes in
  `history.md` marked "not yet verified" are waiting on a real-hardware
  report.

## Out of scope

Per `SPEC.md` section 21: ROM transfer, cloud saves, internet play,
multiple simultaneous clients, user accounts, Android/Windows/iOS
clients, camera emulation, rumble, voice chat, streaming game audio back
to the client, spectator mode, artwork scraping, cheat databases, a
custom emulator core, and replacing the emulators' own rendering.
