# DualDeck -- Decky Loader plugin

Lets you start or stop DualDeck's streaming server from the Steam
Deck's Quick Access Menu, on a melonDS that's already running on your
HTPC -- no walking over to the host, no restarting melonDS. See GitHub
issue "Decky plugin to start/stop the host server" for the original
request.

## What this actually does

Toggles the same live on/off switch documented in
`docs/known-limitations.md`'s "Live-toggle: start/stop remote streaming
without restarting melonDS" section: melonDS (built from
`host/melonds-patches/0001-remote-server-integration.patch`) runs a
small always-on management listener whenever a management token is
configured (`Config > Emu settings > General`, or
`MelonDSRemote.ManagementToken` in `melonDS.toml`), independent of
whether remote streaming itself is currently on. This plugin is just a
thin client for that listener -- see `management_client.py`.

This does **not** start or stop melonDS itself -- melonDS has to already
be running on the host for there to be anything to toggle.

## Setup

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
pnpm install
pnpm run build
```

Then install the plugin directory into Decky Loader the normal way for
a locally-built plugin (copy it into Decky's plugin directory, or use
Decky's developer/test-plugin loading flow -- see Decky Loader's own
documentation, since this project doesn't maintain that part).
