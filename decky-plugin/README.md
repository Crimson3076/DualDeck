# DualDeck -- Decky Loader plugin

The panel has two sections:

- **This PC (host)** appears only when the machine running Decky is
  also running the DualDeck host service, for example a PC in Steam Big
  Picture with the Host Control daemon installed. It lists devices
  asking to connect, with **Approve** and **Deny** buttons, so you don't
  have to find the desktop `kdialog` popup. While an emulator is
  streaming, it also offers **Switch to Host Control** and
  **Return to the emulator**.
- **Remote melonDS** starts or stops the streaming server on a melonDS
  already running on another PC, from the Steam Deck's Quick Access
  Menu. See GitHub issue "Decky plugin to start/stop the host server"
  for the original request.

## This PC (host)

`host_control_client.py` talks to the host service's local control
socket (`host/remote-server/include/host/control_socket.h`). It looks
for the socket at `$XDG_RUNTIME_DIR/dualdeck/host-control.sock`, then
`/run/user/<uid>/...`, then `~/.cache/dualdeck/...`. The panel re-reads
it every two seconds while it's open. While it's closed, the plugin still
checks every three seconds and raises a Steam notification for each
new connection request, so you hear about it mid-game. There is nothing
to configure.

`tests/decky_host_control_test.py` runs `main.py`'s host-side calls
against the real `dualdeck-host-service` in CI, using a stand-in for
the `decky` module.

## Remote melonDS

### What this actually does

Toggles the same live on/off switch documented in
`docs/history.md`'s "Live-toggle: start/stop remote streaming
without restarting melonDS" section: melonDS (built from
`host/melonds-patches/0001-remote-server-integration.patch`) runs a
small always-on management listener whenever a management token is
configured (`Config > Emu settings > General`, or
`MelonDSRemote.ManagementToken` in `melonDS.toml`), independent of
whether remote streaming itself is currently on. This plugin is just a
thin client for that listener -- see `management_client.py`.

This does **not** start or stop melonDS itself -- melonDS has to already
be running on the host for there to be anything to toggle.

### Setup

1. On the host, set a management token (Emu Settings, or
   `MelonDSRemote.ManagementToken = "some-shared-secret"` in
   `melonDS.toml`'s `[MelonDSRemote]` section) and restart melonDS once
   for it to take effect.
2. Open this plugin's panel from the Quick Access Menu, enter the host's
   IP address, its management port (`8764` by default,
   `MelonDSRemote.ManagementPort` if you changed it), and the same
   token, then **Save**.
3. **Start streaming** / **Stop streaming** toggles it; **Refresh
   status** re-checks.

## What's verified and what isn't

**Verified**, against a real, patched, running melonDS in this
project's own sandbox:
- `management_client.py`'s wire protocol (the actual network logic) --
  exercised directly against `ManagementServer` for `STATUS`/`ENABLE`/
  `DISABLE`, including the bad-token rejection path.
- `main.py`'s backend logic (`get_settings`/`save_settings`/
  `get_status`/`set_enabled`) -- exercised end-to-end against the same
  real host, using a minimal stand-in for the `decky` module Decky
  Loader normally provides (this sandbox has no real Decky Loader
  runtime to test against).
- `main.py`'s host-side calls (`get_local_host`/`answer_device`/
  `set_host_control`) and `host_control_client.py`, run in CI against
  the real host service by `tests/decky_host_control_test.py`.
- `src/index.tsx` -- compiles cleanly (`pnpm install && pnpm run build`)
  against the real, current `@decky/ui`/`@decky/api`/`@decky/rollup`
  packages, not just written from memory of the API shape.

**Not verified** -- there is no Decky Loader runtime in this project's
environment, so none of the following has been observed directly:
- That Decky actually loads this plugin (manifest schema, `main.py`'s
  `Plugin` class shape, and the lifecycle hooks were all copied from the
  current official `SteamDeckHomebrew/decky-plugin-template`, fetched
  while writing this, but "matches the template" isn't the same as
  "confirmed working in Decky itself").
- That `decky.DECKY_USER_HOME` (used for the settings file path in
  `main.py`) is exactly the right constant/value on a real Deck.
- The actual Quick Access Menu UI rendering, `TextField`'s
  `bIsPassword` prop behavior, or any real user interaction with it.

If you install this and something doesn't work, please report exactly
where it breaks (fails to load, panel doesn't render, button does
nothing, etc.) -- that tells us which of the untested boundaries above
is the actual problem.

## Building

```sh
pnpm install --frozen-lockfile
pnpm run build
```

`pnpm-lock.yaml` pins every dependency, and `package.json`'s
`packageManager` field pins pnpm itself (9.15.9; `corepack enable`
provides it). After changing `package.json`, run `pnpm install` and commit
the updated lockfile.

Then install the plugin directory into Decky Loader the normal way for
a locally-built plugin (copy it into Decky's plugin directory, or use
Decky's developer/test-plugin loading flow -- see Decky Loader's own
documentation, since this project doesn't maintain that part).
