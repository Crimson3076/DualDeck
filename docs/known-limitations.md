# Known Limitations

This is the consolidated, authoritative list of what does not work yet or
is known to be incomplete, required as its own deliverable by `SPEC.md`
section 25. Individual design docs (`architecture.md`, `protocol.md`,
`testing.md`) call out gaps in context as they come up; this file is the
single place to check "is X done yet" without reading everything else.

## melonDS integration: patch exists, builds, handshake, real-frame delivery, and real input injection verified

`host/melonds-patches/0001-remote-server-integration.patch` implements
the integration against melonDS commit
`10a173b5536fc75cd93f8a3868349dad963542ef`. Unlike the standalone
`host/remote-server` (which still runs against `SyntheticFrameSource` and
`LoggingInputSink` for its own independent testing), the patch wires the
same protocol/host networking code into real melonDS state via
`MelonDSFrameSource`/`MelonDSInputSink`/`RemoteServerBridge`.

**Verified**: the patch applies to a fresh clone and builds from scratch;
the patched binary's embedded remote server starts and correctly performs
a real authenticated TCP handshake (including rejecting a wrong auth
token) against a raw-socket test client, under Xvfb with
`MELONDS_REMOTE_ENABLE=1`. A minimal, fully original homebrew `.nds` ROM
(`tests/homebrew-test-rom/`, written from scratch -- no copyrighted
content) was direct-booted successfully in the patched binary, and the
video path delivered a **stable, non-black, non-test-pattern** frame
consistently across repeated reads and separate process runs -- confirming
`GPU::GetFramebuffers()` → `pushBottomFrame()` → the network client
really does carry live `RunFrame()`-driven output, not a static
placeholder.

Going further, the ROM was extended into a genuinely **interactive**
program that continuously reads the real `KEYINPUT` hardware register
and reflects currently-held buttons as a visible color change
(`tests/homebrew-test-rom/arm9.c`), and driven through the *actual*
network pipeline end-to-end
(`tests/homebrew-test-rom/interactive_pipeline_test.py`): real UDP
`ControllerState` packets → `NetServer` → `RemoteServerBridge` →
`EmuInstance::inputProcess()`'s merge into `inputMask` →
`NDS::SetKeyMask()` → the emulated CPU reading `KEYINPUT` → the running
program reacting → `GPU::GetFramebuffers()` → `pushBottomFrame()` → the
network client. Holding each of several button states (A, B, Up, A+Up,
released) produced **exactly one stable pixel value across 50
consecutive delivered frames per state**, with clean, immediate
transitions and zero noise. This is a genuine, unambiguous, end-to-end
confirmation that DS controls sent over the remote protocol affect a
running program -- **not** a unit test of the merge logic in isolation.

This also **conclusively resolved** the two items the previous pass of
this document flagged as open:

- **Pixel channel order is B,G,R,A bytes in memory** (byte0=Blue,
  byte1=Green, byte2=Red, byte3=Alpha) -- determined by observing which
  byte changed for each button-controlled color channel across a
  continuous session, which the earlier one-sample-per-run static-color
  test couldn't distinguish. This byte-order finding was and is correct;
  what turned out wrong later was the *client's SDL texture format
  constant* used to display those same bytes -- see "Real-usage bug
  fixes" below for that specific, distinct bug.
- **Engine B (not engine A) is the "bottom" screen** delivered by
  `GPU::GetFramebuffers()`.

A sustained-session stability run was also carried out (SPEC.md section
20 criterion (12)) using `tests/homebrew-test-rom/stability_test.py`
against the live patched binary, with continuous ~120Hz input traffic and
continuous video draining for an extended period:

**Result: ran the full 1800s (30 minutes) target duration with 106,785
video frames delivered (~59 fps average), zero connection errors, zero
video stalls beyond 0.11s max, and zero measured RSS growth in the
melonDS process (185,436 kB at both start and end) -- the process was
still running and responsive at the end of the run.** This was a
continuous session with ~120Hz UDP input traffic (cycling through
several button combinations every second) and continuous video draining
throughout, i.e. sustained real network activity for the full period, not
an idle emulator left alone.

**Still not verified** (both require assets this project deliberately
does not include or seek out, for the same copyright reasoning that
excludes ROMs from this repository -- see
`tests/homebrew-test-rom/README.md`'s caveat):

- Booting to the DS system menu without a cartridge (needs genuine
  Nintendo firmware).
- A commercial-cart-style ROM (the test ROM is homebrew, which skips the
  "secure area" decryption step entirely -- untested whether that path
  works).
- A specific commercial game's own input-handling code, real Steam Deck
  hardware, and a physical gamepad -- the *mechanism* verified here
  (remote button state → `SetKeyMask()` → CPU-visible register → program
  logic → framebuffer → network client) is identical regardless of what
  program is running, but a particular commercial game's own logic
  hasn't been (and, per this project's constraints, can't be in this
  sandbox) exercised.

Concretely, on SPEC.md section 20's acceptance criteria:

- (1) melonDS runs a Nintendo DS *game* on the host: **not met literally**
  -- a fully original, from-scratch homebrew program has been run and
  verified interactive, not a commercial game (see the caveat above for
  why).
- (2)/(3) real DS output on top/bottom screens: **met** for the bottom
  screen (verified above); the top screen uses the same
  `GetFramebuffers()` call and code path, just not separately re-tested
  with a second capture point.
- (4)-(8) DS controls/touch actually affect a running program: **met**,
  for the reasons above -- with the explicit caveat that "a running
  program" here means the original homebrew test program, not a
  commercial game.
- (9) touches outside the rendered rect are ignored: covered by
  `protocol/touch_mapping.h`'s unit tests at the coordinate-mapping
  level; not re-verified against a real game's touch-sensitive UI.
- (12) 30-minute emulator stability: **met** -- a full 1800s run with
  continuous input/video traffic completed with zero errors, zero RSS
  growth, and the process still alive and responsive; see the stability
  run above.

**EmuDeck ROM directory default**: the patch also makes melonDS's own
"Open ROM" dialog default to EmuDeck's standard NDS ROM directory
(`~/Emulation/roms/nds`) the first time it's opened (i.e. before melonDS
has ever remembered a `LastROMFolder`), if that directory exists --
`src/frontend/qt_sdl/Window.cpp`'s `pickROM()`. This is a small,
host-local convenience, not a new protocol feature: it does not browse,
list, or auto-select a ROM on the client's behalf (`SPEC.md` section 13
explicitly forbids exposing ROM browsing to the client), and it doesn't
auto-launch anything at startup -- the host operator still picks a game
through melonDS's own menu (or a `.nds` path on the command line, as
before), just starting from a more useful default folder.

## OpenGL/OpenGLCompute 3D renderer: now supported for remote video capture

Previously, remote video capture only worked with melonDS's **Software**
3D renderer -- `GPU::GetFramebuffers()` only hands back real RAM pointers
for that renderer; with **OpenGL** or **OpenGLCompute** it hands back a
GL texture handle instead (see the "Real-usage bug fixes, round 2" entry
below for how this was first discovered, on real Steam Deck hardware).
Fixing the OpenGL renderer path properly (rather than just diagnosing it)
was the first thing tackled after that hardware round, since it was the
single biggest constraint on what games could actually be played well on
the host while still streaming.

**How it works**: `GPU_OpenGL.cpp`'s `GLRenderer` stores both screens in
one `GL_TEXTURE_2D_ARRAY` (`FPOutputTex[frontbuf]`, format
`GL_RGBA`/`GL_UNSIGNED_BYTE`) -- layer 0 is the top screen, layer 1 is
the bottom (established by reading `GLRenderer::Init()`'s
`glFramebufferTextureLayer(..., FPOutputTex[i], 0, 0/1)` calls, the same
way the software path's "engine B is the bottom screen" was established
by reading `GPU_Soft.cpp`, not guessed). A new class,
`GLBottomScreenCapture` (`src/frontend/qt_sdl/remote_server/GLBottomScreenCapture.{h,cpp}`
in the patch), reads that layer back directly:

- Dereferences the `GLuint` texture name `GetFramebuffers()` already
  points `top` at when it returns false (previously unused in that
  branch).
- Queries the texture's actual size via `glGetTexLevelParameteriv`
  (rather than assuming a size), and binds a framebuffer to read
  layer 1 with `glReadPixels(..., GL_BGRA, GL_UNSIGNED_BYTE, ...)` --
  requesting `GL_BGRA` directly from the GPU means no manual R/B byte
  swap is needed to match the wire format, unlike the client-side
  `SDL_PIXELFORMAT_BGRA8888`-vs-`BGRA32` bug this project hit earlier
  (see "Real-usage bug fixes" below) -- this is a standard, well-defined
  `glReadPixels` format/type combination for exactly this purpose, not a
  hand-rolled conversion.
- If the user raised melonDS's internal 3D render resolution above 1x
  (`3D.GL.ScaleFactor`), the texture is larger than native DS
  resolution -- downscaled to 256x192 via a GPU `glBlitFramebuffer` first,
  since the wire protocol and client are both fixed at native DS
  resolution in this version (higher internal resolution doesn't reach
  the remote client, it just renders more cleanly before being
  downscaled for streaming).
- `EmuThread.cpp`'s existing frame-push block calls this when
  `GetFramebuffers()` returns false and a GL context is current, keeping
  the previous one-time diagnostic log as a fallback for any case this
  doesn't cover (so a broken capture never fails silently).

**Verified end-to-end** against the real patched melonDS binary + the
real homebrew test ROM + the real SDL3 client, under Xvfb using Mesa's
software GL rasterizer (`LIBGL_ALWAYS_SOFTWARE=1`, since this sandbox has
no GPU) -- this environment happens to be able to create a real (if
software-emulated) OpenGL context, unlike the earlier round of hardware
bugs which needed a real gamepad/Steam Input this sandbox still can't
provide:

- With `3D.Renderer=OpenGL` and the default `3D.GL.ScaleFactor=1`: sustained
  ~58 fps video delivery over multiple stats windows
  (`NetServer: stats -- ... video: sent=293 (58.3 fps) ...`), no dropped
  frames beyond normal polling variance, and the fallback diagnostic did
  **not** fire -- confirming the direct-read (no downscale) path works.
- With `3D.GL.ScaleFactor=2` (512x384 internal texture, exercising the
  downscale-blit branch specifically): sustained ~57.5 fps with **zero**
  dropped frames, fallback diagnostic did not fire -- confirming the
  `glBlitFramebuffer` downscale path also works.
- Existing protocol/host unit tests and the full CMake build still pass
  unchanged (this patch only touches the melonDS-side integration files,
  not `protocol/`/`host/remote-server/`).

**Not independently verified**: pixel-level correctness (i.e. confirming
the actual DS content, not just frame count/rate) the way the software
path's KEYINPUT-reactive color test did --
`tests/homebrew-test-rom/interactive_pipeline_test.py` predates the
protocol's v3 handshake bump and its handshake is now rejected
(`bad magic/type/version`), and updating that script was out of scope for
this pass. Confidence instead comes from: the capture path using
well-defined, standard GL calls rather than hand-rolled byte
manipulation, matching real DS output (not zeros/garbage) at real DS
frame rate for a sustained period, and zero errors/drops on either
side. Also not tested: `OpenGLCompute` specifically (same `GLRenderer`
code path per `GPU_OpenGL.h`, but compute shaders need GL 4.3+/GLES 3.1+,
which this sandbox's software rasterizer doesn't support), and real
Steam Deck hardware (the actual GPU/driver this was originally reported
on).

## Config/UI toggle for the remote server: implemented (was env-var-only)

Filed as a GitHub issue against this project ("integrate into melonDS
without launching from run-host.sh -- useful for users who have games
launching via Steam Big Picture"): previously, the only way to turn the
remote server on at all was `MELONDS_REMOTE_ENABLE=1` set before
launching the patched `melonDS`, which meant it silently never started
for anyone launching melonDS directly from a Steam/EmuDeck shortcut
(the normal way Big Picture/Gaming Mode launches anything) instead of
through a wrapper script that sets that env var.

Fixed by adding persisted `MelonDSRemote.*` Config keys (mirroring each
`MELONDS_REMOTE_*` env var) that are read as a fallback whenever the env
var isn't set, plus an actual **"Enable melonDS Remote (Steam Deck
streaming)"** checkbox on Emu Settings' General tab for the one setting
that actually needs deliberately turning on (the rest -- bind
address/ports/auth token/state dir/discovery -- already have sensible
defaults matching the old env-var defaults, so they don't need their own
UI to be usable, just Config keys for anyone who wants to override
them). Takes effect on melonDS's next launch, not live, since the remote
server is constructed once when the process starts.

**Verified end-to-end** against the real patched binary: with zero
`MELONDS_REMOTE_*` env vars set, the remote server did not start;
toggled the new checkbox on via the real Qt UI (driven with `xdotool`
under Xvfb) and confirmed `melonDS.toml` persisted
`MelonDSRemote.Enable = true`; killed and relaunched the same
zero-env-var command, and this time the remote server started and
accepted a real socket connection on its own -- see
`host/melonds-patches/README.md` item 16 for the full account.

## Easy Big Picture/Gaming Mode client install: scripted, no manual "Add a Non-Steam Game" needed

Also filed as a GitHub issue ("make an easy way for users to launch from
steam big picture mode/game mode"): adding the client as a Gaming Mode
shortcut previously meant the standard but tedious manual Steam flow
(Games -> Add a Non-Steam Game -> Browse... -> set Launch Options ->
set Controller Layout).

`scripts/install-steam-shortcut.sh` (thin wrapper around
`scripts/lib/steam_shortcut.py`) automates the shortcut-adding part: it
builds the client if needed and writes directly into Steam's binary
`shortcuts.vdf`, using a small dependency-free implementation of that
(community-reverse-engineered, not Valve-documented) format. Deliberately
conservative given it's editing the user's real Steam library file:
refuses to run while Steam is running (unless `--force`) since Steam
caches that file in memory and can silently overwrite the change on its
next save; always backs up the existing file first; parses its own
freshly-written output back as a sanity check before ever touching the
real file; and is idempotent (matches by `Exe` path, so re-running with
different `--launch-options` updates the existing entry instead of
duplicating it). Applies to **every local Steam user by default**, not
just one picked interactively -- a Steam Deck's controller-only input
has no reliable way to type an answer to a "which user?" prompt, so an
early version that errored out and asked for `--user` was changed to
just apply to all of them instead (`--user` still exists to narrow it,
for scripted/single-user use, but is never required). It does **not**
touch Steam Input controller-layout assignments (a separate per-shortcut
config keyed by a derived ID) -- `docs/steam-deck-setup.md`'s "set
Controller Layout to Gamepad" step is still manual.

**Verified**: cross-checked bidirectionally against the independent
reference `vdf` Python package (`pip install vdf`, an established
community implementation used by other Steam tooling) -- confirmed that
package parses this script's binary output correctly, and that this
script's own parser correctly reads output produced by that package's
own `binary_dumps`, both directions preserving all fields including the
computed legacy shortcut `appid`. Also exercised the actual CLI
end-to-end against fake `$HOME`/Steam-userdata directory structures:
fresh-file creation, `--dry-run` preview, preserving a pre-existing
unrelated shortcut untouched while adding a new one, idempotent re-run
updating the existing entry in place (still exactly 2 total entries, not
3), the "Steam is running" guard correctly blocking without `--force` and
proceeding with it, and -- specifically for the apply-to-all-users
change -- a fake two-user setup: one run added the shortcut to both
users' `shortcuts.vdf` with zero prompts, and a second run with
different `--launch-options` updated both in place (still exactly one
entry each, not two). **Not verified**: against a real Steam client
actually reading the result (this sandbox has no real Steam
installation) -- the cross-validation above is strong evidence the
format is correct, but "Steam's Big Picture UI actually shows the
shortcut and launches it" hasn't been observed directly.

**Removal script**: `scripts/uninstall-steam-shortcut.sh` (also
packaged as `client/uninstall-steam-shortcut.sh`) undoes the above --
same underlying `scripts/lib/steam_shortcut.py`, now with a `--remove`
mode rather than a separate script, so it shares all the same safety
behavior (backup first, refuses while Steam is running unless `--force`,
round-trip-validates its own output, applies to every local Steam user
automatically with zero typing). It matches purely on the stored `Exe`
path, so it still works even if the client binary itself was since
deleted or the `build/` directory removed. Unlike the install path,
finding nothing to remove is treated as a harmless no-op (exit 0), not
an error, since re-running it after it already succeeded (or against a
user who never had the shortcut) is an expected, safe case.
**Verified**: same fake-`$HOME`/userdata-directory technique as above --
removing a shortcut while a decoy unrelated entry survives untouched,
re-running the removal a second time as a clean no-op, `--dry-run`
preview, and the same `vdf`-package round-trip cross-check. **Not
verified**: against a real Steam client, same caveat as the install
script above.

**Fixed install/uninstall breaking across release downloads (real bug
report)**: the removal script above shipped in v0.1.11 with a real bug --
each release archive extracts into a directory whose name embeds the
commit hash (`melonds-remote-<commit>-linux-x86_64/`), and both install
and uninstall registered/matched the Steam shortcut's `Exe` as wherever
the binary happened to currently sit, inside that per-release folder.
Download a newer release into a new folder and the uninstaller's
computed `Exe` no longer matched the old shortcut entry, so removal
silently no-opped and a fresh install added a stale duplicate instead of
updating the existing one -- exactly what the reporting user hit.

While fixing this, reading the actual linked binary (`readelf -d`)
turned up a second, related bug: the packaged
`client/install-steam-shortcut.sh` pointed Steam's `Exe` straight at the
raw `melonds-remote-client` binary rather than at `client/run-client.sh`
(the wrapper that sets `LD_LIBRARY_PATH` so the bundled
`client/lib/libSDL3.so*` gets found). There's no `$ORIGIN`-relative
RPATH anywhere in the CMake build, so the binary's baked-in
`DT_RUNPATH` just points at the CI build machine's ephemeral
`/tmp/sdl3-install/lib`, which doesn't exist on a real user's machine.
Since Steam launches `Exe` directly, bypassing `run-client.sh`'s
`LD_LIBRARY_PATH` export entirely, the client would fail to find
`libSDL3.so.0` when launched via the Steam shortcut specifically -- a
plausible root cause for an earlier real-hardware report of "shortcut
created, hit Play, nothing happened" that was never conclusively
diagnosed at the time (it was provisionally worked around by a Steam
Deck reboot, cause unconfirmed).

**Fix**: both install scripts now copy everything the shortcut needs
(binary, bundled SDL3 lib, `run-client.sh`, `ensure-packages.sh`, and a
copy of `steam_shortcut.py` plus the uninstall script itself) into a
fixed central directory, `~/.config/melonds-remote-client/install/`
(a sibling of the existing `device_id.txt`/`last_host.txt` state
directory, kept as a subdirectory so uninstall's cleanup can never touch
those), and point the Steam shortcut's `Exe` at `run-client.sh` inside
that copy -- fixing the `LD_LIBRARY_PATH` bug as a side effect, since
Steam now always launches the wrapper, never the raw binary. Re-running
install later, even from a completely different, newer release's
extracted folder, always resolves to this same fixed path, so it updates
the existing shortcut instead of duplicating it, and the original
extracted/downloaded folder can be deleted immediately after installing.
Uninstall now also deletes this central directory (not just the Steam
shortcut entry), so nothing is left behind, and -- critically -- no
longer depends on its own invocation location or the original repo/
archive still existing, since it's the copy living in the central
directory (or an identical stand-alone copy) that actually does the
work.

To migrate shortcuts installed by an older version of this project
(pointing at a now-stale per-release path), `remove_shortcut`/
`upsert_shortcut` in `scripts/lib/steam_shortcut.py` now also match by
`AppName` (default `"melonDS Remote"`) as a fallback when `Exe` doesn't
match anything, and overwrite the stale `Exe` on a match -- safe because
this project only ever creates shortcuts under that one fixed default
name.

**Verified**: extended the same fake-`$HOME`/userdata-directory
technique -- installing from one fake "download" tree and then
reinstalling from a second, differently-named one still ends up with
exactly one shortcut entry (not two), pointing at the fixed central
path; a hand-crafted legacy entry (`AppName` matching, stale `Exe`) gets
migrated in place by both install and removal instead of left as an
orphaned duplicate; uninstall deletes the central `install/` directory
while leaving `device_id.txt`/`last_host.txt` untouched, and is a clean
no-op if run again (or if nothing was ever installed); the packaged
`run-client.sh` wrapper, copied verbatim into the central directory,
was confirmed to still export a `LD_LIBRARY_PATH` including its own
`client/lib` before exec'ing the binary. **Not verified**: against a
real Steam client or real Steam Deck hardware, same caveat as above --
this is strong evidence the fix is correct but hasn't been observed
launching a real shortcut on a real Deck.

## Release scripts streamlined: no user-selection prompt, no `.desktop` launchers (tried, reverted)

Two follow-up requests after the above: reduce manual input further, and
make every script in the release archive runnable by double-clicking it
in a file manager rather than needing a terminal.

- **Multi-Steam-user prompt removed**: `install-steam-shortcut.sh`
  previously errored out and asked for `--user <id>` when it found more
  than one local Steam account, which is exactly the kind of prompt a
  Steam Deck's controller-only input can't answer (no keyboard to type an
  id into). Changed to apply to every local Steam user automatically by
  default (`--user` still exists to narrow it for scripted/single-user
  use, but is never required) -- see the "Easy Big Picture" section
  above, whose verification section now also covers this.
- **`.desktop` launcher files: tried, and reverted.** The plan was to
  ship a `.desktop` file next to each script using the `%k` field code
  (the launched desktop file's own path) so a single static file would
  work regardless of where the user extracts the archive, unknown at
  build time. This was caught before shipping specifically *because* of
  the practice established throughout this project of verifying against
  real tools rather than trusting spec-reading alone:
  `desktop-file-validate` (from `desktop-file-utils`) flagged the first
  attempt's escaping as invalid, and once that was fixed to pass
  validation, testing the *actual* execution with `gio launch` (GLib's
  own launcher, i.e. what GNOME/Nautilus uses under the hood) showed
  `%k` doesn't expand at all in the GLib version available here -- the
  Exec line silently ran as if empty, no error, nothing happened. Since
  a `.desktop` file that passes static validation but silently fails at
  the exact moment a user double-clicks it is worse than not having one,
  this was reverted in favor of relying on `run-host.sh`/`run-client.sh`/
  `install-steam-shortcut.sh` already being plain, self-locating,
  zero-argument executable scripts: on SteamOS Desktop Mode and Bazzite
  (both KDE Plasma/Dolphin -- this project's two actual target
  environments), Dolphin natively offers to run an executable `.sh` file
  on double-click with no `.desktop` wrapper needed at all, confirmed
  against `desktop-file-utils`' own documentation of that behavior (not
  independently re-verified against a live Dolphin session, since this
  sandbox has no KDE desktop environment -- see the caveats throughout
  this document about what real-desktop-environment testing isn't
  possible here). GNOME-based file managers (Nautilus) may still need a
  one-time "Executable Text Files: Run" preference change or a
  right-click "Open Terminal Here" fallback, documented in
  `docs/steam-deck-setup.md`/`docs/bazzite-host-setup.md`.

## Live-toggle: start/stop remote streaming without restarting melonDS, plus a Decky plugin

Also filed as a GitHub issue ("Decky plugin to start/stop the host
server"). Two readings of that request were possible: (a) toggle remote
streaming inside an already-running melonDS, or (b) remote-launch/kill
the whole melonDS process. (b) would need a new always-on daemon
separate from melonDS itself (to receive "start" commands even when
melonDS isn't running) with its own security/auth story -- given this
project's sandbox can't test a Decky Loader runtime at all, (a) was the
scope actually built, as the one that doesn't add a new persistent
network service beyond what melonDS itself already runs.

**Host side**: `remoteServer` (`EmuInstance`) used to be constructed
once at process startup and never touched again. It's now start-/
stoppable at any point in the process's life
(`EmuInstance::startRemoteServer()`/`stopRemoteServer()`, both
idempotent, both under a new `remoteServerMutex` that every reader of
`remoteServer` -- `EmuThread.cpp`'s frame-push and
`EmuInstanceInput.cpp`'s input-merge -- now holds for as long as they
actually dereference it, not for the whole surrounding frame/function).
A new `ManagementServer` class
(`src/frontend/qt_sdl/remote_server/ManagementServer.{h,cpp}` in the
patch) is a small, separate, always-on (once configured) TCP listener --
independent of whether remote streaming itself is on -- that something
else on the LAN can send `TOKEN ENABLE`/`DISABLE`/`STATUS` to. It only
starts at all if `MelonDSRemote.ManagementToken` is set (empty by
default -- opt-in, since it's a new network listener whenever melonDS is
running); the token is compared with the same constant-time comparison
`NetServer` already uses for its own auth token.

**Verified end-to-end** against the real patched binary: with the
remote server off and a management token configured, sent `STATUS` (got
`DISABLED`), a wrong token (got `ERROR bad token`), `ENABLE` (got
`OK ENABLED`, and the remote server actually started -- a real client
connected and streamed normally), `DISABLE` while that client was
actively connected (client's connection was refused immediately, and the
existing screen-sizing-restore callback fired correctly:
`melonds-remote: client disconnected -- restoring local
ScreenSizing=0`, confirming the forced-shutdown path fires the same
cleanup as a graceful disconnect), then a second `ENABLE`/idempotent
re-`ENABLE`/`STATUS` cycle -- confirming the remote server can be
stopped and restarted repeatedly within one melonDS process, not just
once.

**The Decky Loader plugin** (`decky-plugin/` at the repo root) is a thin
client for that same management protocol. Its Python backend logic
(`main.py`'s settings/status/enable/disable methods) and the wire
protocol itself (`management_client.py`) were both exercised end-to-end
against the real host above (using a minimal stand-in for the `decky`
module, since this sandbox has no real Decky Loader runtime); the
frontend (`src/index.tsx`) compiles cleanly against the real, current
`@decky/ui`/`@decky/api`/`@decky/rollup` packages (`pnpm install && pnpm
run build`), and `plugin.json`/`main.py`'s lifecycle hooks/`package.json`
were all modeled directly on the official
`SteamDeckHomebrew/decky-plugin-template`, fetched while writing this,
not reconstructed from memory. **Not verified**: actually loading this
plugin into a real Decky Loader instance, since none exists in this
project's environment -- see `decky-plugin/README.md` for the precise
boundary between what's tested and what's a best-effort match to the
template.

## The SDL3 client: build- and run-verified, and now tested once on real Steam Deck hardware

Earlier passes of this document said the client had never been compiled,
since no SDL3 package was available in this development environment.
That's since been resolved by building SDL3 3.2.16 from source (SDL3 is
not in Ubuntu 24.04's apt repositories) and configuring the project
against it:

- `client/src/main.cpp` and the full `melonds-remote-client` binary now
  **compile cleanly** with `-Wall -Wextra -Wpedantic -Wconversion
  -Wshadow` against real SDL3 headers/libraries, not just
  `net_client.cpp`/`.h` standalone as before.
- The built binary was **run** (not just compiled) under Xvfb, first
  against the standalone `melonds-remote-server` prototype and then
  against the actual patched melonDS host running the interactive
  homebrew ROM: in both cases the real `NetClient` completed a real
  handshake ("`[net] connected to 127.0.0.1 (session ...)`"), and the
  host's own logs confirmed a sustained real session (e.g. "`NetServer:
  stats -- input: accepted=384 ... video: sent=285 (56.5 fps) ...`").
  This is the first time the actual SDL3 client binary -- not a
  raw-socket stand-in -- has been exercised end-to-end.
- The auto-reconnect thread was exercised implicitly by this real run
  (the client's connection thread is what established the session
  above), though a deliberate host-restart-mid-session test was not
  separately performed.
- **First real Steam Deck hardware test**: controller input and touch
  input both worked. This same run also surfaced three real bugs, all
  from the "Real-usage bug fixes" section below (video not appearing at
  all, the in-app menu opening on a single button instead of a
  deliberate Start+Select hold, and no on-screen indication of how to
  open the menu in the first place) -- none of which this sandbox's
  Xvfb/`xdotool`-keyboard-driven testing could have caught, since it has
  no real gamepad, no Steam Input layer, and (being GPU-less) forced
  software 3D rendering throughout every prior test in this project.
  `SPEC.md` section 19's "Manual Steam Deck tests" list is otherwise
  still open (OLED vs LCD, Gaming Mode vs Desktop Mode coverage, etc.);
  the binary built here is also specific to this environment's Ubuntu
  24.04 x86_64 SDL3 build -- see the release packaging notes for what
  that means for portability to a real Deck.

## Video transport is Stage 1 only

Raw BGRA8888 frames over TCP, no compression, per `SPEC.md` section 8.4's
"Stage 1: development transport". Bandwidth at 60 FPS is ~11.8 MB/s,
acceptable for LAN but not evaluated against the Stage 2 options
(H.264/H.265/AV1/MJPEG/custom delta encoding) at all yet.

## Authentication: device approval and pre-shared tokens both implemented; no QR/certificate pairing

Spec section 13's "later pairing options" are adapted rather than
implemented literally -- see below -- and end-to-end verified (real host
+ real SDL3 client, not just unit tests -- see
`host/melonds-patches/README.md`):

- **Device approval** (the default when no `--auth-token` is configured):
  the client generates a random, persistent device identity once and
  sends it on every connection attempt, to every host, forever -- there
  is no code typed on either side. An unrecognized identity causes the
  host to queue a pending connection request naming the client and its
  address (console log with `approve`/`deny` commands on the standalone
  host; a `QMessageBox` Approve/Deny dialog on the melonDS-integrated
  host). Once a human approves it, that identity is remembered and every
  future connection (including the existing auto-reconnect-on-drop
  logic) is accepted silently -- no re-approval needed unless the host's
  approved-device state is deleted. See `docs/protocol.md`'s
  "Authentication and device approval" section for the full state
  machine, and its "History: the 6-digit pairing code" subsection for why
  this replaced an earlier typed-code flow (Steam Input doesn't reliably
  bring up a virtual keyboard in Gaming Mode, so requiring the client to
  type anything wasn't a workable UX).
- **Pre-shared token** (`--auth-token TOKEN`): unchanged from before,
  still available as an explicit opt-in for scripting/CI
  (`tests/smoke_test.py` uses it) -- bypasses device approval entirely.

**Not implemented**: QR codes, certificate-based pairing, or any
post-handshake re-authentication (a session that's been accepted stays
accepted until it disconnects or times out; there is no `sessionId`
validation on subsequent packets in this version, so nothing currently
distinguishes a stale session from a current one at the protocol level
beyond the one-connection-at-a-time plus source-IP-match rule enforced by
the transport). There is also no UI to list or revoke individual approved
devices -- only deleting the whole approved-device state file (forgetting
every previously-approved client at once).

## LAN discovery implemented; still no multi-client, no IPv6

- **LAN discovery** (`SPEC.md` section 8.1's "future versions" item, now
  implemented, end-to-end verified with a real host binary + real SDL3
  client -- see `docs/protocol.md`'s "Discovery payload" section): the
  host broadcasts availability over a separate UDP port (`8763` by
  default) and the client scans for it on launch instead of requiring
  `--host`. A gamepad/keyboard-navigable list is always shown (bitmap-font
  rendered, so it works in Steam Deck Gaming Mode where there's no
  visible terminal -- see `client/src/bitmap_font.h`), even with just one
  host discovered, rather than auto-connecting silently -- so switching
  to a different HTPC is always one screen away, not just when there
  happens to be more than one on the LAN. The previously-picked host is
  pre-highlighted for a quick one-button reconnect, and the list keeps
  rescanning live while shown. Not implemented: mDNS/SSDP/any standard
  discovery protocol -- this is a small custom broadcast request/response
  instead, with no external dependency. `--host`/a positional host
  address still works exactly as before and skips discovery entirely
  (scripting/CI use, `tests/smoke_test.py`).
- Only one client at a time, by design (`SPEC.md` section 7.1's initial
  scope, and an explicit non-goal in section 21).
- IPv4 only (including discovery).

## Real-usage bug fixes: touch, colors, screen layout, in-app menu

Four issues reported from actually using the client against the real
melonDS-integrated host (not caught by earlier protocol-level/unit
testing, since none of them are wire-format bugs):

- **Touch got stuck on after the first tap**: `EmuInstanceInput.cpp`'s
  remote-input merge set `isTouching = true` when a remote touch was
  active, but never set it back to `false` when the touch ended -- once a
  client tapped the touchscreen once, melonDS treated it as permanently
  held at that position forever after (see `EmuThread.cpp`'s
  `isTouching ? TouchScreen(...) : ReleaseScreen()` check, run every
  frame). Fixed by tracking whether the *current* touch was caused by
  remote input (`EmuInstance::remoteTouchActive`) and releasing it
  specifically when the remote client's touch ends, without clobbering a
  touch caused by local mouse/touchscreen input instead. Build-verified;
  not exercised against a real touch-reactive DS program (would need a
  new homebrew ROM that reads the touchscreen controller registers, which
  wasn't built for this pass -- see the interactive-ROM caveats above for
  why building new homebrew assets is deliberately limited here).
- **Client screen colors were badly wrong**: `client/src/main.cpp` created
  its video texture with `SDL_PIXELFORMAT_BGRA8888`. SDL names its 32-bit
  "packed" formats after a bit layout read MSB-to-LSB as a single integer,
  not a byte order in memory -- on a little-endian machine (the normal
  case, including Steam Deck) `SDL_PIXELFORMAT_BGRA8888` actually means
  the same in-memory byte order as `SDL_PIXELFORMAT_ARGB8888`, a
  completely different order than the wire format (and melonDS's own
  framebuffer) uses. Only `SDL_PIXELFORMAT_BGRA32` (the "32" alias) is
  defined by SDL to always mean "bytes in memory match the name",
  regardless of host endianness. Confirmed empirically, not just from
  reading `SDL_pixels.h`: feeding an isolated test program the exact
  B,G,R,X bytes melonDS produces for pure red, `BGRA8888` displayed it as
  **black**, `BGRA32` displayed it correctly as **red**. Fixed by
  switching to `SDL_PIXELFORMAT_BGRA32`; re-verified live against the
  standalone host's animated gradient pattern, which now shows the full
  yellow/orange/magenta/teal/blue range instead of the washed-out
  blue/purple/teal-only palette the bug produced. See `docs/protocol.md`'s
  "Video payload" section.
- **melonDS's own window showed both screens while a client was actively
  streaming the bottom one**: redundant, and not the "Wii U GamePad"
  layout `SPEC.md` describes (TV/host shows top screen, handheld/client
  shows bottom). Fixed by wiring a new `NetServerConfig::onClientConnectionChanged`
  callback (fired true/false as a client's session starts/ends) to set
  melonDS's existing `ScreenSizing` config to `screenSizing_TopOnly` while
  connected, restoring whatever was configured before once the client
  disconnects. Verified live against the real melonDS-integrated host +
  real client: connecting logged `melonds-remote: client streaming --
  showing top screen only locally (was ScreenSizing=0)` and disconnecting
  logged `melonds-remote: client disconnected -- restoring local
  ScreenSizing=0`.
- **No way to exit or reconnect without killing the process**: the client
  had no in-app menu at all -- Gaming Mode has no window chrome to click a
  close button, and there was no way to switch to a different discovered
  host without restarting the whole client. Fixed by adding a gamepad
  Start+Select-held chord (or Escape in Desktop Mode) that opens an
  in-app menu with **Resume**, **Change Host** (returns to the
  discovery/selection screen without exiting the process -- hidden when
  `--host` was given explicitly, since there's no host list to go back
  to), and **Exit**. Verified live: opening the menu, moving the
  selection, confirming "Change Host" (disconnects and shows the
  discovery screen again, reselecting reconnects), and confirming "Exit"
  (clean process exit, confirmed via `ps` showing no lingering process)
  all behaved as expected.

Also, while investigating these: the client's informational log messages
(`[net] connected to ...`, `[discovery] selected host ...`, etc.) were
using `std::printf` (stdout) instead of `std::fprintf(stderr, ...)`.
stdout is fully buffered once redirected to a file (the common case for
troubleshooting/log capture), which was observed firsthand during this
investigation: a client run with stdout redirected showed **no** log
output at all until the process was killed, even though multiple
messages had clearly been printed. Switched all of them to stderr, which
isn't buffered the same way and is what the host side (`NetServer`)
already uses for exactly this reason.

## Real-usage bug fixes, round 2: real Steam Deck hardware

Three more issues, this time from the client's first run on an actual
Steam Deck (not just the GPU-less Xvfb sandbox used for every prior test
in this project):

- **No video reached the client at all**, though controls and touch
  input both worked. Root cause: the melonDS-integrated patch's frame
  source only got real pixel data out of `GPU::GetFramebuffers()` when
  the 3D renderer was set to **Software** -- with the **OpenGL** renderer
  it hands back a GL texture handle instead, which was silently ignored.
  Every prior test in this project ran in a GPU-less sandbox that
  effectively forced the software renderer, so this had never come up
  before. Not a code regression -- a pre-existing, already-documented
  Stage 1 limitation surfacing for the first time on real hardware.
  Immediately after this hardware round, the OpenGL renderer path was
  actually implemented (not just diagnosed) -- see the dedicated
  "OpenGL/OpenGLCompute 3D renderer" section above for how. This also
  explains "I can't see what I'm tapping" from the same hardware run:
  touch input reaching the emulator was never in question, there was
  just no video feedback to aim it by.
- **The in-app menu opened by itself on a single Start or B press**
  instead of requiring the deliberate Start+Select hold it's designed
  for. The client's `Escape`-toggles-menu keyboard shortcut (added purely
  for keyboard-driven Desktop Mode testing) fired unconditionally,
  regardless of whether a gamepad was connected; the leading explanation
  is that Steam Input's default controller-binding template for a
  freshly-added non-Steam shortcut synthesizes a keyboard `Escape` press
  for some individual buttons (a common Steam Input behavior, meant to
  keep keyboard-only UI usable via a controller). Fixed two ways:
  `client/src/main.cpp`'s `Escape` handling now only fires when no
  gamepad is connected at all, and the actual Start+Select gamepad chord
  now requires a continuous ~350ms hold (`kMenuChordHoldUs`) before it
  fires, rather than triggering the instant both buttons are seen held in
  the same polled frame -- both as defense against whatever Steam Input's
  exact synthesis behavior turns out to be. `docs/steam-deck-setup.md`
  also now recommends setting the shortcut's Steam Input **Controller
  Layout** to a plain "Gamepad" template, which avoids any such synthesis
  in the first place. Not independently re-verified on real hardware (no
  physical gamepad or Steam Input layer exists in this project's sandbox
  -- see the caveat above) -- build-verified only.
- **No indication anywhere of how to open the menu** before you already
  know the combo -- the existing "START+SELECT TO CLOSE" hint only shows
  once the menu is already open. Fixed by adding a
  "HOLD START + SELECT TO OPEN THE MENU" hint to the discovery/searching
  screen, the host-selection list, and the "CONNECTING TO.../WAITING FOR
  APPROVAL..." banner -- i.e. visible from the moment the client starts,
  through connecting, per the original request ("inform the user how to
  open the menu when starting the application or connecting to the
  host").

## No exit control on the discovery/host-selection screen (GitHub issues #8, #9)

Filed after the above: despite the "HOLD START + SELECT TO OPEN THE MENU"
hint already being shown on the discovery/searching and host-selection
screens (see the previous section), holding Start+Select there actually
did nothing -- `discoverAndSelectHost()` (`client/src/main.cpp`) was a
separate function with its own event loop, and had never been wired up
to the chord/pause-menu logic that main()'s inner loop (post-host-
selection) already had. The only way off that screen was
`SDL_EVENT_QUIT`, which has no equivalent gamepad-only trigger in Gaming
Mode -- so opening the client accidentally, or before a host is running,
left no way out short of force-quitting from the Steam Quick Access Menu.
This matches both issue reports: #8 ("no exit application option from
the Searching for host screen") and #9's broader ask for exit/back
controls on every client screen -- discovery/selection was the only
screen actually missing one, since the connecting/waiting-for-approval
and gameplay screens already run inside the same inner loop as the
existing pause menu.

Fixed by giving `discoverAndSelectHost()` its own instance of the same
Start+Select deliberate-hold chord and pause-menu overlay (`RESUME`/
`EXIT`, reusing `renderPauseMenu()`), following the exact same
conventions as the in-app menu elsewhere (`kMenuChordHoldUs`'s constant
was hoisted to file scope so both places share it; the `Escape`-only-
without-a-gamepad gate for Desktop Mode is duplicated the same way).
Choosing `EXIT` (or `SDL_EVENT_QUIT`) both return `std::nullopt` from
`discoverAndSelectHost()`, which `main()` already treated as "cancel the
whole run" before this fix, so no caller-side changes were needed beyond
the function itself. **Verified**: build-verified (client target compiles
cleanly against a real SDL3 build) and by code-level parity with the
already hardware-verified pause-menu chord in the inner loop -- not
independently re-verified on real Steam Deck hardware (no physical
gamepad in this project's sandbox, same caveat as the "round 2" fixes
above).

## Latency instrumentation assumes synced clocks

The host's periodic latency stat (`docs/protocol.md`'s note on
`clientTimestampUs`) is computed as `hostWallClockNow -
clientTimestampUs`. This only produces a meaningful number if the client
and host system clocks are reasonably synchronized (e.g. both running
NTP), which is a reasonable assumption on a home LAN but is not verified
or negotiated by the protocol. On badly-skewed clocks the number will be
wrong (though implausible deltas are excluded from the running average
rather than silently included).

## No CI-verified client build

`.github/workflows/ci.yml` builds and tests `protocol/` and
`host/remote-server/` (no SDL3 needed) and compiles `client/src/net_client.cpp`
standalone, but does not attempt a full SDL3 client build, since no CI
runner image with SDL3 pre-installed has been wired up. A follow-up should
either install SDL3 in CI or add a client-specific job once a known-good
SDL3 version is pinned.

## Things intentionally out of scope for v0.1

Per `SPEC.md` section 21 (explicit non-goals): ROM transfer, cloud saves,
internet play, multiple simultaneous clients, user accounts, remote
desktop/filesystem browsing, Android/Windows/iOS clients, microphone
streaming, camera emulation, rumble, voice chat, spectator mode, artwork
scraping, cheat databases, a custom emulator core, or replacing melonDS's
rendering. These are not bugs or gaps in this implementation -- they are
deliberately not attempted yet.
