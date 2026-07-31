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

## Hardening the client install/uninstall scripts (GitHub issue #11)

Filed after the above shipped, in the same rewritten-with-detailed-
acceptance-criteria style as issue #10: asked to harden and polish the
already-working central-install-directory workflow rather than replace
it. Most of the acceptance criteria were already satisfied by the work
above (single stable install location, exactly-one-shortcut updates in
place, device identity/saved-host state already protected by keeping
`install/` a subdirectory separate from `device_id.txt`/`last_host.txt`,
uninstall already removing only this project's own files). Two concrete
gaps remained and were addressed directly:

- **A failed update could still lose a working install**: both
  `install-steam-shortcut.sh` (repo and packaged) previously deleted the
  old central install *before* copying the new one in -- a failure
  partway through a copy left nothing usable. Now applies the exact same
  stage-then-swap pattern already built for the host's
  `install-host-distrobox.sh` (see the Bazzite section above): new files
  are staged at `install.new`, and only swapped into place (keeping the
  replaced version as a one-generation `install.previous` backup) once
  staging fully succeeds. **Verified**: a real successful install; a
  simulated failure inside `steam_shortcut.py` itself (Steam "running")
  correctly logs the error while leaving already-staged files in place
  (harmless, since the shortcut's `Exe` path never changes between
  versions); and, more usefully, an actual accidental exercise of a
  *different* real failure path (the auto-build step failing when the
  source binary went missing) confirmed the existing install directory's
  contents were byte-for-byte unchanged (compared via checksum)
  afterward, with the failure logged accurately. `uninstall-steam-shortcut.sh`
  now also cleans up `install.new`/`install.previous` alongside `install/`
  itself, verified via the same fake-`$HOME` technique, including that a
  second uninstall run (or one against a machine where nothing was ever
  installed) is a clean no-op.
- **Errors could vanish with nothing to see** if a script run by
  double-clicking it in Dolphin has no visible terminal at all --
  install/uninstall now both trap any failure, log a timestamped,
  specific message (including the exact failing command) to
  `~/.config/melonds-remote-client/install.log`, and pop up a graphical
  `kdialog` error box when available (SteamOS Desktop Mode and Bazzite
  are both KDE Plasma, where `kdialog` is a standard component) --
  degrading silently to log-only if `kdialog` isn't present, never
  failing the error-reporting itself. **Verified** the logging path
  directly (no `kdialog` binary exists in this project's sandbox to
  exercise that specific branch, so that part is reviewed/syntax-checked
  only, not run-verified) -- confirmed accurate, correctly-timestamped
  log entries for both a `steam_shortcut.py`-level failure and a
  build-step failure, in both the repo and packaged variants.
- Also documented shortcuts.vdf recovery steps in
  `docs/troubleshooting.md` (restoring from the automatic `.bak-*`
  backup, diagnosing a stuck "Steam appears to be running" false
  positive, and where to look for the new error log).

Still not addressed from issue #11's scope: controller-friendly artwork/
icon metadata for the shortcut (currently blank -- no icon asset exists
anywhere in this repo yet to bundle), and a repeatable/automated test
suite for the install/update/uninstall paths beyond this project's
existing manual fake-`$HOME` verification technique.

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

## Easier host installation and updates on Bazzite (GitHub issue #10)

Filed as "simplify host installation and updates on Bazzite and other
supported Linux systems." Investigating this surfaced a real gap: the
prebuilt release's `host/run-host.sh` auto-installs missing runtime
libraries on a normal Debian/Fedora/Arch system, but on Bazzite (or any
other immutable/rpm-ostree system) that auto-install step correctly
refuses to touch the read-only base image and just prints manual
instructions -- meaning `docs/bazzite-host-setup.md`'s "quickest path,
no terminal needed" claim was actually false for Bazzite specifically,
the one platform this project names as its primary target. There was
also no update story at all: getting a newer version meant manually
redoing the same Distrobox/`rpm-ostree install` steps from scratch every
time.

Added `host/install-host-distrobox.sh` (packaged alongside `run-host.sh`
in every release archive, generated by `scripts/build-release.sh`) to
automate this: detects whether the system is actually immutable at all
(reusing the same `ensure_packages()` check used elsewhere, refusing to
run and pointing at `run-host.sh` instead if not), creates/reuses a
Fedora-based Distrobox container (`melonds-remote-host`), installs the
runtime packages the host needs inside it via `dnf`, and launches melonDS
from there. Syncs the whole `host/` directory into a fixed location
first (`~/.config/melonds-remote/install/`, mirroring the same
central-install-directory pattern already used for the client's Steam
shortcut and its own reasoning -- see the "removal script" and
"install/uninstall...across release downloads" sections above), so
downloading a newer release and re-running this script is the entire
update process: no manual Distrobox commands, no recreating the
container, nothing to redo by hand. `run-host.sh` itself also now
detects the immutable case and points at this new script instead of
just failing on a missing-library error with no explanation.
`docs/bazzite-host-setup.md` documents all of this under "Easier Bazzite
host install and updates."

Deliberately did **not** try to replace `run-host.sh`'s existing runtime
dependency package lists with "true" non-dev runtime-only package names
for apt specifically -- checked directly against a real Ubuntu 24.04
package index while working on this, and found that several of the
obvious literal runtime package names (e.g. `libqt6core6`, `libarchive13`,
`libcurl4`, `libpcap0.8`) no longer exist under those names at all on
current Ubuntu due to the 2024 time_t64 ABI transition (the real
packages are now e.g. `libqt6core6t64`, `libarchive13t64`), while older
Ubuntu/Debian releases don't have the `t64` suffix -- there is no single
correct literal name across supported versions. The existing `-dev`
packages remain the right choice specifically because their dependency
metadata always points at whatever the current runtime package is
actually named, on any given release, without this project needing to
track that; the "cost" is a few extra MB of headers, not a functional
bug. The install-host-distrobox.sh script's own `dnf install` list
reuses this project's already-existing `-devel` build dependency list
for the same reason (not independently verified against a real Fedora
package index the way the apt case above was, since no Fedora/dnf
environment exists in this project's sandbox).

**Not verified**: no rpm-ostree/immutable-filesystem environment and no
`distrobox`/`podman` binary exist in this project's own sandbox (plain
Ubuntu 24.04), so `install-host-distrobox.sh` is build/syntax-checked
(`bash -n`) and carefully reviewed against Distrobox's documented
behavior only -- not run-verified end-to-end on real Bazzite. This
matches the same honest caveat this file and `docs/bazzite-host-setup.md`
already carry for all other Bazzite-specific content in this project.

**Version checking**: also added `check-for-updates.sh` (packaged at the
top level of every release archive, applies to both `host/` and
`client/`) -- reads a `VERSION` file embedded at build time (the actual
published tag, wired through from `.github/workflows/release.yml` via a
`RELEASE_VERSION_TAG` env var into `scripts/build-release.sh`) and
compares it against `https://api.github.com/repos/Crimson3076/DualDeck/releases/latest`.
Deliberately read-only -- reports whether a newer version exists and
its Releases page URL, but never downloads or installs anything
automatically, and is never invoked by `run-host.sh`/`run-client.sh`
themselves, so a slow or unreachable network can never add delay to
actually starting the app. **Verified** live against the real GitHub API
for this repo, exercising all of its fallback paths directly: a stale
local `VERSION` correctly reports the real newer tag with the correct
release URL, a matching `VERSION` reports "up to date," a `VERSION`-less
directory falls back to "unknown" without erroring, and both the
"`curl` isn't installed" and "GitHub unreachable" paths degrade to a
clear one-line message and exit 0 rather than failing loudly.

**Issue #10's scope grew substantially after the above shipped**: the
issue was rewritten (Goal/Scope/Acceptance-criteria/Design-constraint
sections added) after `install-host-distrobox.sh` was already merged,
adding requirements not addressed by that first pass -- notably an
explicit rollback path when an update fails, a dedicated host
uninstaller, and a design constraint that Distrobox/containerization
must not be *mandatory* for ordinary users even though it's fine as a
"supported development path." Flagged to the user rather than guessed
at given the scale and possible direction change (a fully
self-contained/bundled-runtime-libraries binary, closer to how
`client/lib/` already bundles SDL3 for the client, would sidestep
needing Distrobox on Bazzite at all) -- the user's call: keep the
already-working, already-automated Distrobox path rather than pursue a
bundled-binary rebuild, since it's a real, working, zero-typing solution
today and a bundled binary carries its own real risk (bundling the
wrong things -- Mesa/GPU-driver libraries specifically must *not* be
bundled, since they need to match the host's actual driver stack -- with
no real Bazzite hardware in this project's sandbox to verify that
distinction against).

Addressed the concrete remaining gaps instead:

- **Rollback on failed updates**: `install-host-distrobox.sh` no longer
  deletes the working central install before copying the new one in --
  it now stages the new files and re-installs container packages in a
  separate `install.new` location first, and only swaps it into place
  (keeping the replaced version as a one-generation `install.previous`
  backup) once *both* steps succeed. A failure partway through (copy
  error, `dnf` failure, network drop) leaves the previous, still-working
  install completely untouched. **Verified** with a fake `distrobox`
  stub and fake `$HOME`: a successful install populates the central
  directory correctly; a simulated `dnf` failure during a second
  "update" leaves the original install's contents completely unchanged
  (confirmed via a version marker file) with only a harmless leftover
  staging directory (cleaned up automatically on the next attempt); a
  subsequent successful update correctly swaps in the new version and
  produces a `.previous` backup with the prior version's contents.
- **Host uninstaller**: added `host/uninstall-host-distrobox.sh` --
  removes the Distrobox container and the central install directory
  (plus any `.new`/`.previous` leftovers), and is a no-op (not an error)
  if nothing is installed. Never touches ROMs, saves, firmware, or other
  melonDS data, since none of that lives in either of those locations --
  it stays in the normal shared home directory throughout, regardless of
  whether the container or central directory exist. **Verified** with
  the same fake-stub technique: removes an existing container + both
  central directories, and is idempotent on a second run and on a
  never-installed system alike.
- Still not addressed at the time: launching the host itself from Steam
  Big Picture/Gaming Mode, and CI-tested install/upgrade behavior (the
  latter still unaddressed -- this project's CI has no rpm-ostree/
  Distrobox environment to test against, same limitation as everything
  else Bazzite-specific here).

## Host launchable from Steam Big Picture/Gaming Mode (GitHub issue #10, continued)

Addressed the one concrete gap called out above: added
`host/install-steam-shortcut.sh`, `host/uninstall-steam-shortcut.sh`, and
`host/launch-host.sh` (packaged alongside the existing host scripts),
reusing `scripts/lib/steam_shortcut.py` -- the same layout-agnostic
Steam-shortcut helper the client already uses -- bundled as a flat
sibling file inside `host/` this time (along with a flat copy of
`ensure-packages.sh`) rather than shared from a top-level `scripts/`
directory, since `host/`'s own central-install-directory layout
(`~/.config/melonds-remote/install/`, already established by
`install-host-distrobox.sh`) is flat, unlike the client's nested one.

`install-steam-shortcut.sh` registers a **"melonDS Remote Host"** Steam
shortcut whose `Exe` is `launch-host.sh` -- a new single entry point that
picks Distrobox (`install-host-distrobox.sh`) or a direct launch
(`run-host.sh`) depending on whether the system is immutable, so the
same shortcut works on both. Update-in-place and the central install
directory work the same way as the client's shortcut (re-running from a
newer release updates everything without leaving a stale duplicate
shortcut), and `steam_shortcut.py`'s existing AppName-fallback matching
means a shortcut from before this feature existed would be migrated
rather than duplicated (not applicable in practice yet, since no
version of this project shipped a host shortcut before now, but verified
anyway via the same hand-crafted-fixture technique used for the client).

**A real ordering bug was found and fixed while building this**: an
early version of `install-host-distrobox.sh --install-only` (added so
`install-steam-shortcut.sh` can prepare the Distrobox container without
also immediately launching melonDS) moved the file-staging swap to
*before* the `dnf install` step, as part of adding a "skip re-copying if
already running from the central directory" fast path for repeated
Steam-shortcut launches. That reordering meant a failed `dnf install`
during an update would leave the *new, not-yet-verified* files already
active -- exactly the bug `install-host-distrobox.sh`'s original design
(swap only after the package install succeeds) was built to prevent.
Caught by the same fake-`distrobox`/fake-`dnf`/fake-`$HOME` verification
technique already used for the original rollback-safety work: a version-
marker file survived a real successful install, then *did not* survive
a simulated `dnf` failure on the following update (a real regression),
which was traced back to the reordering and fixed by restoring the
original "stage now, swap only after the package install succeeds"
order, while keeping the fast-path skip for the specific case of the
directory already being the central install (nothing to stage or swap
there regardless of dnf's outcome). Also required removing a second,
related mistake: `install-steam-shortcut.sh` had its own separate,
unconditional file-copy-then-swap for the *files* alone, done before
ever calling `install-host-distrobox.sh --install-only` for the
*packages* -- which had exactly the same premature-activation problem.
Fixed by having `install-steam-shortcut.sh` delegate entirely to
`install-host-distrobox.sh --install-only` on immutable systems (that
script alone now owns the whole stage-then-verify-then-swap sequence),
and only doing its own simple copy-then-swap on regular systems, where
there's no separate package-install step to gate on in the first place.

**Verified** end-to-end with fake `distrobox`/`dnf`/`$HOME` stubs, on
both a simulated immutable system and a simulated regular one: fresh
install (stages files, creates the Distrobox container, installs
packages, registers the shortcut, does not launch melonDS during
install), launch via the shortcut's `Exe` (skips the redundant re-copy,
re-verifies packages, launches melonDS with the right arguments and
`MELONDS_REMOTE_ENABLE=1`), a real update (new files correctly swapped
in, old version correctly preserved as `install.previous`), the dnf-
failure-during-update regression above (caught, then re-verified fixed:
previous install's version marker survives intact), a full uninstall
(shortcut removed, Distrobox container removed, central directory
removed, second run is a clean no-op), and `--dry-run` (confirmed to
create nothing at all). **Not verified**: real Distrobox/`dnf` behavior
against an actual Fedora/Bazzite image, and Steam's own real Big
Picture/Gaming Mode UI actually showing and launching the shortcut --
this sandbox has neither.

Still not addressed at the time: CI-tested install/upgrade behavior,
and real Steam Deck/Bazzite hardware verification.

## One consolidated host menu instead of five separate scripts (GitHub issue #10, continued)

User feedback after the Steam-shortcut work above: five host scripts
(`run-host.sh`, `install-host-distrobox.sh`, `uninstall-host-distrobox.sh`,
`install-steam-shortcut.sh`, `uninstall-steam-shortcut.sh`) plus the
`launch-host.sh` dispatcher is real progress but still asks a user to
know which one applies to them. Requested this be "condensed down into
one menu that opens up with an executable."

Added `host/melonds-remote-host.sh` as that one entry point -- a menu
with four choices (Launch now / Add to Steam / Remove from Steam /
Check for updates), using a graphical `kdialog --menu` when available
(SteamOS Desktop Mode and Bazzite are both KDE Plasma) and a plain
numbered terminal prompt otherwise. It's a thin dispatcher, not a
rewrite: each choice just calls the already-tested script that does the
real work (`launch-host.sh`, `install-steam-shortcut.sh`,
`uninstall-steam-shortcut.sh`, `../check-for-updates.sh`) rather than
reimplementing any of that logic -- deliberately, so none of the
rollback-safety/error-visibility work already verified above needed to
be touched or re-risked. The five underlying scripts still exist and
still work standalone (e.g. for scripting), but this is now the only
one documented as the thing to actually double-click.

**A real bug was caught while testing this**: the terminal (no-`kdialog`)
fallback's menu-display text (`echo "1) Launch melonDS now"` etc.) was
being printed to stdout, the same stream the calling code captures via
`action="$(choose_action)"` to read back which option was picked --
so the captured `action` variable ended up containing the entire
displayed menu text with the real selection tacked onto the end,
breaking every `case` match silently (every choice fell through to the
default "do nothing" branch, and "Add to Steam" through the terminal
prompt did nothing at all). Caught immediately by a first real test run
(chose "Add to Steam" through the terminal-mode path, then checked
whether the central install directory or Steam shortcut actually
appeared -- neither did). Fixed by redirecting the whole menu-display
block to stderr, leaving only the actual selection on stdout.

**Verified** with the same fake-`distrobox`/`dnf`/`$HOME` stubs as
before, plus a fake `kdialog` stub: every menu choice on both a
simulated immutable and a simulated regular system (launch, add to
Steam, remove with confirmation, remove again idempotently, check for
updates against the real GitHub API, exit, and an invalid/empty input
falling through to a safe no-op) via the terminal fallback; the same
"Add to Steam" and Cancel choices via the fake-`kdialog` menu path,
confirming argument formatting (`--menu`/`--msgbox`/`--yesno`) and
return-value handling are both correct. **Not verified**: a real
`kdialog` binary or Steam's actual Big Picture/Gaming Mode UI -- neither
exists in this sandbox.

Still not addressed at the time: real one-click *updating* (as opposed
to just checking) -- see below -- and CI-tested install/upgrade
behavior.

## "Check for updates" now actually offers to update (GitHub issue #10, continued)

User follow-up: "does check for updates also auto install updates? it
should do that for the user." `check-for-updates.sh` itself stayed
exactly as it was (read-only, shared by both host and client, safe to
run unattended or in scripts) -- but `melonds-remote-host.sh`'s "Check
for updates" menu choice now parses its report, and if a newer version
exists, asks for confirmation (`kdialog --yesno` or a terminal `y/N`
prompt) before doing anything further. Confirm and it hands off to a
new `host/apply-update.sh`, which downloads the latest release from a
fixed GitHub Releases URL (`.../releases/latest/download/melonds-remote-linux-x86_64.tar.gz`
-- a stable redirect that always points at whichever release is
currently latest, no API/JSON parsing needed for the download step
itself, only for the version-string comparison `check-for-updates.sh`
already did), extracts it to a temp directory, and hands off *again* --
to that freshly-downloaded release's own `install-steam-shortcut.sh
--force`. That reuse is deliberate: none of the stage-then-swap file
safety or Distrobox/`dnf`-gated activation logic verified earlier in
this file needed to be duplicated or re-risked here -- `apply-update.sh`'s
only real job is fetching and extracting. `--force` is passed through
so the update doesn't silently do nothing just because Steam happens to
be open at the time (the confirmation prompt already covers "are you
sure"); if Steam genuinely was never set up on the machine, the
Steam-shortcut part fails visibly (its own error-trap logs and shows a
dialog) while the file update itself still completes regardless, since
that copy step doesn't depend on Steam at all.

**A real gap was found and fixed while verifying this**: `check-for-updates.sh`
and the archive's top-level `VERSION` file were never being copied into
the central install directory by `install-host-distrobox.sh`/
`install-steam-shortcut.sh` -- only `host/`'s own contents were. That's
invisible as long as a user always re-launches from wherever they most
recently downloaded and extracted an archive, but `melonds-remote-host.sh`
itself *is* one of the files that gets copied into the central
directory (it's a flat sibling inside `host/`), and its "Check for
updates" choice references `../check-for-updates.sh` -- so running that
same menu script a second time, from inside the central directory
instead of a fresh download (e.g. after deleting the original archive,
which the docs already say is safe to do), would fail with a
file-not-found error the first time anyone actually exercised that
path. Fixed by having both installer scripts also copy
`check-for-updates.sh`/`VERSION` to `~/.config/melonds-remote/` (one
level up from `install/`, as stable siblings that survive the
`install`/`install.previous` swap dance), which happens to resolve via
the *same* unmodified `../check-for-updates.sh` reference regardless of
which of the two locations `melonds-remote-host.sh` is currently
running from. Both uninstallers were updated to remove these two
sibling files too, so uninstalling stays complete.

**Verified**: with fake `distrobox`/`dnf`/`$HOME` stubs for the
install/uninstall/rollback mechanics (as before), but the actual
download-and-extract step was exercised against the **real, currently
published GitHub release** (not a fake stub) -- a real `curl` download,
real `tar` extraction, and a real invocation of that release's own
`install-steam-shortcut.sh --force`, including one run that hit a
genuine transient GitHub 503 mid-test (unrelated to this change --
proof `check-for-updates.sh`'s existing "couldn't reach GitHub, exit 0"
degradation path works under a real failure, not just a simulated one)
and a later successful run against the same real endpoint once it
recovered. Confirmed: the temp download directory is always cleaned up
(via an `EXIT` trap, deliberately *not* skipped by an `exec` at the
handoff point, since `exec` replacing the process image would skip
pending traps and leak the temp directory); `check-for-updates.sh`/
`VERSION` correctly resolve via `../check-for-updates.sh` both from the
original archive location and from inside the central install
directory after the fix; both uninstallers correctly remove the two
sibling files alongside everything else, idempotently. **Not verified**:
a real `kdialog` binary showing the actual confirmation dialog, or
letting an update run to completion against a genuine Bazzite/Distrobox
environment -- neither exists in this sandbox.

Still not addressed at the time: the same auto-update treatment for the
client (added below, in the layout restructuring), CI-tested
install/upgrade behavior, and real Steam Deck/Bazzite hardware
verification.

## Release archive restructured into host/client + internal/, client gets its own menu (GitHub issue #10, continued)

User follow-up: "there should be a readme in the main directory for new
users to use. all of these scripts being in the same directory is
confusing even for me. this needs to be majorly streamlined." Even
after the consolidation above, `host/` still showed all ten-odd
supporting scripts flat alongside the one (`melonds-remote-host.sh`) a
user actually needed, and `client/` had no menu at all -- still three
separate visible scripts there (`run-client.sh`,
`install-steam-shortcut.sh`, `uninstall-steam-shortcut.sh`), an
asymmetry with the host side.

**New layout**: `host/` and `client/` each now show only the one
double-click entry point plus the binary it launches; everything else
(the install/uninstall scripts, `ensure-packages.sh`, `steam_shortcut.py`,
the Distrobox path, etc.) moved one level down into `host/internal/`
and `client/internal/`. Both directories are now fully self-contained
(their own `internal/ensure-packages.sh` and `internal/steam_shortcut.py`
copies) -- the archive no longer has a shared top-level `scripts/`
directory at all. `client/melonds-remote-client.sh` is new: the same
four-choice menu (Launch now / Add to Steam / Remove from Steam / Check
for updates) as `host/melonds-remote-host.sh`, including its own
`client/internal/apply-update.sh` (simpler than the host's -- no
Distrobox step to gate on). A real, genuine top-level `README.md`
(`docs/release-readme.md` in the repo, copied into the archive at build
time) replaces relying on `RELEASE_NOTES.md` -- which itself was
trimmed down to just build provenance, since usage instructions now
live in `README.md` instead and were growing stale there (still
mentioning the old flat script names).

**The central install directories were also restructured to match**:
`~/.config/melonds-remote/install/` (host) and
`~/.config/melonds-remote-client/install/` (client) now each mirror
their entire source directory exactly -- entry-point script, binary,
and `internal/` subfolder all in the same shape as the archive itself
-- rather than the client's previous nested `install/client/` +
`install/scripts/lib/` layout. Steam shortcuts now point at
`install/internal/launch-host.sh` / `install/internal/run-client.sh`
respectively. This is a breaking change for anyone who already had a
shortcut from a previous release, but self-heals automatically:
`steam_shortcut.py`'s existing AppName-fallback matching (built for the
exact cross-release-path-change scenario earlier in this file) finds
and rewrites the old entry in place the next time install runs, rather
than leaving a duplicate.

**This required updating essentially every relative path reference** in
the host and client scripts, since each one now lives one directory
level deeper than before. The riskiest part: `install-host-distrobox.sh`
and `install-steam-shortcut.sh` (both host and client) stage their
central-directory copy via `cp -a` from a computed `host_root`/`client_root`
(`cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd`) rather than the
script's own directory, so the copy captures the *whole* parent
directory -- entry point, binary, and internal/ together -- not just
the internal/ subfolder the script itself lives in. The "already
running from the central directory, skip the wasteful re-copy" check
added for the host's Distrobox path in the previous pass was updated
the same way (compares `host_root` against `central_install_dir`,
not `$(pwd)`).

**Verified** with the same fake-`distrobox`/`dnf`/`kdialog`/`$HOME`
technique as every pass before this one, rebuilt against the new nested
paths: for both host and client, on both a simulated immutable and a
simulated regular system -- fresh install (confirmed the central
directory is an exact mirror of the source, including the entry-point
script and binary, not just internal/), the Steam shortcut pointing at
the correct nested `internal/` path, launching via that shortcut path
(including the host's `run-host.sh` correctly finding `../melonDS` and
the client's `run-client.sh` correctly finding `../melonds-remote-client`
and `../lib/` from within `internal/`), a real update, the dnf-failure-
mid-update rollback-safety property (re-verified after the refactor --
a version-marker file survives a simulated `dnf` failure exactly as
before), a full uninstall (shortcut, central directory, and Distrobox
container all removed, second run idempotent), the AppName-fallback
migration of a stale pre-restructuring shortcut entry, and `--dry-run`
(confirmed to create nothing, reporting the correct new nested `Exe`
path). **Not verified**: an actual self-update from a real *previous*
published release into this new layout (the currently published
release predates this restructuring, so `apply-update.sh` was only
exercised against that older layout during this pass, not against
itself) -- this will only be meaningfully verified once this version is
actually published and a later version updates over it. Also not
verified: real Distrobox/Steam UI, and real Bazzite/Steam Deck
hardware, same as every other Bazzite-specific item in this file.

Still not addressed at the time: CI-tested install/upgrade behavior,
real Steam Deck/Bazzite hardware verification, and (as it turned out)
updating *from* a real pre-restructuring release, below.

## Compatibility shim for updating from a pre-restructuring release (GitHub issue #10, continued)

**Real-hardware report, the first concrete confirmation of the
"not verified: an actual self-update from a real previous published
release" gap flagged immediately above**: a user running an
already-installed pre-restructuring release picked "Check for updates"
and got `Updating failed: "${extracted_dir}/host/install-steam-shortcut.sh"
--force (exit code 127)`.

Root cause was exactly the anticipated one: every release before the
`host/internal/` restructuring had `install-steam-shortcut.sh` directly
at `host/`, and that version's own `host/apply-update.sh` hardcodes
that exact path when it downloads and invokes a *newer* release's copy.
Once the restructured release was published, that file no longer
exists there -- it moved to `host/internal/install-steam-shortcut.sh`
-- so the already-installed old `apply-update.sh` (which can't be
changed retroactively; it's already on the user's disk) downloaded the
new release fine and then failed trying to exec a path that no longer
existed (exit 127 = command not found).

Fixed by adding `host/install-steam-shortcut.sh` back as a three-line
compatibility shim in the packaged archive -- `exec`s straight through
to `host/internal/install-steam-shortcut.sh`, forwarding all arguments.
Every *current* code path (`melonds-remote-host.sh`, the current
`apply-update.sh`) already calls `internal/install-steam-shortcut.sh`
directly and never touches this file -- it exists purely so an old,
already-installed `apply-update.sh` downloading *this* release can
still find what it's hardcoded to look for. Safe to delete once no
supported release still depends on it (i.e. once enough time has
passed that no one reasonably still has a pre-restructuring version
installed).

**Verified**: reproduced the exact reported failure first (extracted
just the pre-fix `host/install-steam-shortcut.sh` heredoc and the
`host/internal/` one side by side, confirmed the flat path was
genuinely absent, matching exit 127), then confirmed the fix by
invoking the new shim with the *exact* argument pattern the old
`apply-update.sh` uses (`"${extracted_dir}/host/install-steam-shortcut.sh"
--force`) against a full fake-`distrobox`/`dnf`/`$HOME` install --
completes successfully end to end (container created, packages
"installed", central directory populated with the full new nested
layout, Steam shortcut registered at the correct `internal/launch-host.sh`
path) instead of failing with a missing file. **Not verified**: this
exact scenario against a real previously-installed binary on real
Bazzite hardware (the report that surfaced this was exactly that, but
this fix itself has only been re-verified in this project's own
sandbox) -- the reporting user re-running "Check for updates" against
this fix is the next real confirmation.

Still not addressed: CI-tested install/upgrade behavior, and real Steam
Deck/Bazzite hardware verification. Given these, GitHub issue #10
remains open.

## Client menu chord changed from Start+Select to L3+R3 (Steam Deck system-chord conflict)

**User report**: the in-app menu's Start+Select chord "causes conflicts
with the Steam Deck's built-in 'action set gamepad/desktop' function."
Steam Deck's Steam Input layer reserves Start+Select (held together) by
default as the chord that switches a game's active controller "action
set" (e.g. from a "Gamepad" set to a "Desktop" set), so on real hardware
that hold could get intercepted by Steam Input itself before this app's
own polling loop ever saw a sustained two-button press -- consistent
with, and a plausible explanation for, the intermittent single-button-
opens-the-menu reports from earlier real-hardware testing (see "Real-usage
bug fixes, round 2" above), which were worked around at the time but
never fully explained.

Fixed by moving the chord to **L3+R3** (both analog stick clicks held
together) in `client/src/main.cpp` -- both chord-check sites
(`discoverAndSelectHost()`'s screen and `main()`'s inner loop), the
`kMenuComboHint` on-screen text, and the pause menu's own "...TO CLOSE"
hint. L3/R3 were chosen because they're the only standard gamepad
buttons not already mapped to a DS button in `kButtonMappings` (the DS
has no analog sticks, so nothing gameplay-relevant is lost), and because
they're not a Steam Deck system-reserved chord the way Start+Select is.
The same ~350ms deliberate-hold requirement (`kMenuChordHoldUs`) was kept
unchanged.

A `host/install-steam-shortcut.sh`-style compatibility shim isn't
applicable here -- this is client-side input-handling logic inside the
binary itself, not a file path an old installed script hardcodes, so a
normal client update (see the auto-update work directly below) is
sufficient for existing installs to pick this up.

**Verified**: client rebuilds cleanly against the sandbox's built SDL3
(`/tmp/sdl3-install`); `grep` confirms no remaining functional references
to `SDL_GAMEPAD_BUTTON_START`/`SDL_GAMEPAD_BUTTON_BACK` in either chord
check, and both now check `SDL_GAMEPAD_BUTTON_LEFT_STICK`/
`SDL_GAMEPAD_BUTTON_RIGHT_STICK`. **Not verified**: real Steam Deck
hardware -- specifically, whether L3+R3 is itself free of any other
Steam Input default reservation. This project has no access to real
Steam Deck hardware to confirm directly; if a future report finds L3+R3
also conflicts with something, the fix is the same shape (pick another
unmapped combination and update this section).

## Client auto-update on launch (no manual step required)

User follow-up, same message as the chord change above: "I would also
like for the client to be able to update via the menu, or better yet,
check for an update on launch and automatically apply it when there's
an update detected." The "Check for updates" menu choice already existed
(see "'Check for updates' now actually offers to update" above, which
added it for the host and, in the same pass, for the client's menu) --
what was missing was the "better yet": checking automatically on launch,
with no menu interaction and no confirmation prompt.

Added directly to `client/internal/run-client.sh`, not just the menu,
specifically because that script -- not `melonds-remote-client.sh` -- is
what `install-steam-shortcut.sh` points the Steam shortcut's `Exe` at, so
it's the one launch path that's genuinely unavoidable regardless of how
the client is started (Steam Gaming Mode, "Launch now" from the menu, or
running it directly). On launch, it calls the existing
`check-for-updates.sh` (unmodified -- still read-only, still capped at a
5s network timeout, still never exits non-zero on failure) and, only if
its report contains "update available:", immediately hands off to
`apply-update.sh` with no confirmation -- reusing the exact same
download-and-install path the menu's "Check for updates" already used
and had verified. On success it `exec`s the freshly-installed copy of
`run-client.sh` so the rest of the launch (library check, the binary
itself) runs the new version; on any failure at any step (missing
`check-for-updates.sh` on an old install, offline, GitHub down, a failed
download) it logs a line to stderr and falls through to launching
whatever's already installed, exactly like a normal launch with no
update available -- consistent with `check-for-updates.sh`'s own
existing "never blocks or fails a real launch" principle, just now
applied one layer up.

**A second real gap was found and fixed while wiring this up**: unlike
the host's installers, the client's `install-steam-shortcut.sh` was
never copying `check-for-updates.sh`/`VERSION` into the central install
directory as siblings of `install/` -- the exact bug already fixed for
the host in "'Check for updates' now actually offers to update" above,
just never carried over to the client side at the time. That meant both
the pre-existing client "Check for updates" menu choice *and* this new
launch-time check would have failed with a file-not-found the first time
either ran from inside the central directory (e.g. after deleting the
original downloaded archive) rather than from a fresh extraction. Fixed
the same way as the host: both files are now copied as siblings of
`install/` by `install-steam-shortcut.sh`, and removed by
`uninstall-steam-shortcut.sh`.

**Known tradeoff, stated honestly**: `apply-update.sh`'s download is
capped at `curl --max-time 180`, and that full 3 minutes is now
reachable from a completely ordinary launch (not just an explicit,
opted-in "Check for updates" click) whenever an update genuinely exists
and the connection is slow. There is currently no progress indication
during this beyond the stderr lines above, which Steam Gaming Mode has
no visible terminal for -- the screen just doesn't render anything until
the relaunch happens. A future pass could show this on the client's own
window (it already has a bitmap-font renderer for the discovery/
connecting screens) rather than only on stderr.

**Also inherited from the "Check for updates" menu choice, and not
new to this change**: `apply-update.sh` hands off to
`install-steam-shortcut.sh --force`, which unconditionally adds/updates
a Steam shortcut entry as a side effect of installing the new files
(that script's only job is "make the Steam shortcut point at current
files", so there's no separate "just update the files" mode). For
someone who launched via the Steam shortcut in the first place this is
a harmless no-op refresh of an entry that already existed. For someone
who runs `run-client.sh` directly and had never used "Add to Steam" at
all, an auto-update happening to also silently create one is a genuine,
if minor, surprising side effect -- the same tradeoff the host already
carries for its own auto-update, just reachable now without an explicit
click.

**Verified**: `bash -n` on every generated script in a full local
packaging run (fake melonDS/client binaries, real `ldd`/`tar`, real
`steam_shortcut.py` against a fake Steam userdata directory) confirms
no syntax errors; a real install run against that fake `$HOME` confirms
`check-for-updates.sh`/`VERSION` now land as central-directory siblings
and a real `shortcuts.vdf` entry is created. The launch-time logic
itself was exercised directly with stub `check-for-updates.sh`/
`apply-update.sh` scripts covering all four reachable paths: no update
available (binary launches normally, `apply-update.sh` never invoked);
update available and applied successfully (re-execs into a freshly
"installed" `run-client.sh`, confirmed by that script announcing itself
distinctly, with the original arguments preserved across the re-exec);
update available but `apply-update.sh` fails (falls through and still
launches the current version rather than exiting); and
`check-for-updates.sh` missing entirely, simulating an old install from
before this fix (launches normally, no error). The uninstaller was
separately confirmed to remove the two new sibling files. **Not
verified**: an actual launch-time update against the real, currently
published GitHub release triggering a real ~180s download (the stub
tests above cover the branching logic; a real end-to-end run is left to
whenever the next real release supersedes this one, same as the host
auto-update's own "not verified" note above), and real Steam Deck/Gaming
Mode hardware.

## Client-host version check (AppVersionMismatch)

Third part of the same user message as the two entries above: "I would
like to have some sort of client host version check verification, so
that the client does not try to connect using an older version." The
existing `kProtocolVersion` check (see `docs/protocol.md`) only guards
*wire-format* compatibility -- two different releases can share a wire
format while having meaningfully different features/fixes, and nothing
previously stopped a stale client from connecting to a newer host (or
vice versa) as long as the bytes on the wire still parsed.

Added a second, orthogonal check: `HelloPayload`/`HelloAckPayload` both
gained an `appVersion` string field (the release version, e.g.
"v0.1.24"), and a new `HelloRejectReason::AppVersionMismatch`.
`kProtocolVersion` bumped 3->4 for the wire-format change this required
(protocol.h/.cpp, unit-tested in `protocol/tests/test_handshake.cpp`).
`net_server.cpp` rejects a handshake with `AppVersionMismatch` -- checked
before authentication/device-approval, so a stale client never learns
whether its stale credentials would have worked -- whenever both the
host's `NetServerConfig::appVersion` and the connecting client's
`Hello.appVersion` are non-empty and don't match exactly; either side
being empty (a from-source dev build with no wrapper script setting it)
skips the check entirely, so this is opt-in hardening for packaged
releases rather than a new requirement for development. The host always
echoes its own version back in `HelloAckPayload.appVersion` regardless
of accept/reject, so the client can show e.g. "host is on vX, you're on
vY" -- both `client/src/main.cpp`'s setup-wizard connect step and the
main connection loop's status line do this now.

Threaded through via the same `MELONDS_REMOTE_*` env-var pattern as
`MELONDS_REMOTE_AUTH_TOKEN`: `run-host.sh`/`install-host-distrobox.sh`
export `MELONDS_REMOTE_VERSION` (read from the archive's `VERSION` file,
one level up from `host_root`/`central_install_dir`, same lookup
`check-for-updates.sh` already uses) before launching melonDS;
`run-client.sh` does the same for the client binary. The standalone host
prototype (`host/remote-server/src/main.cpp`) gained a matching
`--app-version` CLI flag for testing without needing melonDS at all.

**The melonDS patch had to be regenerated** (not just this repo's own
`protocol/`/`host/remote-server/` code) since
`host/melonds-patches/0001-remote-server-integration.patch` vendors full
copies of `protocol.h/.cpp` and `net_server.h/.cpp` as embedded "new
file" diff content -- see that file's own history for why. Regeneration
procedure used here (not previously written down anywhere in this repo,
so noting it for next time): clone melonDS fresh at the pinned commit,
`git apply` the *old* patch, overwrite the vendored copies of the
now-changed repo files with the current repo versions (they're literal
copies, so this is a straight `cp`), hand-edit the melonDS-only glue
(`RemoteServerBridge.h/.cpp` gained an `appVersion` constructor
parameter forwarded to `NetServerConfig::appVersion`; `EmuInstance.cpp`'s
`startRemoteServer()` passes
`envOr("MELONDS_REMOTE_VERSION", "MelonDSRemote.Version")` for it,
matching the existing `AuthToken` wiring exactly; `Config.cpp` gained a
`MelonDSRemote.Version` default-empty-string key), then `git add -A &&
git diff --cached` for a fresh full patch (plain `git diff` without
`add -A` misses "new file" hunks entirely, since `git apply`-created
files are untracked until staged -- a real mistake made and caught while
doing this). Diffed the regenerated patch against the original
file-by-file (Python, splitting on `diff --git` lines) to confirm only
the 8 intentionally-changed files actually differ and all 25 others are
byte-identical (module a `core.abbrev`-driven blob-hash-length cosmetic
difference in the `index` lines, normalized away with `git -c
core.abbrev=7`) -- i.e. this regeneration didn't accidentally reintroduce
or lose anything from earlier passes.

**Verified**: the regenerated patch applies cleanly (`git apply`, no
fuzz/reject) to a fresh, pristine clone of melonDS at the pinned commit
(not just as an incremental edit on top of an already-patched tree, which
would have hidden an apply-order mistake); the full patched melonDS
Release build (Qt6, the same ~15-20 minute build this project always
requires) compiled with no errors or warnings from the changed files.
`protocol/tests/test_handshake.cpp` gained round-trip tests for
`appVersion` on both payloads and a dedicated
`hello_ack_payload_round_trip_app_version_mismatch` test; full `ctest`
run clean. The standalone host prototype and the SDL3 client were both
rebuilt against the new protocol/net_client/net_server code and compile
cleanly. **Not verified**: an actual end-to-end handshake rejection
against the real patched melonDS binary with a genuinely mismatched
client (this sandbox has no windowing/GPU environment to run melonDS
itself, the same limitation noted throughout this file for other
melonDS-specific features) -- the protocol-level round-trip and the
`net_server.cpp` rejection logic are unit- and structurally-verified, but
a live two-process handshake through the real patched binary specifically
is not. Also not verified: real Steam Deck hardware.

**A real regression was found and fixed in a later pass**: this change's
own `ctest` (the C++ protocol/host/client unit and structural tests) was
run and passed at the time, but `tests/smoke_test.py` and
`tests/device_approval_smoke_test.py` -- the two Python end-to-end
scripts CI's `ci.yml` actually runs against the real standalone host
binary -- were never updated to match the new wire format (still sending
protocol version 3 and a `HelloPayload` with no trailing `appVersion`
string), so every host handshake in both scripts was silently rejected
with `ProtocolVersionMismatch`/a malformed-payload parse failure from
the moment this change merged. Caught only because a later, unrelated
task happened to run `smoke_test.py` directly and got an assertion
failure instead of the expected pass -- CI itself would have been
showing this as red the whole time otherwise. Fixed by bumping both
scripts' `VERSION` to 4 and adding the trailing `appVersion` string to
`hello_payload()` (empty by default, so the check stays skipped for
every existing test case exactly as before); `smoke_test.py` also
gained a dedicated `--app-version`-on-the-host-process test case
(mismatched version rejected with `AppVersionMismatch`, matching
version accepted, host's own version correctly echoed back in
`HelloAck` even on the rejection) since the server-process fixture was
already right there to exercise it cheaply. Both scripts re-verified
passing end to end against the real binary.

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

Narrower than it used to be, though: `client/tests/melonds_remote_net_client_tests`
(GitHub issue #4 Phase E, added alongside `NetClient::controlReceiveLoop()`
-- see that phase's entry above) is a real end-to-end test suite for
`net_client.cpp`/`net_client.h` against a real `host::NetServer`, and it
needs no SDL3 at all -- `add_subdirectory(client/tests)` in the top-level
`CMakeLists.txt` is gated only by `DUALDECK_BUILD_TESTS`, not
`DUALDECK_BUILD_CLIENT`, so it already builds and runs as part of
`ci.yml`'s existing `protocol-and-host` job (confirmed by reproducing
that job's exact `cmake`/`-Werror` configuration locally) with no
workflow changes needed. `main.cpp`'s SDL3-dependent rendering/input/menu
code -- including the new host-control screen itself -- is still
untested by CI and still needs a real display or run to exercise, as
this phase's own real-binary verification (`SDL_VIDEODRIVER=dummy`, done
by hand, not in CI) had to do.

## First-run setup wizard (GitHub issue #19)

Added a linear, gamepad-navigable setup wizard to the client
(`client/src/main.cpp`'s `runSetupWizard()` and its step functions), run
automatically once on first launch and reachable again afterward via a
new "SETUP WIZARD" entry in the existing Start+Select pause menu.
Completion is tracked by a plain marker file
(`~/.config/melonds-remote-client/setup_complete.txt`, see
`client/src/wizard_state.h`/`.cpp`) so it only runs unprompted once.
Skipped entirely (same as "CHANGE HOST") when an explicit `--host`/
positional address is given, since that means scripted/CI use, not an
interactive first-time user.

Steps, in order, each with Back navigation to the previous step (except
Welcome, where B/Escape exits the wizard instead) and window-close
(`SDL_EVENT_QUIT`) always exiting immediately from any step:

1. **Welcome** -- what the wizard does and roughly how long it takes.
2. **Choose connection method** -- auto-discover on the LAN (reuses the
   existing `discoverAndSelectHost()` screen verbatim) or enter a host
   address manually.
3. **Manual entry** (if chosen) -- a text-input screen using
   `SDL_StartTextInput()`/`SDL_EVENT_TEXT_INPUT`. Assumes the host's
   default ports (8760/8761/8762); there is no port-entry field, since
   supporting non-default ports without discovery would need a second
   input field for comparatively little benefit -- a user with
   non-default ports can just use auto-discovery instead. Carries the
   same known limitation as the pairing-code screen this project removed
   earlier: Steam Input doesn't reliably bring up a virtual keyboard in
   Gaming Mode, so this screen is only really usable with a physical or
   Bluetooth keyboard attached. Not solved here, just documented.
4. **Connect and wait for approval** -- runs the connection attempt on a
   background thread (mirroring `main()`'s own reconnect-thread pattern)
   with backoff retry, so the UI never freezes on a blocking `connect()`
   call. Distinguishes every `HelloRejectReason` value in the status
   text (protocol version mismatch, authentication failed, host busy,
   approval required, or plain unreachable) instead of collapsing them
   to one generic message, since a first-time user has no other way to
   learn why a connection attempt is failing. This step also exposed and
   fixed a real bug: `NetClient::connect()` (`client/src/net_client.cpp`)
   never reset `lastRejectReason_` at the start of an attempt, so a
   caller could see a stale reason left over from a previous attempt's
   real rejection if the current attempt failed earlier (e.g. the host
   simply being unreachable, which fails before any HelloAck is ever
   parsed). Fixed by resetting it to `None` under `handshakeResultMutex_`
   at the top of `connect()`, verified by a clean rebuild with warnings
   enabled.
5. **Video test** -- shows live frames the same way the normal connected
   view does, with a "no video yet -- open a ROM on the host" hint if
   the screen stays blank, and only allows advancing once at least one
   frame has actually arrived.
6. **Controller test** -- a 12-button grid (all `DSButton_*` values)
   that highlights each button live and auto-advances about a second
   after every one has been pressed at least once. Deliberately
   keyboard-only for going back (Escape) -- gamepad South/East are
   themselves under test here (A and B), so treating either as a menu
   action would make it impossible to confirm they report correctly.
7. **Touch test** -- four corner targets over the aspect-fit DS
   rectangle, hit-tested by Euclidean distance in window space, using
   `mapPointToDSCoords()` only to confirm a touch fell inside the valid
   DS rectangle at all. Auto-advances about a second after all four are
   hit.
8. **Done** -- marks setup complete and returns to the normal discovery
   screen.

All wizard screen text was written using only the bitmap font's
supported characters (space, `0`-`9`, `A`-`Z`, `.`, `-`, `:` --
see `client/src/bitmap_font.cpp`), avoiding commas, parentheses,
apostrophes, and question marks, which silently render as blank glyphs.

**Verified**: a full warnings-enabled rebuild (`-Wall -Wextra -Wpedantic
-Wconversion -Wshadow`, zero warnings) against the real SDL3 build
already used elsewhere in this project; an Xvfb smoke test launching the
real binary and using `xdotool` to drive it through Welcome -> Choose
Method -> Manual Entry (typing `127.0.0.1`) -> Connect (observed three
real `connect() -> Connection refused` attempts against the unreachable
address, confirming the background retry thread runs) -> Back through
every step in sequence all the way to Welcome -> Exit, confirming clean
process shutdown (exit code 0, no hung threads) the whole way; and a
separate run with a pre-created `setup_complete.txt` confirming the
wizard is correctly skipped on a subsequent launch. `ctest` re-run to
confirm no regressions elsewhere.

**Not addressed, and not possible from this sandbox or without further,
separate work**:

- **No real Steam Deck LCD/OLED hardware testing.** This sandbox has no
  such hardware; issue #19's acceptance criteria explicitly ask for this
  before it can be considered complete. The wizard follows the same
  input-handling conventions (gated Escape-key handling, deliberate
  chord holds, etc.) already validated on real hardware for the rest of
  this client in earlier work, but the wizard's own screens have only
  been exercised via Xvfb + `xdotool` and manual code review here.
- **No audio/microphone test step.** The setup wizard (issue #19) has no
  "test your microphone" screen -- picking a device and confirming it
  works happens later, in the in-app Settings menu's own level meter
  (see the microphone section below, GitHub issue #2), not during first
  setup.
- **No host-side changes** (approve/deny dialog improvements, a
  server-state display, or a test-pattern mode) -- issue #19 asks for
  these too, but they require patching real melonDS Qt source, which is
  a separate, much larger undertaking than the client-side wizard and
  was left out of this pass.
- **Cannot distinguish "denied" from "still pending" approval.** Per
  `docs/protocol.md`, the wire protocol has no distinct signal for a
  denied device -- a denied client just stays `ApprovalRequired` forever
  from the client's point of view. The wizard's Connect step is honest
  about this (it only ever says "waiting for approval"), rather than
  guessing or fabricating a "denied" state the protocol can't actually
  report. Fixing this would require a protocol change.
- No end-to-end run against a real melonDS host was possible in this
  sandbox (no windowing/GPU environment to run melonDS itself here);
  the Connect/Video/Controller/Touch steps were verified structurally
  (compiles, runs, navigates, threads join cleanly) rather than against
  a live host actually streaming frames.

Given the above, GitHub issue #19 is left open rather than closed --
this is a substantial but partial pass at its scope, not a complete
implementation of its acceptance criteria.

## Host-picker input unresponsiveness (GitHub issue #21, reopened)

A first pass at issue #21 (see the two sections above, "Add immediate
client connection feedback") added a "CONNECTING TO..." screen for the
wait *after* a host is selected, but the issue was reopened with a more
specific report: "sometimes when using A to select a host, it does
nothing, either frozen or does not respond." That's a different,
earlier point in the flow -- the host-picker screen itself, before
selection.

Root cause: `discoverAndSelectHost()`'s loop called `discoverHosts()`
(a blocking UDP scan, `kDiscoveryScanMs` = 1.2s per call) **inline**,
once per loop iteration, for as long as the picker was shown. Every
single rescan -- not just the first one -- blocked `SDL_PollEvent()`
entirely for up to 1.2s, so a button press landing in that window sat
unprocessed until the current scan happened to finish. The existing
code comment even described this as intentional ("how often does the
searching screen get a chance to notice SDL_EVENT_QUIT"), without
recognizing it also gated every other input, not just quit.

Fixed by moving the scan onto its own background thread
(`client/src/main.cpp`): the thread does nothing but call
`discoverHosts()` in a loop and publish results into a mutex-guarded
`latestScan` vector; the render/input loop never blocks on it, just
copies out whatever's latest each frame (same pattern as
`NetClient::getLatestFrame()`) and keeps polling events and rendering
continuously. `discoverHosts()` (`client/src/discovery_client.h/.cpp`)
gained an optional `cancel` parameter so the background thread's
`select()` waits are capped at `kCancelPollMs` (100ms) instead of one
uninterruptible wait for the full remaining budget -- without this, a
thread-shutdown `join()` (e.g. when the user actually does select a
host, or backs out) could itself block for up to 1.2s waiting for the
in-flight scan to finish before the function could return, which would
have just moved the same class of delay to a different, though smaller
and one-time, point in the flow.

**A related regression was found and fixed during verification, not
present in the initial rewrite**: removing the accidental 1.2s-per-frame
throttling this bug had been providing meant the loop, once no longer
gated by anything, polled events and re-rendered as fast as the
platform would allow -- measured at a full CPU core pinned near 100%
under this sandbox's Xvfb/software rendering. Added an explicit
`kPickerFrameIntervalMs` (16ms, ~60Hz) `SDL_Delay()` at the end of each
loop iteration (including the menu-active branch, which already had
this same unthrottled-loop shape from before this fix and was fixed the
same way while already in this function). A picker/menu screen has no
low-latency requirement of its own, so this cap is unnoticeable while
eliminating the CPU burn.

**Verified**: full host/protocol/client build with
`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`, clean;
`ctest` passes. Built both the fixed code and the pre-fix baseline
(via a git worktree at the previous commit) side by side and compared
them directly under Xvfb + a real standalone host prototype instance
answering real LAN discovery broadcasts: closing the client's window
(which exercises the exact same blocked-`SDL_PollEvent`-loop mechanism
as a picker button press, `SDL_EVENT_QUIT` going through the same inline
`discoverHosts()` call) **did not get honored at all within 3 seconds**
on the pre-fix baseline (had to be force-killed) -- directly reproducing
the reported "frozen/does not respond" symptom -- versus **34-35ms**
consistently on the fixed build, run twice. Confirmed the CPU-usage
regression was fixed by comparing instantaneous per-process CPU
consumption (via `/proc/<pid>/stat` tick deltas over fixed windows, not
`ps`'s cumulative-since-start average, which is misleading for a
short-lived comparison) before and after adding the frame-rate cap.
**Not verified**: real Steam Deck hardware, and a full press-a-button-
during-an-active-scan trace with real gamepad input specifically (LAN
broadcast discovery between two processes on the same host inside this
sandbox worked reliably enough to run the host/protocol/client build
against, but driving the actual on-screen host-selection list via
synthetic input under a window-manager-less Xvfb setup was not reliable
enough in this environment to complete that exact trace -- the
window-close reproduction above exercises the identical blocked-event-
loop code path, which is what makes it a faithful stand-in).

## Mouse-click touch (GitHub issue #23)

User request: "Allow the client to click on the screen using mouse
inputs to register touchscreen taps, such as using the touchpads on the
steamdeck, so users have the option of using both touchscreen and
touchpad." Steam Input's default "Trackpad" binding template maps a
Deck trackpad to mouse motion/clicks, not touch/finger events, so
without this the client's existing touch handling (finger events only)
had no way to receive that input at all.

Added `SDL_EVENT_MOUSE_BUTTON_DOWN`/`_MOTION`/`_UP` handling in both
`client/src/main.cpp`'s main connected loop and the setup wizard's touch
test, mirroring the existing finger-event handling exactly: left
click/drag maps through the same `mapPointToDSCoords`/
`computeAspectFitRect` used for real touch, ignores clicks/drags outside
the rendered DS rectangle, and a plain mouse-motion event with no button
held is *not* treated as touch movement (unlike finger motion, which a
touchscreen only ever generates while actually being touched -- a mouse
generates motion events continuously regardless of button state, so
this needed an explicit gate a finger-only implementation didn't). Touch
and mouse inputs are tracked as independent sources (a new
`mouseTouchDown`/`mouseDown` flag alongside the existing
`activeFingerId`), so releasing one doesn't clear a touch still held by
the other, and mouse events synthesized *from* a real touch
(`which == SDL_TOUCH_MOUSEID`, a platform convenience some backends
provide so mouse-only UI still works via touch) are filtered out to
avoid double-handling the same physical touch as two separate input
sources.

**Verified**: full build with
`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`, clean;
`ctest` passes (unaffected -- no protocol/wire changes, this is
client-local input handling only). End-to-end, driven for real: a
minimal Python fake host (handshake + a UDP listener decoding real
`ControllerState` packets) plus the actual client binary running under
Xvfb, with `xdotool`-injected mouse events (this needed
`SDL_MOUSE_FOCUS_CLICKTHROUGH=1` and un-windowed/global click injection
to actually reach the app in this window-manager-less sandbox -- notably,
this succeeded here where an earlier attempt at synthetic keyboard/mouse
input for the issue #21 investigation above did not, so that gap may be
more about the specific injection technique used there than a hard
sandbox limitation). Confirmed all three cases: a click inside the DS
rectangle registers `touchActive=1` at the exact expected DS coordinate
(clicking the window's center produced `touchX=128, touchY=96`, the
precise center of the 256x192 DS touch range) and a subsequent drag
updates position before release clears it; mouse motion with no button
held produces no touch events at all; a click outside the DS rectangle
is ignored, matching out-of-bounds finger-touch behavior. **Not
verified**: real Steam Deck hardware, and specifically a real trackpad
configured via Steam Input's "Trackpad" binding template (this sandbox
has no Steam Input to exercise -- the verification above used a real X11
mouse device, which is the same SDL event path a Steam Input-emulated
mouse would produce, but the actual trackpad-to-mouse mapping step
itself is unverified).

## Client-side exit-emulation menu (GitHub issue #25)

User request: "Client should have a secondary 'exit emulation' button, so
that it closes on the host device. when pressing the button, it should
prompt the user to either exit the current ROM, or exit the entire
application." The existing L3+R3 pause menu only offered `RESUME`,
`CHANGE HOST`, `SETUP WIZARD`, and a client-local `EXIT` (which just
closes the client app, leaving the host's melonDS running) -- there was
no way to affect the *host* from the Deck at all.

Added a new `EXIT EMULATION` entry to that menu which opens a submenu
(`EXIT ROM` / `EXIT MELONDS ENTIRELY` / `CANCEL`) rather than acting
immediately, matching the issue's explicit request for a confirmation
prompt. `protocol.h` gained `EmulatorAction_QuitApplication` (bit 8) as a
new value within the existing `emulatorActions` bitmask field -- since
this only adds a bitmask value and doesn't change wire *layout*, it did
not need a `kProtocolVersion` bump (unlike a new field, e.g.
`HelloPayload::appVersion` from earlier work). The pre-existing but
previously-unused `EmulatorAction_QuitSession` (bit 7) was repurposed/
documented as "eject the current ROM."

Picking either option sends the corresponding action bit, resent for
`kPendingEmulatorActionUs` (250ms, roughly 28 packets at the ~120Hz
`ControllerState` send rate) rather than in a single packet, matching the
existing resend-for-reliability convention used elsewhere in the client
for one-shot actions over lossy UDP. On the host side,
`EmuInstance::inputProcess()` (`EmuInstanceInput.cpp`, in the melonDS
patch) can't just treat "bit is set this frame" as the trigger given that
resend window -- it would fire repeatedly for one menu confirmation -- so
it edge-detects against a new persistent `lastRemoteEmulatorActions`
member (`EmuInstance.h`) and only acts on the rising edge. melonDS has no
native hotkey (`HK_*`) equivalent for "eject cart" or "quit app" -- only
Qt menu-action slots (`MainWindow::onEjectCart()`/`onQuit()`, both
private) -- so unlike the other emulator actions here (which ride
`hotkeyMask`), these two are invoked directly via
`QMetaObject::invokeMethod(mainWindow, "...", Qt::QueuedConnection)` from
the emulation thread, the same cross-thread marshaling pattern
`EmuInstance.cpp`'s `startRemoteServer()` already uses for its
pending-approval dialog and screen-layout-change callback. Qt's
meta-object system exposes slots for invocation by name regardless of
C++ access control, so this needed no changes to `Window.h`'s access
specifiers.

**Verified**: protocol, host, and client all build clean with
`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`; `ctest` passes.
The regenerated melonDS patch applies cleanly to a fresh pristine clone
of the base commit and a full Release build of the patched melonDS
succeeds. End-to-end, driven for real: a minimal Python fake host (same
technique as the issue #21/#23 entries above) plus the actual client
binary running under Xvfb, with `xdotool`-injected keyboard input
navigating Escape -> Down -> Enter -> Enter through the real menu/submenu
UI. Confirmed all three submenu outcomes by decoding the real
`ControllerState` packets the client sent: picking `EXIT ROM` sends
`emulatorActions=0x0080` (`EmulatorAction_QuitSession`) for exactly the
expected resend window, then correctly stops (returns to `0x0000`);
picking `EXIT MELONDS ENTIRELY` likewise sends `0x0100`
(`EmulatorAction_QuitApplication`) and stops; picking `CANCEL` returns to
the main menu (confirmed via screenshot) without ever sending either bit.
**Not verified**: real Steam Deck hardware, and the deepest host-side
behavior specifically -- that `onEjectCart()`/`onQuit()` firing actually
ejects the cart/closes melonDS's Qt window when driven by a real client
connected to the real patched melonDS binary (verified only that the
patched binary builds and links correctly, and that
`EmuInstanceInput.cpp`'s edge-detection/invokeMethod logic itself is
correct C++ by inspection and compilation, not by observing melonDS's
actual UI react to it in this sandbox, which has no way to load a ROM
and drive a full Qt GUI end-to-end).

## Microphone support (GitHub issue #2)

User request: "Add optional Nintendo DS microphone support by capturing
audio from a microphone connected to the client device and forwarding it
to the host's native melonDS microphone input," with device selection
and a live input-level meter under client Settings.

**Protocol** (bumped to v5, a wire-layout change): `HelloAckPayload`
gained `micSupported` (host capability, checked before the client ever
opens a capture device or sends anything); `DiscoveryResponsePayload`
gained `audioPort` (default 8765 -- distinct from the pre-existing
`ManagementPort` default of 8764 from the Decky-plugin work, an actual
port collision caught and fixed before this ever shipped). A new
`MicAudioFrame` packet type carries a fixed-format payload: mono 16-bit
PCM at a fixed `kMicAudioSampleRate` (48 kHz, matching melonDS's own
local-mic capture rate), `kMicAudioSamplesPerPacket` (480, i.e. 10ms)
samples per packet -- raw, uncompressed, no forward-error-correction, so
a lost UDP packet is an audible ~10ms gap rather than a corrupted
decode. Capability negotiation and the packet itself are entirely
independent: a host can advertise `micSupported=false` (e.g.
`--no-mic`), and a client with no microphone at all still connects and
works exactly as before -- silence in either direction is the same as
"no client/feature."

**Host** (`host/remote-server`): a new `IMicAudioSink` interface (mirrors
`IEmulatorInputSink`) is fed by a dedicated `audioLoop()` UDP receive
loop, gated by the same authenticated-source-address check as
`ControllerState`. `LoggingMicAudioSink` (used by the standalone
synthetic host) tracks an RMS level and logs it periodically, useful for
this session's own end-to-end verification without needing melonDS
built at all.

**Client** (`client/src/mic_capture.{h,cpp}`): SDL3's audio-capture API
(`SDL_GetAudioRecordingDevices`/`SDL_OpenAudioDeviceStream`), enumerated
fresh each time the user cycles the device in Settings so a
newly-plugged-in mic shows up without restarting. "SYSTEM DEFAULT" is
always the first, default-selected entry (stored as an empty
`micDeviceName`, not the literal label, so a later SDL enumeration
change can't strand a saved setting). Capture runs continuously for the
whole connected session once the host advertises support -- not just
while the Settings screen happens to be open, since the host keeps the
game running regardless of which screen this client-local menu is
showing. Muting stops packets from being *sent* but leaves capture (and
the level meter) running, so the meter still shows real input while
muted rather than reading as "no signal" -- matching the issue's
distinct "mute" vs. "no microphone" states.

**melonDS integration**: reuses melonDS's own existing local-mic
pipeline (`EmuInstance::micCallback()`/`micResample()`/`micExtBuffer`,
originally built for a physical mic in "External" input mode) rather
than building a parallel one -- remote audio is just another producer
for that same buffer, so it gets melonDS's own time-stretch-to-DS-rate
handling for free. `micReadInput()` overrides the host's local
`Mic.InputType`/push-to-talk configuration only while
`RemoteServerBridge::hasActiveMicAudio()` is true (a client has sent
audio within the last 500ms) -- a liveness check, not just "the
remote-server feature is enabled," so a host operator's own local
Noise/Wav test setup isn't silently replaced by silence just because
the feature exists with no client ever sending audio. The one
documented tradeoff of this reuse: if a host operator *also* manually
enables local External-mode capture from a real device at the same time
a remote client is streaming, both sources share `micExtBuffer` and
interleave -- an unsupported combination this patch doesn't try to
arbitrate between, not a crash or data race (both writers hold the same
`micLock`).

**Verified**: protocol, host, and client all build clean with
`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`; `ctest`
passes, including new protocol tests for `MicAudioFrame` round-trip/
validation and `client_settings` round-trip for the new
`micDeviceName`/`micMuted` fields. `tests/smoke_test.py` sends a
real `MicAudioFrame` over UDP to a real standalone host process and
confirms the decoded RMS level matches the sent samples exactly
(computed by hand: four samples of ±1000/±2000 give RMS ≈0.048,
logged by the host as `level=0.05`), plus a malformed one (declared
sample count not matching the actual payload) that's rejected without
crashing the server. The regenerated melonDS patch applies cleanly to a
fresh pristine clone of the pinned upstream commit and a full Release
build succeeds. End-to-end client verification (real client binary
under Xvfb, `SDL_AUDIO_DRIVER=dummy`, driven via `xdotool` against a
fake host advertising `micSupported=1`): confirmed the Settings screen
renders "MICROPHONE: <device>" and "MIC: ON/MUTED" entries with a live
level meter; cycling the device via the same entry re-opens capture on
the new device without dropping the connection (4693+ packets received
across the switch); muting stops packets arriving at the fake host
within the same session, observed directly. **A real bug was caught by
this verification and fixed**: the client's `SDL_Init()` call was
missing `SDL_INIT_AUDIO` entirely, so every `MicCapture::open()` call
failed silently and no audio was ever captured, despite the rest of the
pipeline (settings UI, protocol, host) working -- this shipped fixed,
not discovered after the fact.

**Not verified**: real Steam Deck hardware, a real physical microphone
(the dummy-driver verification above proves the whole pipeline moves
real-shaped silence correctly end-to-end, including RMS math on the
host side against real non-zero synthetic packets, but never a live
non-silent capture device end-to-end in one run), Steam Input's
trackpad-configured-as-microphone edge cases (not applicable here --
this is about real audio input devices, not touch), and the
local-External-mode-plus-remote-audio interleaving edge case mentioned
above (understood by code inspection, not exercised).

## Unified bootstrap installer (GitHub issue #26, Phase 1 of 4)

User request: a single downloadable installer (`DualDeck-Installer.sh`)
offering Install Client/Host/Both, Repair, and Uninstall, with
checksum-verified downloads, a graphical menu where available, and a
much larger set of structural changes (an XDG-standard
`~/.local/share/dualdeck/` versioned-symlink install layout, a JSON
release manifest with separately-published host/client archives, and
unified launch-time auto-update for both components) laid out across
four explicit phases in the issue itself.

**What Phase 1 implements**: `scripts/DualDeck-Installer.sh`, published
as its own downloadable GitHub Release asset (not bundled inside the
archive it installs -- issue #26's core "one file to download first"
ask). Detects x86_64 Linux and identifies Steam Deck/SteamOS, Bazzite,
or a generic desktop for an informational banner; presents Install
Client / Install Host / Install Both / Repair / Uninstall via kdialog
(SteamOS/Bazzite/any KDE desktop) -> zenity -> a terminal menu fallback
chain, or non-interactive `--client`/`--host`/`--both`/`--repair`/
`--uninstall` flags for scripting. Downloads the release archive and a
new `SHA256SUMS` file (also newly generated by `scripts/build-release.sh`
into every release), verifies the checksum with `sha256sum -c` *before*
extracting anything, then delegates to that archive's own
already-battle-tested `client/internal/install-steam-shortcut.sh` and
`host/internal/install-steam-shortcut.sh` (or their `uninstall-*`
counterparts) -- reusing their existing atomic stage-then-swap file
safety (GitHub issue #11) rather than reimplementing it in a second,
potentially-drifting copy.

**Deliberately deferred to later phases** (this is Phase 1 of 4, not a
complete implementation of the issue): the XDG `~/.local/share/dualdeck/
current -> versions/vX` layout and migration from today's
`~/.config/melonds-remote-client`/`~/.config/melonds-remote` paths
(Phase 1's own remaining scope); the JSON manifest and separately-
published `dualdeck-client-*`/`dualdeck-host-*` archives, replacing
today's single combined `melonds-remote-linux-x86_64.tar.gz` (Phase 2);
making the Host's launch path auto-update the same way the Client's
`run-client.sh` already does, plus Host-side "Auto Update on Launch"/
"Check for Update" settings to match the Client's (Phase 3); in-app
"Restore Previous Version" UI and background update staging during an
active session (Phase 4). Today's existing per-component install/
update scripts are reused as-is, not replaced, in this change --
GitHub issue #20 ("Standardize the DualDeck project name") is also
still open, and a full rename of on-disk paths/binary names is
better done once that's decided than piecemeal here.

**Verified**: `bash -n` and `shellcheck` clean on both
`DualDeck-Installer.sh` and the updated `build-release.sh`. End-to-end
against a local fixture (a fake HTTP server serving a hand-built
`melonds-remote-linux-x86_64.tar.gz` + matching `SHA256SUMS`, with
`DUALDECK_INSTALLER_DOWNLOAD_BASE` overriding the real GitHub URL for
testing, and stub `internal/install-steam-shortcut.sh`/
`uninstall-steam-shortcut.sh` scripts that log their own invocation
instead of touching a real Steam install): `--client`/`--host`/`--both`
each download, verify, extract, and invoke exactly the expected
component script with no extra arguments; `--repair` correctly gates on
an interactive confirmation and passes `--force` (matching how
`apply-update.sh` already uses `--force` for reinstall-like flows, vs.
a fresh install respecting the "Steam is running" safety check
un-forced); `--uninstall` invokes both components' uninstall scripts;
the interactive terminal-menu fallback (no kdialog/zenity available)
correctly maps a numbered choice to the same action; and, critically, a
deliberately corrupted download (content that doesn't match
`SHA256SUMS`) is rejected with a clear error *before* any install
script ever runs -- confirmed via the same call-logging stub showing no
invocation at all in that case.

**Not verified**: real Steam Deck/Bazzite hardware, the actual kdialog/
zenity graphical menu rendering (this sandbox has no display server
with either installed; the terminal-fallback code path was exercised
directly instead, and the kdialog/zenity command construction was
reviewed but not run), and a real download against the actual published
GitHub release (verified against a local fixture server instead, for
speed and to test the checksum-rejection path deliberately, which isn't
practical to trigger against a real upload) -- the next real release
publish is itself the first genuine end-to-end run against the real
GitHub Releases URL.

## Emulator identity model (GitHub issue #28, architecture foundation milestone)

GitHub issue #28 tracks a large architecture change: evolving DualDeck
from a melonDS-specific tool into an emulator-independent platform with
future Nintendo 3DS and Wii U adapters. This entry covers only the first
foundation slice explicitly scoped for this milestone -- shared identity
types and their surfacing in discovery/handshake/UI -- not the full
6-phase plan in the issue.

**What this milestone implements**: `melonds_remote::SystemIdentity`
(`systemId`/`systemName`, e.g. `"nds"`/`"Nintendo DS"`) and
`AdapterIdentity` (`adapterId`/`adapterName`/`adapterVersion`, e.g.
`"melonds"`/`"melonDS"`/melonDS's own version), added to
`DiscoveryResponsePayload` and `HelloAckPayload` (`kProtocolVersion`
5→6). `host::NetServerConfig` carries both with clearly-labeled
synthetic defaults (`"synthetic"`/`"Synthetic Test System"`,
`"synthetic-test"`/`"Synthetic Test Adapter"`), overridable on the
standalone host via `--system-id`/`--system-name`/`--adapter-id`/
`--adapter-name`/`--adapter-version` for testing/fixture purposes. The
melonDS integration (`RemoteServerBridge`'s constructor) hardcodes the
real `"nds"`/`"Nintendo DS"` and `"melonds"`/`"melonDS"`/`MELONDS_VERSION`
identity. Client-side: the host-selection/discovery list now shows a
second line per host (e.g. `NINTENDO DS - MELONDS`, using `-` in place
of a middle dot since the client's self-contained bitmap font has no
glyph for one -- see `client/src/bitmap_font.cpp`'s supported character
set); the connected-session in-app menu shows the same line as a
subtitle under "MENU"; the disconnected/reconnecting overlay shows the
last known identity (primed from discovery, refreshed from a live
HelloAck, deliberately never cleared on disconnect). See
`docs/architecture.md`'s "Emulator identity model" section for the full
design rationale and `docs/protocol.md`'s section of the same name for
the exact wire layout.

**Verified**: protocol-level round-trip/rejection tests for both
identity structs independently and embedded in each payload
(`protocol/tests/test_identity.cpp`, plus extended
`test_handshake.cpp`/`test_discovery.cpp` cases), including a
deliberately non-melonDS synthetic-identity round trip proving the wire
format doesn't special-case any particular string (issue #28: "keep the
identity model reusable for future adapters"). `tests/smoke_test.py`
extended to start the standalone host with fake `"3ds"`/`"Fake 3DS
Adapter"` identity flags and assert those exact values come back in
both an accepted and a rejected handshake (identity is sent regardless
of `accepted`, same convention as `appVersion`). The melonDS patch was
regenerated, applied cleanly to a **fresh** pristine `git worktree` at
the pinned upstream commit, and built completely from scratch
(`RemoteServerBridge.cpp` now reports `"nds"`/`"melonDS"` real
identity) -- no build errors, no regressions in the surrounding
melonDS-specific files (`EmuInstance*.cpp` etc. are untouched by this
change; only `protocol.h/.cpp`, `net_server.h/.cpp`, and
`RemoteServerBridge.cpp` differ from the previous patch). Both the
client and host binaries build cleanly with strict warnings
(`-Wall -Wextra -Wpedantic -Wconversion -Wshadow`) enabled. `ctest`
passes for both the protocol and client test suites.

**Not verified**: real Steam Deck hardware for the new two-line
discovery list layout (reviewed on the same Xvfb+dummy-driver setup
used throughout this project, not real hardware); no real 3DS or Wii U
adapter exists yet -- the `"3ds"`/`"Fake 3DS Adapter"` identity used in
testing is exactly that, a fake fixture identity proving the plumbing
is generic, not a step toward a real 3DS integration.

**Deliberately not attempted in this milestone** (explicitly out of
scope per the task that produced this change, tracked as later phases
on issue #28 itself): extracting a standalone Host Service decoupled
from the melonDS patch; a full versioned emulator-adapter contract
(session lifecycle, local IPC to an out-of-process adapter); replacing
the single fixed 256x192 DS framebuffer with a list of described video
surfaces; replacing `ControllerState.dsButtons` with a generic input
model; real 3DS/Wii U adapters; and installer/manifest support for
selecting adapters independently (coordinate with GitHub issue #26).

## Adapter contract, ADR, and fake DS/3DS/Wii U fixtures (GitHub issue #28, rest of Phase 1)

Continues the "Emulator identity model" entry above (same GitHub issue
#28), completing the rest of the issue's own "Phase 1: Architecture
decision and generic contracts" milestone: an ADR recording the Host
Service + adapter split, the versioned Emulator Adapter Contract itself
(session lifecycle, video surfaces, generic input, capabilities), the
local-IPC and protocol-migration decisions, and fake DS/3DS/Wii U
capability fixtures proving the contract generalizes. **None of this
touches the live client/host** -- see below.

**What this implements**: `docs/adr/0001-host-service-and-adapter-architecture.md`
records the decisions; a new `adapter-sdk/` component
(`melonds_remote::adapter` namespace) defines `SessionState` +
`isValidTransition()`, `VideoSurfaceDescriptor`/`SurfaceRole`/
`PixelFormat`/`Orientation`, `GenericInputState`/`TouchContact`/
`GenericButton`/`GenericEmulatorAction`, and `AdapterCapabilities`/
`SurfaceFrame`/`IEmulatorAdapter` (`kAdapterContractVersion = 1`).
`adapter-sdk/fake_adapters/` implements that contract three times --
`FakeDsAdapter` (one 256x192 touch surface, matching melonDS's real
shape exactly, including reusing `protocol.h`'s own `kTouchMaxX`/
`kTouchMaxY` constants in its test), `FakeThreeDsAdapter` (400x240
non-touch top + 320x240 touch bottom, real 3DS resolutions), and
`FakeWiiUAdapter` (1920x1080 non-touch TV + 854x480 touch GamePad, real
Wii U resolutions) -- test fixtures only, no real emulator behind any of
them.

**Verified**: 23 new tests (`adapter-sdk/tests/`) covering session-state
transition validity (every documented valid/invalid transition pair,
including the "any state may fault to Error" and "Error/Stopped may
recover to Available" rules), each fixture's declared capabilities/
surfaces/touch ranges, per-surface latest-frame-wins behavior (including
that pushing frames to one surface never disturbs another surface's
independently-tracked frame index -- the required "multiple video
surfaces with different dimensions" test, exercised concretely on the
two-surface 3DS/Wii U fixtures), and input release on every path
(before any input was ever applied, after applying it, and repeatedly).
Critically, `test_adapter_contract_generic.cpp` runs the *same*
assertions against all three fixtures purely through `IEmulatorAdapter&`/
`FakeAdapterBase&` with no per-adapter branches, plus an explicit check
that the three fixtures report distinct `systemId`s (so that generic
test can't silently be testing the same thing three times without
noticing) -- the concrete proof that the contract isn't secretly shaped
only around melonDS. All of `adapter-sdk` (library, fake-adapter
library, and the executable above) builds cleanly with strict warnings
(`-Wall -Wextra -Wpedantic -Wconversion -Wshadow`) and is wired into the
existing `ctest` run via a new root `add_subdirectory(adapter-sdk)`.

**Deliberately not verified / not applicable yet**: there is no wire
transport for this contract (it's an in-process C++ interface only), so
none of the "oversized/malformed message rejected without crashing"
testing-strategy items from issue #28 apply here -- those become
relevant once the ADR's Unix-domain-socket decision is actually
implemented (issue #28 Phase 2), at which point they'll be tested the
same way `protocol.h`'s existing wire types already are (see
`protocol/tests/test_identity.cpp`'s truncated-buffer-rejection style,
for example).

**Zero changes to the live, running system**: `protocol.h`
(`kProtocolVersion` stays 6), `host/remote-server`'s `NetServer`,
`client/src/main.cpp`, and the melonDS patch are all completely
untouched by this entry -- confirmed via `git diff --stat` showing only
new files plus the two docs files and root `CMakeLists.txt`'s one new
`add_subdirectory()` line. This was a deliberate scoping choice (see the
ADR's own framing): get the contract/IPC/migration decisions right
first, proven out by fixtures, before touching the working v0.1
networking code in the higher-risk Host Service extraction that comes
next.

## Adapter IPC channel + real out-of-process synthetic adapter (GitHub issue #28, start of Phase 2)

Continues the two entries above (same GitHub issue #28): with the
contract (`adapter-sdk/`) and its ADR in place, this entry implements
the ADR's Unix-domain-socket IPC decision for real, plus a genuine
out-of-process adapter proving it -- issue #28 Phase 2's "make the
synthetic adapter connect through the same contract real adapters will
use" checklist item. **At the time this entry was written,
`host/remote-server`'s `NetServer` did not yet use any of this in
production** -- see below for exactly what was still deferred; the next
entry below closes that gap.

**What this implements**: `adapter-sdk/ipc/` -- a versioned wire format
(`ipc_protocol.h/.cpp`, distinct magic `"DAI1"` from the client<->host
channel's `"DMR1"` so the two can never be confused, gated on
`kAdapterContractVersion`) for `Hello`/`HelloAck`/`InputState`/
`ReleaseInputs`/`Frame`/`StateChanged`/`Heartbeat`/`Disconnect`
messages; `AdapterIpcServer` (implements `IEmulatorAdapter` itself,
listens on a Unix domain socket under `$XDG_RUNTIME_DIR/dualdeck/`
mode 0700/0600, checks the connecting peer's UID via `SO_PEERCRED`
before even reading a handshake byte, and proxies every
`IEmulatorAdapter` call to whatever's currently connected); and
`AdapterIpcClient` (wraps any local `IEmulatorAdapter` and exposes it
to a Host Service over that socket). `adapter-sdk/synthetic_adapter/`
adds `SyntheticEmulatorAdapter` (a real, self-contained animated BGRA
test pattern on its own thread -- no dependency on
`host::SyntheticFrameSource`, since `adapter-sdk` is meant to be a
layer `host/remote-server` will eventually depend on, not the reverse)
and `dualdeck-synthetic-adapter`, a standalone executable wiring it
through `AdapterIpcClient`.

**Verified**: 22 new tests (`test_ipc_protocol.cpp`,
`test_adapter_ipc_end_to_end.cpp`) covering wire round-trips for every
message payload type, oversized/malformed-size rejection (a declared
frame pixel count or surface count over the documented bound is
rejected before any allocation), a real `AdapterIpcServer` +
`AdapterIpcClient` pair connected over an actual Unix socket in the
same test process (capability negotiation, input relayed from the
server call all the way into the real connected `FakeDsAdapter`, frames
and session-state changes relayed the other direction, a hand-crafted
raw-socket contract-version mismatch correctly rejected with
`ContractVersionMismatch`, hand-crafted garbage bytes correctly
rejected without crashing the server, and "replacement" -- a second
adapter successfully taking over once the first disconnects).
**Beyond the automated tests**, a real cross-process run was performed:
a throwaway harness process started a real `AdapterIpcServer`, and the
actual packaged `dualdeck-synthetic-adapter` binary was launched
separately and pointed at its socket -- confirmed identity/capabilities
negotiated correctly, and 147 real frames received with a strictly
increasing frame index (7 → 154 across the observation window) and the
correct 196,608-byte (256×192×4) payload size, i.e. genuinely animated
frames flowing between two independent OS processes, not just two
threads in one test binary.

**A real bug was caught by this verification and fixed**: none of the
IPC socket code originally set a receive timeout, so a peer that never
sends anything (including a second adapter's connection attempt sitting
in the listen backlog while `AdapterIpcServer`'s single-connection-at-
a-time accept loop was still busy serving a different, still-connected
adapter) left `recv()` blocking forever -- a real deadlock, hit
immediately by the first version of the "second connection while one is
active" test. Fixed by adding `SO_RCVTIMEO` (5s) to every socket on this
channel, matching `NetServer::controlLoop()`'s existing precedent for
the client<->host control channel; the test itself was also restructured
to run the blocking `connect()` call on its own thread rather than the
test's main thread, since a second adapter genuinely is expected to
block until the first disconnects under this "one at a time" design,
not fail immediately.

**Deliberately not done in this phase** (tracked as issue #28 Phase 2's
remaining work at the time, not attempted in this entry): wiring any of
this into `host/remote-server`'s actual `NetServer`/`main.cpp` -- at
this point there was no production Host Service listening on this
socket anywhere yet, only the test suite and the manual verification
harness described above. The live client<->host wire protocol
(`protocol.h`, `kProtocolVersion 6`) and the melonDS patch remained
completely untouched, confirmed the same way as the previous two entries
(`git diff --stat` showing only new files under `adapter-sdk/` plus this
doc and the ADR). See the entry immediately below for the follow-up that
closed this gap.

## AdapterBridge wires the adapter contract into the real Host Service (GitHub issue #28, rest of Phase 2)

Continues the entry above (same GitHub issue #28): the previous entry
built the IPC mechanism but explicitly stopped short of wiring it into
`host/remote-server`'s actual `NetServer`/`main.cpp`. This entry closes
that gap -- issue #28 Phase 2's "make NetServer able to drive a real
out-of-process adapter" checklist item.

**What this implements**: `host/remote-server/src/adapter_bridge.{h,cpp}`
adds `AdapterBridge`, which implements the existing
`host::IEmulatorInputSink`/`host::IFrameSource` interfaces by
translating to/from `melonds_remote::adapter::IEmulatorAdapter` -- the
"DS compatibility adapter" the ADR (`docs/adr/0001-...md`) already
decided on, now actually built. It picks one target video surface at
construction time (the first `remotelyDisplayed` surface, falling back
to the first declared surface if none is flagged -- verified against
`FakeThreeDsAdapter`, which declares a `locallyDisplayed` "top" surface
before its `remotelyDisplayed` "bottom" surface, to prove the bridge
isn't just taking whichever surface happens to be listed first), maps
`ControllerState.dsButtons` to `GenericButton` via an explicit
bit-by-bit table (deliberately not a shift/mask trick, since the two
enums' bit layouts coincide for A/B/X/Y/D-pad/L/R but diverge at
Start/Select -- `GenericButton` reserves bits 10-13 for L2/R2/L3/R3,
concepts the DS has none of, pushing Start/Select to bits 14/15 where
`DSButton` has them at bits 10/11), and passes
`EmulatorAction`/`GenericEmulatorAction` straight through as a raw
`uint32_t` since those two enums were deliberately given identical bit
positions when the generic contract was designed.

`host/remote-server/src/main.cpp` gained an opt-in
`--adapter-ipc`/`--adapter-socket PATH` flag pair: when given, the
server starts an `AdapterIpcServer`, blocks until a real adapter
connects over the local Unix socket, auto-detects
`SystemIdentity`/`AdapterIdentity` from the connected adapter's reported
capabilities (unless the corresponding `--system-id`/`--system-name`/
`--adapter-id`/`--adapter-name`/`--adapter-version` flags were given
explicitly, in which case those win), then constructs `NetServer` with
an `AdapterBridge` wrapping that adapter instead of the default
`LoggingInputSink`+`SyntheticFrameSource` pair. **The default,
no-flag invocation is completely unchanged** -- this is purely additive.

**Deliberately not done in this phase**: mic audio is not bridged --
`GenericInputState` only carries a `micActive` bool, not raw PCM
samples, so actually forwarding audio through `IEmulatorAdapter` would
need a contract change not made here; `--adapter-ipc` mode keeps using
the same `LoggingMicAudioSink` as the default mode (mic is logged, never
injected, in either mode on the standalone host -- this was already true
before this milestone). There is also no host-side notification yet if
the adapter disconnects mid-session -- frames/input just silently stop
flowing via existing no-op/false-return paths; this is deferred until
melonDS itself becomes a real out-of-process adapter, since that's when
this gap first has real consequences for an actual user session.

**Verified**: 11 new unit tests (`host/remote-server/tests/test_adapter_bridge.cpp`)
covering both target-surface-selection cases above, every individual
`DSButton`→`GenericButton` mapping (data-driven, one case per button),
a button-combination test that also asserts the DS-unused L2 bit stays
clear, touch-active/inactive translation, `emulatorActions` bit-identical
passthrough, sequence/timestamp passthrough, `releaseAll()` reaching the
wrapped adapter, and `getLatestFrame()`'s both false-when-empty and
proxies-pushed-frame behavior -- all run against the same
`FakeDsAdapter`/`FakeThreeDsAdapter` fixtures from the earlier Phase 1
entry, so no socket is needed for this layer's own tests.

**Beyond the unit tests, a real cross-process end-to-end run was
performed** with three genuinely separate OS processes and no test
harness standing in for any of them: the real `melonds-remote-server`
binary (`--bind 127.0.0.1 --adapter-socket <path>`), the real
`dualdeck-synthetic-adapter` binary pointed at that same socket, and the
real SDL3 `melonds-remote-client` binary (run under Xvfb, exactly the
same verification pattern used for the identity-model milestone
earlier). The client's log showed a genuine `[net] connected` against
the adapter-driven server; two screenshots taken one second apart while
the adapter was running showed different pixel content (the synthetic
adapter's animated test pattern actually changing), confirming live
frames are reaching the real client end-to-end from a separate adapter
process through the unchanged wire protocol. (An earlier pair of
screenshots, taken after the synthetic adapter process had already
exited, came back byte-identical -- a useful negative control showing
the client correctly holds its last frame rather than fabricating motion
when the adapter goes away, matching the "silently stop flowing" gap
noted above.) Input-path coverage for this specific run relied on the
unit tests above plus the previous entry's real cross-process IPC tests
rather than a live keypress probe, since the translation and transport
legs are each already independently proven and a live probe would only
be re-confirming the same wiring a third time.

Existing `tests/smoke_test.py` and `tests/device_approval_smoke_test.py`
were re-run against the default (no `--adapter-ipc`) invocation after
every change in this milestone and continued to pass, confirming
today's released client/host behavior is unaffected.

## melonDS itself now implements the generic adapter contract, in-process (GitHub issue #28, Phase 2 continuation)

Continues the two entries above (same GitHub issue #28): those built
`AdapterBridge` and proved it against a real out-of-process synthetic
adapter, but explicitly left `host/melonds-patches`'s `RemoteServerBridge`
untouched. This entry closes that gap for melonDS specifically.

**What this implements**: a new `MelonDSAdapter` class
(`host/melonds-patches/0001-remote-server-integration.patch`'s
`remote_server/MelonDSAdapter.{h,cpp}`) implements
`melonds_remote::adapter::IEmulatorAdapter`, wrapping the same
`MelonDSInputSink`/`MelonDSFrameSource` the patch already used
(internally unchanged) and translating `GenericInputState` back into a
wire `ControllerState` via the exact inverse of `host::AdapterBridge`'s
DS-button table. `RemoteServerBridge` now constructs `MelonDSAdapter` +
`host::AdapterBridge` (the identical class, vendored verbatim into the
patch alongside the rest of `adapter-sdk`'s contract headers) and hands
the bridge to `NetServer`, instead of exposing
`MelonDSInputSink`/`MelonDSFrameSource` as
`IEmulatorInputSink`/`IFrameSource` directly.

**Deliberately in-process, not out-of-process**: this does *not* spawn
`melonds-remote-server` as a child process or connect over
`adapter-sdk/ipc/` -- it reuses the ADR's "an adapter that runs in the
same process as the Host Service needs no IPC at all" allowance instead.
`NetServer`, `DeviceApprovalManager`, LAN discovery, and
`MelonDSMicAudioSink` are all completely untouched (mic audio stays
wired directly to `NetServer` as its own sink, independent of the
adapter contract, which only covers video + controller/touch input) --
same public `RemoteServerBridge` interface as before, so
`EmuInstance.cpp`/`EmuInstanceInput.cpp`/`EmuInstanceAudio.cpp`/
`Window.cpp`/`Config.cpp`/`EmuSettingsDialog.*` needed **zero changes**,
confirmed by diffing the regenerated patch against the previous one:
every change is contained to `remote_server/` (new `MelonDSAdapter.h/.cpp`,
vendored `adapter_sdk/` contract headers + `host/adapter_bridge.h/.cpp`,
and a small `RemoteServerBridge.{h,cpp}`/`CMakeLists.txt` rewire).

A true out-of-process melonDS adapter (spawning `melonds-remote-server
--adapter-ipc` as a child process, connecting over the socket like the
synthetic adapter does) was considered and deliberately set aside: it
opens a real, unresolved design question that doing the in-process
version first avoids risking anything on -- `DeviceApprovalManager`'s
pending-request queue and `approveDevice()`/`denyDevice()` are in-process
method calls with no cross-process API, so a Qt approval dialog running
in a different process than the one holding `DeviceApprovalManager`
would need a new RPC surface that doesn't exist yet (the adapter IPC
channel only covers session/video/input, deliberately not device
management -- that's a Host Service concern per the ADR). Tracked as
future work on issue #28, not attempted here.

**Verified**: rebuilt the patched melonDS binary (fresh clone at the
pinned commit, patch reapplied, `MelonDSAdapter.cpp`/vendored
`adapter_bridge.cpp`/`session_state.cpp` all compiled clean under the
same strict warnings as the rest of the patch) and ran the real pipeline
proof from `tests/homebrew-test-rom/`: direct-booted the real,
JIT-executing `test.nds` ROM under Xvfb with `MELONDS_REMOTE_ENABLE=1`,
confirmed the v6 Hello/HelloAck handshake and identity
(`nds`/`Nintendo DS`, `melonds`/`melonDS`/`1.1`) round-tripped correctly,
then sent real UDP `ControllerState` packets for A/B/Up in turn through
the actual `NetServer` → `AdapterBridge` → `MelonDSAdapter` →
`MelonDSInputSink` → `EmuInstance::inputProcess()` →
`NDS::SetKeyMask()` → CPU register read → `arm9.c`'s KEYINPUT-reactive
backdrop-color write → `GPU::GetFramebuffers()` →
`MelonDSAdapter::pushFrame()` → `AdapterBridge::getLatestFrame()` →
`NetServer`'s video thread → client chain. Each button produced a
distinct, stable, correctly-mapped color (A → red channel, B → green,
Up → blue, matching the exact mapping `tests/homebrew-test-rom/README.md`
already documented) -- conclusive proof the double bit-table translation
(wire `DSButton` → `GenericButton` → wire `DSButton` again) didn't
scramble which physical button does what.
(`tests/homebrew-test-rom/interactive_pipeline_test.py` itself is stale
-- written for protocol v1, predating v2-v6's Hello/HelloAck growth --
so this verification used an ad-hoc probe speaking the current v6
framing instead; fixing that stale script is tracked separately, not
blocking here since `tests/smoke_test.py`'s handshake logic already
covers the current wire format.)

## melonDS as an opt-in out-of-process adapter (GitHub issue #4 Phase A)

Issue #4 asks for host-side controls to reach the Steam Deck client
*without* melonDS needing to already be running -- navigating/launching
the host interface before melonDS starts and after it closes. That
requires a Host Service that outlives melonDS's process, which is
impossible under the in-process design the previous entry describes
(`NetServer` lives inside melonDS itself, so there's no host process
before melonDS starts or after it exits). Phase A is the prerequisite
that makes that possible at all: letting melonDS connect to an
*already-running, standalone* `melonds-remote-server --adapter-ipc`
process as a genuine out-of-process adapter, strictly opt-in, with
zero change to the in-process default any existing user is on.

**What this implements**: `RemoteServerBridge` (in the patch) gained a
second constructor taking just an adapter-socket path, alongside the
existing in-process one -- both share the same `MelonDSAdapter`/
`MelonDSMicAudioSink` members, so `pushBottomFrame()`,
`latestControllerState()`, `drainMicAudio()` etc. need no branching at
all; only `start()`/`stop()`/`approveDevice()`/`denyDevice()` check
which of `server_` (in-process `NetServer`) or `ipcClient_`
(`AdapterIpcClient`) is populated. `start()` in out-of-process mode
spawns a reconnect-loop thread mirroring `client/src/main.cpp`'s own
`NetClient` reconnect pattern (1s→5s exponential backoff, 100ms poll
granularity for responsive shutdown). Two new opt-in settings —
`MelonDSRemote.OutOfProcess` (bool, default false) and
`MelonDSRemote.AdapterSocket` (string, default empty →
`defaultAdapterSocketPath()`), plus matching
`MELONDS_REMOTE_OUT_OF_PROCESS`/`MELONDS_REMOTE_ADAPTER_SOCKET` env
overrides — gate this in `EmuInstance.cpp`'s `startRemoteServer()`,
checked *before* any in-process setup so the existing default path is
completely untouched when unset. `approveDevice()`/`denyDevice()`
return `false` in this mode (documented limitation, not silently
broken): device approval is `DeviceApprovalManager`'s in-process
concern on the *Host Service* side now, same unresolved gap the
previous entry flagged for full out-of-process operation.

**Two real bugs found and fixed while getting this working end-to-end**
(both in `adapter-sdk/ipc/`, the shared library the standalone
`melonds-remote-server` binary and this vendored-into-melonDS client
side both build from):

1. **Reconnect-after-drop crash**: `AdapterIpcClient::disconnect()`
   only joined `readThread_`/`writeThread_` when it itself had set
   `connected_` false first. But those threads can *also* clear
   `connected_` on their own (a recv timeout or send failure -- e.g.
   the Host Service went away), leaving them joinable without the
   caller ever knowing to call `disconnect()`. melonDS's new
   reconnect loop calls `connect()` directly without a preceding
   `disconnect()` -- the first real caller to do that — and
   `std::thread`'s move-assign inside `connect()` terminates the
   process if the target is still joinable. Fixed by making
   `disconnect()` always join regardless of `wasConnected`, and by
   having `connect()` itself defensively join any leftover joinable
   threads at its start. Covered by a new regression test,
   `ipc_client_reconnects_on_the_same_instance_after_connection_drop`
   (`adapter-sdk/tests/test_adapter_ipc_end_to_end.cpp`), verified to
   actually reproduce the pre-fix crash via a temporary revert.

2. **Adapter-side idle timeout with no client connected**:
   `AdapterIpcServer::serveConnection()` only ever sent bytes to the
   connected adapter in direct response to real client input
   (`applyGenericInput()`/`releaseAllInputs()`). With no client
   connected yet -- or one that's just quiet -- that's genuinely zero
   outbound traffic for 5+ seconds, which the adapter's own 5s
   `SO_RCVTIMEO` on its `recv()` (`AdapterIpcClient::readLoop()`)
   wrongly read as a dead connection, tearing the whole IPC session
   down and reconnecting in a loop. Fixed by giving
   `serveConnection()` its own ~1s heartbeat-sender thread, mirroring
   `AdapterIpcClient::writeLoop()`'s existing heartbeat-when-idle
   logic in the other direction. That heartbeat thread and
   `applyGenericInput()`/`releaseAllInputs()` (called from a
   different thread, in direct response to client UDP input) both
   write to the same socket fd -- a `writeMutex_` was added to
   serialize every send on `clientFd_`, since without it the two
   threads racing could interleave message bytes on the wire and
   corrupt the framing the other end's `recvMessage()` relies on
   (silently garbling or dropping whatever message lost the race,
   not crashing outright -- this is what a first, unsynchronized
   version of the heartbeat fix actually did, caught during
   real-pipeline verification below before landing the mutex).
   Covered by a new regression test,
   `ipc_connection_survives_idle_gap_with_no_client_driven_traffic`,
   asserting the connection and real input delivery both survive a
   6.5s gap with zero `applyGenericInput()` calls.

**Verified**: real cross-process pipeline, matching the previous
entry's rigor. A standalone `melonds-remote-server --adapter-ipc`
process and a separately-launched, real, JIT-executing
`tests/homebrew-test-rom/test.nds` melonDS instance (headless under
Xvfb, `MELONDS_REMOTE_ENABLE=1 MELONDS_REMOTE_OUT_OF_PROCESS=1`) --
two genuinely independent OS processes connected only over the
adapter-IPC Unix socket. Confirmed: handshake + identity
(`nds`/`Nintendo DS`, `melonds`/`melonDS`/`1.1`) round-trip correctly;
held buttons (A/B/Up) produce the correct distinct, stable colors
matching the wired mapping, exactly as the in-process entry above; and
-- the actual point of this milestone's two bug fixes -- the whole
session (adapter IPC connection *and* real button-driven video colors)
survives a deliberate 7-second window with zero client-driven UDP
input, proving the Bug 2 fix rather than just asserting it.

That last check took three iterations to get right and is worth
recording: the first version of the idle-gap probe also stopped
*reading* the video socket during the gap, which correctly-by-design
trips a completely separate, pre-existing 1-second `SO_SNDTIMEO` in
`NetServer::videoLoop()` (a stalled/non-reading client's video
connection is deliberately dropped rather than left to block that
thread forever) -- not a bug, but it looked exactly like one from the
symptom (colors staying black after the gap) until traced with
targeted `recv()`/`inputLoop()` diagnostics on both the adapter-IPC and
client-facing sides. The probe was fixed to keep draining video and
sending control-channel heartbeats throughout the gap, same as any
real client (which is always rendering) already does -- at which point
the real fix was confirmed working cleanly on the first try.

## NetServer's runtime-swappable target + ModeChanged notification (GitHub issue #4 Phase B)

Continues the entry above (same GitHub issue #4): Phase A made it
possible for an emulator to connect to an already-running Host Service
as an out-of-process adapter. Phase B is the other half of the
prerequisite for host-control mode -- letting the Host Service itself
switch which adapter is driving a session *while a client stays
connected*, instead of every mode change requiring a fresh handshake.
This is what will let a client navigate the host's own UI before an
emulator has been launched and after it exits (issue #4's later
phases), without ever needing to reconnect.

**What this implements**: `NetServer` no longer binds permanently to
one `IEmulatorInputSink&`/`IFrameSource&` pair at construction.
`NetServer::setTarget(inputSink, frameSource, mode, systemIdentity,
adapterIdentity)` atomically swaps the active pair (guarded by a new
`targetMutex_`, held briefly around every touch of the target from
`inputLoop()`/`videoLoop()`/`watchdogLoop()`/handshake/discovery code,
matching how this codebase already treats `trackerMutex_`/`statsMutex_`
for similarly frequent, briefly-held state), calls the *previous*
target's `releaseAll()` (so a button/touch held at the moment of a swap
can never carry over onto whatever's driving the session now), and
sends an already-connected, already-authenticated client a new
`ModeChanged` packet (`protocol/`'s `PacketType::ModeChanged`) on the
control channel -- a safe no-op, deferred to the next connecting
client's `HelloAck`, if nobody is connected right now or the send
fails. `HostMode` (`Emulation` or `HostControl`) and `ModeChangedPayload`
(mode + the same `SystemIdentity`/`AdapterIdentity` encoding
`HelloAckPayload`/`DiscoveryResponsePayload` already use) are new wire
types in `protocol/`; see `docs/protocol.md`'s "ModeChanged payload"
section for the exact format and why this addition did not need a
`kProtocolVersion` bump (unlike every previous wire change, it's not
part of Hello/HelloAck/DiscoveryResponse negotiation -- an older client
build simply never reads it, since none of them read anything from the
control channel post-handshake at all yet).

**Client-side is deliberately not touched here**: no client build
today has a read loop on the control channel after the initial
`HelloAck`, so a sent `ModeChanged` packet currently just sits unread
in the client's TCP receive buffer -- harmless, not an error, but also
not yet acted on. Wiring an actual read loop and reacting to mode
changes in the UI is GitHub issue #4 Phase E's job, intentionally
deferred; this phase only had to prove the host side of the mechanism
works, ahead of a client that consumes it (same "wire format lands
before the consumer" sequencing `docs/protocol.md`'s new section
explicitly calls out).

**Verified**: a new real end-to-end test suite,
`host/remote-server/tests/test_net_server_mode_switch.cpp` (7 new
cases, all real sockets against a real running `NetServer` -- no mocks
of the network layer, matching this project's established testing
convention). Confirms: a real UDP `ControllerState` packet reaches
whichever `IEmulatorInputSink` is currently active and *not* the
previous one; the previous target's `releaseAll()` fires immediately on
swap (checked by asserting its last-known state goes back to
all-released); a real TCP video connection's next frames come from the
newly-active `IFrameSource` (a small `FakeFrameSource` test double
fills every pixel with one distinct byte per instance, so which source
produced a captured frame is unambiguous without needing
`SyntheticFrameSource`'s own generator thread); a connected client
receives a correctly-populated `ModeChanged` packet exactly when
`setTarget()` is called mid-session; calling `setTarget()` with nobody
connected is a genuine no-op (no crash, no hang); `currentMode()`
always reflects the most recent call; and a brand-new handshake made
*after* a `setTarget()` call that predates any connection at all
correctly sees the swapped identity in its `HelloAck`, not the
construction-time default. Full `ctest` suite (protocol, adapter-sdk,
client-settings, host) passes with strict warnings
(`-Wall -Wextra -Wpedantic -Wconversion -Wshadow`) enabled.

## HostControlAdapter: a virtual gamepad for host navigation (GitHub issue #4 Phase C)

Continues the two entries above (same GitHub issue #4): Phase A let an
emulator connect as an out-of-process adapter, Phase B let
`NetServer` swap its active target while a client stays connected. This
phase builds the *other* target those two phases were building toward
-- something to swap *to* before an emulator has been launched (or
after it exits), so a connected client isn't just sitting on a dead
session in that window.

**What this implements**: `HostControlAdapter`
(`host/remote-server/include/host/host_control_adapter.h` + `.cpp`)
implements `IEmulatorInputSink`/`IFrameSource` directly -- the same two
interfaces `LoggingInputSink`/`SyntheticFrameSource` implement, and
exactly what `NetServer::setTarget()` needs -- rather than going through
the generic `adapter-sdk::IEmulatorAdapter` contract real emulator
adapters use. Host-control mode isn't "emulating a system" with
capabilities/surfaces to negotiate; it's host-side input translation, so
that extra layer would add nothing here. It creates a virtual
Xbox-360-style gamepad via Linux's uinput subsystem (reusing the real
Xbox 360 controller's USB vendor/product IDs, the same convention
several other open-source virtual-gamepad projects use, so desktop
environments and Steam Input recognize its button layout correctly
without a manual mapping step) and translates incoming
`ControllerState`/`DSButton` fields onto it: `DSButton_A`/`B` -> south/
east face buttons, `DSButton_X`/`Y` -> west/north (matching
`host::AdapterBridge`'s existing `dsButtonsToGenericButtons` table's
X-is-west/Y-is-north convention exactly, so this project has one
consistent meaning for those two buttons everywhere, not two disagreeing
ones), `L`/`R` -> shoulder buttons, `Start`/`Select` -> their Xbox
equivalents, the D-pad -> a hat axis pair, and the left analog stick
passed through unscaled (both use the same centered-at-0 `int16_t`
range already). `getLatestFrame()` always returns `false` -- there is no
emulated screen to stream while no emulator is running; a client shows
its own local UI instead (issue #4 Phase E, not yet built).

The DSButton -> gamepad translation is deliberately a pure function,
`translateControllerState()`, entirely free of I/O, separated from the
actual `open("/dev/uinput")`/`ioctl()`/`write()` calls
(`HostControlAdapter::emitState()`) specifically so the mapping logic
can be unit-tested without a real uinput device.

**Real environment limitation, anticipated when this milestone was
first planned**: this sandbox has no `/dev/uinput` node at all, so real
virtual-device creation cannot be exercised here -- only the pure
translation function above, and `HostControlAdapter`'s graceful-failure
path (`open()` failing, logging once, `isDeviceReady()` staying `false`,
every subsequent call becoming a safe no-op rather than crashing --
matching this codebase's established handling of any other unavailable
optional resource, e.g. `net_server.cpp`'s audio-port bind failure).
Real device creation (the `UI_SET_EVBIT`/`UI_SET_KEYBIT`/`UI_DEV_SETUP`/
`UI_ABS_SETUP`/`UI_DEV_CREATE` ioctl sequence) compiles clean against
this environment's real `<linux/uinput.h>` headers and follows the
documented modern uinput API exactly, but has only been verified by
inspection, not by actually creating a device and confirming a desktop
environment recognizes it -- that needs a real Linux host with the
uinput kernel module loaded and either root or `/dev/uinput` write
access (typically via the `input` group or a udev rule), which is
worth calling out explicitly to whoever eventually runs this on real
hardware, the same way `docs/steam-deck-setup.md` calls out other
one-time host permission steps.

**Deliberately not wired up yet**: `HostControlAdapter` is not
constructed or passed to any `NetServer::setTarget()` call anywhere in
this codebase -- deciding *when* a Host Service should actually swap to
it (on startup before any adapter has connected, on an adapter
disconnecting, and swapping back once one connects) is GitHub issue #4
Phase D's job, intentionally left open here so this phase's scope stays
to "build the thing being swapped to," not "decide when to swap."

**Verified**: `host/remote-server/tests/test_host_control_adapter.cpp`
(11 new cases) covers every button/hat/stick mapping decision above
against `translateControllerState()` directly, plus a guard confirming
`HostControlAdapter` degrades gracefully (no crash, `isDeviceReady() ==
false`, every call a safe no-op) in this sandbox's actual no-`/dev/
uinput` environment. Full `ctest` suite passes with strict warnings
(`-Wall -Wextra -Wpedantic -Wconversion -Wshadow`) enabled, including a
clean compile of the real uinput ioctl sequence itself.

## Host Service mode coordination + manual override (GitHub issue #4 Phase D)

Completes the trio started by the three entries above (same GitHub
issue #4): Phase A let an emulator connect out-of-process, Phase B let
`NetServer` swap targets at runtime, Phase C built the host-control
target itself. This phase is what actually *decides when* to swap --
wiring `--adapter-ipc` mode's real startup sequence so a client no
longer has to wait for an emulator before it can do anything at all.

**What this implements**: `host/remote-server`'s `--adapter-ipc` mode no
longer blocks waiting for an adapter to connect before starting
`NetServer`. It now constructs a real `HostControlAdapter` and starts
`NetServer` pointed at it immediately (`HostMode::HostControl`), then
hands both that and an `AdapterBridge`-wrapped `AdapterIpcServer`
(`HostMode::Emulation`) to a new `ModeCoordinator`, which polls (100ms)
whether an adapter is currently connected and calls
`NetServer::setTarget()` on every actual transition -- `HostMode::Emulation`
(reporting the connected adapter's own identity, or `--system-id`/
`--adapter-id` if explicitly given) whenever one is connected,
`HostMode::HostControl` otherwise. The mode-decision itself is a pure
function, `computeDesiredMode(adapterConnected, manualHostControlOverride)`,
kept free of any I/O so it's testable in isolation.

**Manual override**: an operator can type `hostcontrol` at this
process's console to force `HostMode::HostControl` even while an
adapter stays connected -- stepping back out to host navigation without
having to disconnect the emulator adapter first -- `resume` to clear
that override and let auto-detection resume immediately, or `mode` to
check the current state. These share the exact same console-reading
loop the existing `approve`/`deny`/`list` device-approval commands
already use (renamed `consoleLoop`, was `approvalConsoleLoop`) rather
than a second thread, since two threads both calling
`std::getline(std::cin, ...)` concurrently would race on stdin.

**Verified**: `host/remote-server/tests/test_mode_coordinator.cpp` (12
new cases: 4 on `computeDesiredMode()` directly, 8 real end-to-end
against a real `AdapterIpcServer`/`NetServer`/`ModeCoordinator`, with a
real `SyntheticEmulatorAdapter` connecting and disconnecting over a real
Unix-domain-socket `AdapterIpcClient` -- adapter-sdk's own established
e2e style, no mocks of the network layer). Also verified end-to-end
against the actual `melonds-remote-server` binary (not just the test
suite): a real subprocess started in `--adapter-ipc` mode accepted a
client handshake immediately, reporting `host-control`/`Host Control`
identity, with no adapter connected at all; connecting a real
`dualdeck-synthetic-adapter` subprocess auto-switched it to
`synthetic-ipc`/`Synthetic IPC Adapter`; the `hostcontrol` console
command forced it back to `host-control` identity while that adapter
process was still connected; `resume` switched it back to
`synthetic-ipc` with no reconnect needed; and terminating the adapter
subprocess auto-switched it back to `host-control`.

That real-binary verification surfaced a pre-existing, unrelated
correctness lesson worth recording: `NetServer`'s connection-attempt
rate limiter (spec section 13, predates this phase entirely) allows
only a handful of new control connections per 10-second window from one
address. A first version of the verification script re-connected and
re-handshook every 100ms while polling for a mode change to take
effect, which tripped that limiter itself and produced a misleading
`ConnectionResetError` that had nothing to do with `ModeCoordinator`'s
actual behavior (the server-side logs showed the mode change had
already been applied correctly). Fixed in the script, not the product,
by checking once per phase with a short fixed delay instead of polling
in a tight loop -- `ModeCoordinator`'s own 100ms internal poll interval
means a single check after 1 second is already generous.

**Still open**: `HostControlAdapter`'s virtual gamepad remains
unavailable in this sandbox (no `/dev/uinput`, see the Phase C entry) --
this phase's coordination logic is exercised and confirmed correct
regardless, since `ModeCoordinator` only cares about swapping targets
correctly, not about what a real uinput device actually does once
selected. See the next entry (GitHub issue #4 Phase E) for the client
side of this, which was left untouched through Phases B-D deliberately.

## Client UI for host-control mode, and the control channel it needed (GitHub issue #4 Phase E)

Closes out the client side of GitHub issue #4, started by the four
entries above: Phases A-D built a host that can run with no emulator
attached at all and tell a connected client about it via `ModeChanged`
-- but until this phase, nothing on the client ever read that packet.
Answering the "how many more phases until a working build?" gap
analysis from partway through this issue: the client had no
control-channel read loop at all, so host-control mode could never
actually be *used*, only exercised server-side.

**What this implements**:

- **`NetClient::controlReceiveLoop()`** (`client/src/net_client.{h,cpp}`),
  a new background thread alongside the pre-existing `videoReceiveLoop()`/
  `heartbeatLoop()`, started the moment `connect()` succeeds. Reads and
  parses each packet off the control socket; `ModeChanged` updates a new
  `hostMode()`/`hostSystemIdentity()`/`hostAdapterIdentity()` triple
  (the latter two already existed for the initial handshake -- this
  phase makes them live for the rest of the session too). Anything else
  recognized-but-unhandled is tolerantly ignored rather than dropping
  the connection, mirroring the host's own `NetServer::controlLoop()`
  convention exactly (`docs/protocol.md`'s "ModeChanged payload" section
  describes why this matters for forward compatibility). Before this,
  the control socket was write-only from the client's side -- unsolicited
  host->client packets simply sat unread in the TCP receive buffer
  forever.
- **`HelloAckPayload.mode`** (protocol v7, bumping from v6): a fresh
  handshake now reports the host's *current* mode directly, since
  `ModeChanged` is only ever sent to an already-connected client and so
  would never arrive for a client that connects (or reconnects) while
  the host is already in `HostControl` mode -- see `docs/protocol.md`'s
  "HelloAck payload" section. `host/remote-server/src/net_server.cpp`'s
  handshake response now fills this from `currentMode_` under the same
  lock it already takes for `system`/`adapter`.
- **A dedicated "HOST CONTROL" screen** (`client/src/main.cpp`'s
  `renderHostControlScreen()`), shown in place of the video texture
  whenever `net.hostMode() == HostMode::HostControl`. `ControllerState`
  packets keep being sent every frame exactly as in `Emulation` mode --
  input isn't gated on which screen is showing -- so a real host's
  `HostControlAdapter` virtual gamepad is already receiving input the
  instant this screen appears; only the on-screen presentation differs
  (there is no video to show: `HostControlAdapter::getLatestFrame()`
  always returns `false`). Falls back to the ordinary video-texture path
  the instant the mode flips to `Emulation`, and the identity line
  (already tracked for GitHub issue #28) refreshes on every mode
  transition, not just the initial connect, so it reflects e.g. "HOST
  MENU" while in host-control mode and the real system/adapter identity
  once an emulator connects.
- **A latent heartbeat-failure bug this phase's own testing surfaced**:
  `heartbeatLoop()` used to just `break` out of its loop on a `send()`
  failure without ever setting `connected_ = false`. In `Emulation` mode
  this was usually masked, since a dead connection also stops video
  frames from arriving and `videoReceiveLoop()`'s own `recv()` failure
  would eventually notice. In `HostControl` mode, no video frames were
  ever going to arrive in the first place, so a control-channel-only
  failure (e.g. the host process dying) could leave `isConnected()`
  reporting `true` forever with a socket that could never recover.
  `controlReceiveLoop()` closes the more direct gap (a `recv()` failure
  on the control channel itself now sets `connected_ = false`, same as
  `videoReceiveLoop()` already did), and `heartbeatLoop()`'s `send()`
  failure path was fixed the same way while this file was open, since it
  was the exact scenario that motivated the fix in the first place.

**Verified**:

- `client/tests/test_net_client.cpp` (5 new cases, real sockets, no
  mocking of either side -- a real `NetClient` against a real
  `host::NetServer`, matching this project's established e2e test
  style): a fresh handshake reports `HostMode::Emulation` by default and
  `HostMode::HostControl` when the server was already switched before
  the client ever connected; a live `setTarget()` call while connected
  updates `hostMode()`/identity in both directions without dropping the
  connection; and stopping the server is actually detected
  (`isConnected()` becomes `false`) rather than hanging forever, which
  is precisely the gap this phase closed.
- A full real-binary, real-process, no-mocking end-to-end run: the
  actual `melonds-remote-client` binary (`SDL_VIDEODRIVER=dummy`/
  `SDL_AUDIODRIVER=dummy`, no real display available in this sandbox --
  see "No CI-verified client build" below for that pre-existing,
  unrelated limitation) connected to a real `melonds-remote-server
  --adapter-ipc` subprocess, stayed alive showing the host-control
  screen, observed a real `dualdeck-synthetic-adapter` subprocess
  connecting (`[net] host mode changed to EMULATION`, no crash),
  observed it disconnecting again (`[net] host mode changed to HOST
  CONTROL`, no crash) -- proving the client-side wiring in `main.cpp`
  actually works end-to-end, not just `NetClient` in isolation.
- Protocol round-trip/rejection tests for the new `mode` field
  (`protocol/tests/test_handshake.cpp`), plus `mode` assertions added to
  the existing Phase B/D host-side handshake tests
  (`test_net_server_mode_switch.cpp`, `test_mode_coordinator.cpp`) to
  confirm the field is actually populated from `currentMode_`, not just
  parseable.
- `tests/smoke_test.py`/`tests/device_approval_smoke_test.py` updated
  for protocol v7 and re-run against the real `melonds-remote-server`
  binary -- both still pass.

**Still open**: The real-hardware gaps already on record for Phases A-D
carry over unchanged and are not re-verified here: `HostControlAdapter`'s
uinput device has still only ever been exercised via pure-logic tests
and the graceful-degradation path (no `/dev/uinput` in this sandbox --
see the Phase C entry), and there is still no CI-verified client build
or real-hardware Steam Deck run for this specific screen (see "No
CI-verified client build" below, pre-existing and unrelated to this
phase). See the next entry (GitHub issue #4 Phase F) for the packaging
gap that used to be here: nothing in the actual shipped release could
trigger host-control mode at all, only this phase's and Phase D's
scripted/manual verification against the standalone binaries directly.

## Host-control mode reaches the packaged release, as an opt-in "experimental" launch path (GitHub issue #4 Phase F)

Closes the very last gap Phase E's entry above left open: everything
through Phase E worked, and was verified working, but only against the
standalone `melonds-remote-server`/`melonds-remote-client` binaries run
by hand or by test scripts -- the actual downloadable release never
shipped the standalone Host Service binary at all, and nothing in
`scripts/build-release.sh`'s generated launch scripts ever started it.
A user downloading a release and clicking the host launcher had no way
to reach host-control mode, full stop.

**What this implements**, all in `scripts/build-release.sh`'s generated
scripts (no application code changed):

- The standalone `melonds-remote-server` binary now ships in every
  release, at `host/internal/melonds-remote-server`.
- `melonds-remote-host.sh` gained a new menu choice, "Launch with
  host-control mode (experimental)", alongside the existing "Launch
  melonDS now". Deliberately **not** the default and deliberately
  labeled experimental -- see "What this does not do" below for why.
  Picking it prompts for a shared secret (kdialog input box, or a plain
  terminal prompt without kdialog) and passes it through as
  `MELONDS_REMOTE_AUTH_TOKEN`.
- `host/internal/run-host.sh`, when `MELONDS_REMOTE_HOST_CONTROL=1` is
  set, starts `melonds-remote-server --adapter-ipc` in the background
  (a per-user Unix-domain socket under
  `~/.config/melonds-remote/run/adapter.sock`), waits briefly for it to
  bind, then launches melonDS with `MELONDS_REMOTE_OUT_OF_PROCESS=1` and
  `MELONDS_REMOTE_ADAPTER_SOCKET` pointed at that socket -- exactly the
  env-var contract `EmuInstance::startRemoteServer()` (Phase A) already
  implements, just finally something in the packaged product actually
  sets it. A shell `trap ... EXIT` kills the background Host Service
  once melonDS exits, so nothing is left running behind it; getting this
  right required *not* `exec`-ing melonDS in this branch (an `exec`
  replaces the shell process image, traps and all, which would have
  orphaned the Host Service instead of cleaning it up).
- `host/internal/install-host-distrobox.sh` (the Bazzite/immutable-system
  path) explicitly rejects `MELONDS_REMOTE_HOST_CONTROL=1` with a clear
  error rather than silently falling back to ordinary in-process mode --
  see "What this does not do" below.

**What this does not do, and why**: this is deliberately *not* the
default launch path, and deliberately requires a manually-entered shared
secret instead of just working like every other launch. The reason is a
real, pre-existing gap this phase does not attempt to solve under time
pressure: melonDS's interactive device-approval dialog (the normal,
default authentication flow) lives entirely inside melonDS's own process
and has no bridge to a Host Service running in a *different* process --
`docs/adr/0001-host-service-and-adapter-architecture.md`'s "What this
ADR does not decide yet" section already flagged this
("`RemoteServerBridge`'s ... `approveDevice()`/`denyDevice()` just
return `false`" in out-of-process mode). A static auth token sidesteps
that gap safely (the standalone server's `--auth-token` flag is the same
mechanism the in-process flow already supports as an alternative to
interactive approval), but building a real cross-process approval bridge
is its own, separate, nontrivial piece of work -- flipping this to the
default without one would have broken authentication for every existing
user on every normal launch, which this phase was not going to risk.
The Distrobox/Bazzite path is left out entirely for the same
not-under-time-pressure reason: it would need the Host Service running
*outside* the container with its socket shared *into* it, which hasn't
been built or tested at all.

**Verified**: `bash -n` on the full `scripts/build-release.sh` and on
each of the three modified generated scripts extracted in isolation
(`run-host.sh`, `install-host-distrobox.sh`, `melonds-remote-host.sh`).
A real functional run of the extracted `run-host.sh` against the actual
compiled `melonds-remote-server` binary and a stub `melonDS` executable
in a fake `$HOME`: confirmed the Host Service actually starts, the
adapter socket is created and is a real Unix socket by the time melonDS
sees it, `MELONDS_REMOTE_ENABLE`/`MELONDS_REMOTE_OUT_OF_PROCESS`/
`MELONDS_REMOTE_ADAPTER_SOCKET` all reach melonDS's environment
correctly, and -- the part that actually needed a real test, not just
reading the script -- the background Host Service process is genuinely
gone (checked with `pgrep`, not just "the script exited 0") after the
stub melonDS exits, proving the `EXIT` trap cleanup actually works and
nothing gets left running. Also verified both error paths (missing
`MELONDS_REMOTE_AUTH_TOKEN`, missing the packaged binary) exit non-zero
with a clear message instead of a confusing failure further down, and
that the Distrobox path's rejection fires correctly when
`MELONDS_REMOTE_HOST_CONTROL=1` is set on a faked immutable system.

**A real, if narrow, edge case this testing surfaced**: the adapter
socket path (`~/.config/melonds-remote/run/adapter.sock`) is a Unix
domain socket, which has a kernel-enforced path-length limit (`sun_path`,
typically 108 bytes on Linux). The very first test run, against a fake
`$HOME` nested many directories deep inside this sandbox's own scratch
space, hit that limit and failed with a clear
`AdapterIpcServer: socket path too long` error (an existing, correct
check from Phase A/Phase 2 -- not something this phase had to add). A
real installation's `$HOME` (e.g. `/home/deck/...` on a Steam Deck) is
nowhere near long enough to hit this in practice, and the failure mode
is a clear error rather than a crash or silent corruption either way,
so this isn't fixed here -- just recorded in case an unusually deep or
long `$HOME` path is ever reported as a real bug against this feature.

## AzaharAdapter: a second emulator (Nintendo 3DS), and a host launcher that no longer assumes melonDS

Answers two direct user questions after issue #4 shipped: "is the server
now independent of melonDS like planned?" (partially -- see the Phase F
entry above for the honest nuance) and "can we add 3DS support?" This
entry covers the follow-up work that started from
`docs/azahar-integration-analysis.md`'s Phase 0 investigation: a real
`AzaharAdapter` implementing this project's existing, unchanged
`IEmulatorAdapter` contract (issue #28), and a host launcher reworked so
it no longer boots straight into melonDS.

**What this implements**:

- **`AzaharAdapter`** (`src/citra_qt/remote_server/AzaharAdapter.{h,cpp}`
  in the new `host/azahar-patches/0001-remote-server-integration.patch`):
  video via `VideoCore::RendererBase::RequestScreenshot()` on a
  bottom-screen-only `Layout::SingleFrameLayout(320, 240, swapped=true,
  upright=false)` (native 3DS touch-screen resolution, backend-agnostic
  across the software/OpenGL/Vulkan renderers -- see the analysis doc's
  section 1), polled on its own thread at ~30fps (not 60 -- see the
  class's own comment on why that's a conservative starting point, not
  a measured optimum, since no display/GPU stack exists in this sandbox
  to measure against). Input via a registered `Input::Factory` engine
  (`"melonds_remote"`) for the 12 buttons DS and 3DS share physically
  (confirmed identical ordering by reading `Settings::NativeButton`),
  both analog sticks (circle pad + New3DS-only c-stick, both already
  forwarded end-to-end since `protocol.h`'s `ControllerState` already
  had `leftStickX/Y`/`rightStickX/Y` fields with nothing consuming them
  for DS), and touch (reusing `protocol.h`'s `touchX/Y` as a
  proportional 0..1 position within whatever surface it's actually
  addressed to, not literally "DS pixel space" -- see
  `AzaharAdapter::applyGenericInput()`'s comment). No protocol changes
  were needed for any of this -- confirmed by reading
  `host/adapter_bridge.cpp`'s existing `dsButtonsToGenericButtons()`
  table and `ControllerState`'s fields before writing any new code, not
  assumed.
- **Out-of-process only, deliberately**: unlike melonDS's own default
  in-process mode (its own `NetServer` + interactive device-approval
  dialog), Azahar's integration only ever connects out to an
  already-running standalone Host Service via `AdapterIpcClient` --
  the exact mechanism issue #4 Phase A built for melonDS's *opt-in*
  out-of-process mode, now Azahar's *only* mode. Reimplementing
  `NetServer`'s device-approval Qt dialog a second time for a second
  emulator, on a first integration, under the same goal that also asked
  for the launcher rework and the custom-emulator-patching feature
  below, was not worth the scope -- a static shared secret
  (`AZAHAR_REMOTE_AUTH_TOKEN`) is required instead, the same trade-off
  already made and shipped for host-control mode (Phase F).
- **Host launcher rework**: `melonds-remote-host.sh`'s "Launch melonDS
  now" choice is now "Launch...", which opens a picker -- Nintendo DS
  (melonDS), Nintendo 3DS (Azahar, prompts for the shared secret), Host
  control only (unchanged from Phase F), or Custom. `run-host.sh`
  (melonDS) and the new `run-host-azahar.sh` are otherwise unchanged/
  new-but-parallel -- picking DS still goes through the exact same
  Distrobox-vs-plain dispatch (`launch-host.sh`) Phase F already
  verified; Azahar has no Distrobox path yet (see "Still open" below)
  and is launched directly.
- **"Patch my own emulator" feature**: `scripts/patch-existing-emulator.sh`
  (repo-level, run from a terminal) applies either patch to a
  user-supplied existing melonDS or Azahar git checkout (`git apply`,
  with a same-commit warning-not-block check, since the patch is
  generated against one pinned commit and may not apply cleanly, or --
  worse -- apply with unintended differences, against a different one)
  and optionally builds it. `host/internal/launch-custom-emulator.sh`
  (packaged) then remembers the resulting binary's path and system type
  in `~/.config/melonds-remote/custom-emulator.conf` and launches it
  through the menu's "Custom" choice from then on, using the same
  env-var contract as the bundled binaries (`MELONDS_REMOTE_ENABLE` for
  a DS-based custom build; the same out-of-process
  `AZAHAR_REMOTE_*`-and-Host-Service wiring as `run-host-azahar.sh` for
  a 3DS-based one) -- for anyone who already has an emulator set up the
  way they like it and doesn't want a separate DualDeck-managed copy
  alongside it.
- **`scripts/build-release.sh`/`.github/workflows/release.yml`**: Azahar
  is now cloned, patched, and built the same way melonDS is (a new
  `[3/5]` step), and packaged as `host/azahar` (top-level, alongside
  `host/melonDS`) plus its own
  `host/internal/azahar-shared-library-dependencies.txt`. Cached across
  CI runs by pinned commit (`actions/cache`, mirroring SDL3's own
  existing cache step) since Azahar's build is far heavier than
  melonDS's (36 git submodules -- Vulkan, boost, dynarmic,
  spirv-tools, etc., for real 3D rendering instead of the DS's mostly
  software-rendered 2D) and would otherwise add a large amount of time
  to every single release build, not just ones that touch Azahar code.

**Verified**:

- A real, from-scratch build of the patched Azahar in this sandbox
  (clone at the pinned commit, all 36 submodules fetched shallow,
  `AzaharAdapter`/`RemoteServerBridge` compiled as part of `citra_qt`)
  -- Qt6, Vulkan headers/loader, and boost were all already present in
  this sandbox (left over from earlier work in this same session),
  which made this possible at all; a genuinely clean machine would need
  `scripts/build-release.sh`'s new `ensure_packages "azahar build"`
  call to actually install them first, which this particular sandbox
  build did not exercise (dependencies were already satisfied).
- `bash -n` on the full `scripts/build-release.sh` and on each of the
  newly-generated scripts extracted in isolation (`run-host-azahar.sh`,
  `launch-custom-emulator.sh`, the reworked `melonds-remote-host.sh`).
- A real functional run of `launch-custom-emulator.sh`'s DS path (a
  stub binary, config file written and reused correctly on a second
  run) and its 3DS path (a stub binary + the real
  `melonds-remote-server` binary: confirmed the Host Service starts,
  the adapter socket is real by the time the stub sees it, the right
  `AZAHAR_REMOTE_*` env vars reach it, and -- checked with `pgrep`, not
  just exit code -- the Host Service is genuinely gone after the stub
  exits).
- `scripts/patch-existing-emulator.sh` run for real against this
  session's own already-patched melonDS scratch clone: correctly
  detected "patch already applied, nothing to do" rather than either
  erroring or silently double-applying.

**Still open**:

- **No real 3DS game was ever tested against this** -- no display, no
  GPU stack, and no ROM available in this sandbox (the same category of
  gap `docs/known-limitations.md` already records for melonDS's own
  real-hardware-only pieces, e.g. uinput). `RequestScreenshot()`'s real
  per-frame cost against an actual running game, whether ~30fps is
  actually a reasonable rate or too aggressive/too conservative, and
  whether the touch/circle-pad/button mapping actually feels right in
  practice are all genuinely unverified, not just "verified elsewhere
  and not re-checked here."
- **`scripts/build-release.sh`'s own Azahar step was not re-run
  end-to-end in this sandbox** -- it uses the exact same clone/checkout/
  submodule/`git apply`/`cmake` commands already verified via the
  standalone scratch build above, but running the *packaging script
  itself* (which would re-clone and re-build Azahar a second time) was
  not done, purely for time -- the underlying build commands are
  proven, the script wiring around them is `bash -n`-clean but not
  execution-tested as a whole.
- **No Distrobox/immutable-system path for Azahar** -- `run-host-azahar.sh`
  warns (does not block) on an apparent immutable system and just tries
  anyway, unlike melonDS's real `install-host-distrobox.sh`. Building
  that (Azahar running inside a container, reaching a Host Service
  running outside it) is real, separate work, matching the same
  "don't rush a second implementation of something nontrivial" reasoning
  already applied to skipping in-process device-approval for Azahar
  above.
- **Fedora/Arch runtime and build package names for Azahar are
  unverified** -- only Debian/Ubuntu package names in both
  `ensure_packages "azahar build"` (build-time) and
  `run-host-azahar.sh`'s `ensure_packages "azahar runtime"`
  (runtime) were checked against this actual sandbox; the others are
  reasonable best guesses, not confirmed installs, matching how several
  earlier `ensure_packages` calls in this project already carry the
  same caveat for less-common distros.
- **New3DS-exclusive `ZL`/`ZR` shoulder buttons have no wire
  representation** -- `protocol.h`'s `DSButton`/`ControllerState` have
  no bits for them (base-model 3DS/2DS never had them either), so a
  New3DS game that specifically requires them can't be fully controlled
  yet. The c-stick itself does work (mapped onto the already-existing,
  previously-unused `rightStickX/Y` wire fields).
- **This is still opt-in and labeled experimental everywhere it
  appears** -- deliberately: a first 3DS integration, verified only via
  compilation and scripted stub tests in a sandbox with no display, is
  not something to default users into.

## Atomic updates: Steam no longer needs to be closed to update

**The problem, reported by the user**: applying an update ("Check for
updates" from either menu, or the client's silent auto-update on
launch) always ended with a "Restart Steam" instruction, and in the
worst case an update applied while Steam was running could be silently
undone. Tracing this to its root cause: `apply-update.sh` (both client
and host) always finished by re-running `install-steam-shortcut.sh
--force`, and `steam_shortcut.py` *unconditionally* rewrote
`shortcuts.vdf` on every such run -- even though a routine version
update never actually changes the shortcut's Exe, AppName, StartDir, or
LaunchOptions (they're all derived from the fixed central install
directory, not the release version). `steam_shortcut.py`'s own
docstring already documented the resulting risk: Steam caches
`shortcuts.vdf` in memory and "can silently overwrite this script's
change on its next save" -- which is exactly why "restart Steam" was
the standing advice, and why an update applied by the *silent*
auto-update-on-launch path (which never showed that advice at all) was
riskier still.

**The fix**: `scripts/lib/steam_shortcut.py` now checks, before doing
anything else, whether the shortcut already has exactly the fields a
write would produce (`shortcut_up_to_date()`, comparing against
`find_matching_entry()`'s result). If it does -- the case for every
ordinary update -- `shortcuts.vdf` is never opened for writing at all,
and the Steam-running/`--force` safety gate doesn't even apply, since
there's nothing at risk of being clobbered. This makes updates
genuinely atomic with respect to Steam: the only thing that changes on
disk is the already-safe stage-then-rename swap of the installed
program files in `~/.config/melonds-remote-client/install/` (or the
host equivalent), which Linux handles correctly even while the running
process has the old files open. `--dry-run` was also fixed to never
need `--force` (it never writes regardless of whether Steam is running
-- a pre-existing minor inconsistency, fixed as part of the same
restructuring). A genuine change (e.g. different `--launch-options`,
or migrating a stale pre-central-install-dir shortcut via the AppName
fallback) still goes through the exact same Steam-running/`--force`
gate as before -- this only removes the check for the common no-op case,
it doesn't weaken the protection for a real write.

`scripts/build-release.sh`'s "Check for updates" menu choice (both
client and host) now captures `apply-update.sh`'s own output and only
shows the "Restart Steam" message when that output actually mentions
writing the file -- so the common case now just says "Updated to
vX.Y.Z." with no restart instruction. `docs/steam-deck-setup.md` and
`docs/bazzite-host-setup.md` updated to match; **Add to Steam**
(genuinely creating a brand-new shortcut) still recommends closing
Steam first, since that's an intentional, real write every time.

**Verified**: manual testing against a fake `$HOME` and a fake Steam
userdata directory, following this project's established convention for
`steam_shortcut.py` (no automated test suite exists for it -- see the
Steam-shortcut entries above): a fresh install writes as before; running
again with identical arguments while a process literally named `steam`
was running (spawned as a same-named script so `pgrep -x steam` matches
it) succeeded with exit 0, left `shortcuts.vdf` byte-for-byte unchanged,
and required no `--force`; a genuine change (different
`--launch-options`) under the same "Steam running" condition was still
correctly refused without `--force`, and succeeded once `--force` was
given; `--remove` run twice was idempotent (second run: "nothing to
remove", exit 0, no Steam gate); the pre-existing AppName-fallback
migration path (stale `--exe`, matching `--name`) still correctly
detects a change is needed and writes. `bash -n` on the full
`scripts/build-release.sh` and each of its 15 heredoc-embedded scripts
individually, plus `python3 -m py_compile` on `steam_shortcut.py`, all
pass.

## Fix: the Steam shortcut still booted straight into melonDS, bypassing the new launcher

**The problem, reported by the user**: after the AzaharAdapter/launcher
rework above shipped (v0.1.37), the host's Steam Big Picture/Gaming
Mode shortcut still launched straight into melonDS with no "Which
system?" picker at all -- as if that rework had never happened for the
Steam-launched path specifically. Double-clicking
`host/melonds-remote-host.sh` directly showed the new picker correctly;
only the Steam shortcut didn't.

**Root cause**: the rework changed `melonds-remote-host.sh`'s own
"Launch..." menu choice to call the picker, but never touched what the
Steam shortcut's `Exe` actually points at. `install-steam-shortcut.sh`
(`scripts/build-release.sh`'s packaged heredoc) was still registering
`Exe` as `internal/launch-host.sh` -- an older, melonDS-only entry point
that predates the picker and has no knowledge of it at all (it just
picks Distrobox-vs-direct-launch for melonDS specifically). Since
`Exe` is what Steam actually runs, every Steam-launched session skipped
the picker entirely, regardless of what `melonds-remote-host.sh` itself
now did.

**The fix**: both `install-steam-shortcut.sh` and
`uninstall-steam-shortcut.sh` (in `scripts/build-release.sh`'s packaged
heredocs) now register/match `Exe` as `melonds-remote-host.sh` itself --
the same entry point a double-click runs -- instead of
`internal/launch-host.sh`. Picking "Nintendo DS (melonDS)" from the
resulting picker still dispatches to `internal/launch-host.sh` exactly
as before, so the Distrobox-vs-direct-launch logic for melonDS itself is
unchanged; only what Steam's shortcut points *at* changed.

**Why this genuinely requires one Steam restart, and always will for a
change like this**: `scripts/lib/steam_shortcut.py`'s `find_matching_entry()`
matches an existing shortcut by `Exe` *or* `AppName` (see "Atomic
updates" above), so an already-installed shortcut (`AppName` = "melonDS
Remote Host", stale `Exe`) is found via the `AppName` fallback and its
`Exe` is corrected in place automatically the next time "Add to Steam"
or "Check for updates" runs -- no manual remove/re-add needed. But
that's a genuine field change, not a no-op, so it correctly falls
outside the no-restart-needed case the "Atomic updates" fix above
covers: Steam only reads `shortcuts.vdf` from disk at its own startup
and can silently overwrite an in-place edit from its in-memory cache
otherwise, so picking up *any* real `Exe`/`AppName`/`LaunchOptions`
change -- this one included -- has always needed a Steam restart (or
switch to Gaming Mode) and still does; nothing about that is fixable
from this project's side. This is a one-time cost for anyone who
already had the shortcut installed before this fix: once their `Exe` is
corrected, routine version updates go back to needing no restart at all,
same as before.

**Verified**: manual testing against a fake `$HOME` and fake Steam
userdata directory, same convention as the other `steam_shortcut.py`
entries in this file. A fresh install registers `Exe` as
`.../install/melonds-remote-host.sh` directly (not `internal/launch-host.sh`).
Simulating a pre-fix install (`AppName` = "melonDS Remote Host", `Exe` =
`.../install/internal/launch-host.sh`) and then re-running
`install-steam-shortcut.sh` correctly detects the change via the
`AppName` fallback, corrects `Exe` in place (still one entry, not a
duplicate), and is correctly refused without `--force` while a
same-named `steam` process is running -- succeeding once `--force` is
passed, matching every other genuine-change case already covered by the
"Atomic updates" fix. `bash -n` on the full `scripts/build-release.sh`
passes. Not yet tested against a real Steam client's Big Picture/Gaming
Mode UI in this sandbox (no Steam installation available here) -- same
caveat as every other Steam-shortcut entry in this file.

## The shared secret (3DS/host-control/custom-3DS) is now auto-generated instead of hand-typed

**The problem, reported by the user**: the AzaharAdapter and
host-control-mode shared-secret prompts (`get_or_create_shared_token`,
formerly `prompt_for_shared_token`) originally just asked the user to
type a value with no guidance -- fine for a keyboard, bad for a
controller-only Steam Deck, and nothing stopped someone from typing
something short and easily guessed.

**The fix**: `melonds-remote-host.sh`'s "Launch..." picker no longer
prompts for a value at all when Nintendo 3DS or host-control mode is
chosen. It generates a random 32-character hex token
(`python3 -c "import secrets; print(secrets.token_hex(16))"`, with an
`openssl rand -hex 16` / `/dev/urandom` fallback chain in case python3
is somehow missing) the first time either mode is used, and persists it
at `~/.config/melonds-remote/shared-token.conf` (`chmod 600`) so it
survives across relaunches -- without persistence, every relaunch would
silently invalidate whatever a client already had configured in its
`--auth-token`. One token covers both modes, since they're not meant to
run at the same time. Every time either mode is launched, the current
token is shown (a `kdialog` message box, or a plain terminal echo)
so it can be copied into the client's `--auth-token` (Launch Options or
a manual invocation) -- necessary precisely because a random value
can't be memorized the way a hand-picked one might be.

`scripts/build-release.sh`'s `launch-custom-emulator.sh` (the "patch my
own emulator" feature) got the same treatment for its 3DS case: it used
to prompt for a token once and store it in `custom-emulator.conf`
alongside the binary path/type; it now generates one the same way,
still persisted in the same config file, and shows it on every launch
rather than only when first configured (the config file itself is still
loaded silently thereafter for the path/type fields, same as before --
only the token gained the "show every time" behavior, since it's the
one field of that file a user actually needs to go copy elsewhere).

**Verified**: extracted both heredocs from `scripts/build-release.sh`
and ran their token-handling logic directly against a fake `$HOME`.
Confirmed the generated value is exactly 32 lowercase hex characters,
that a second call (simulating host-control mode being launched right
after 3DS mode, or the host being relaunched later) returns the
byte-identical token rather than generating a new one, and that the
persisted file is `chmod 600`. `bash -n` on the full
`scripts/build-release.sh` and both extracted heredocs passes.

## Azahar crashing when browsing files to pick a ROM (code-level fix applied, not conclusively verified)

**The problem, reported by the user**: Azahar kept crashing specifically
when browsing for a ROM (File > Load File's file-picker dialog).

**First hypothesis (wrong)**: a well-documented, broader class of Qt6
bug where native file dialogs crash due to a GTK3 platform-theme
integration bug in window parenting. `QT_QPA_PLATFORMTHEME=""` was
applied to both `internal/run-host-azahar.sh` and
`internal/launch-custom-emulator.sh`'s Azahar case as the standard
workaround for that bug class. **This did not fix it** -- the user
confirmed the exact same crash persisted on the release with that fix
applied, and also persisted after manually trying `QT_QPA_PLATFORMTHEME`
set to two other placeholder values and to `gtk3` (to force a non-KDE
theme), even when bypassing Steam entirely and running the binary
directly from a terminal. All four attempts crashed identically,
ruling out platform-theme selection as the actual lever.

**Real diagnosis, from a real crash backtrace**: the user captured a
`coredumpctl gdb` backtrace of the actual SIGSEGV. It shows:

```
QFileDialog::getOpenFileName -> QFileDialog::getOpenFileUrl -> QDialog::exec()
  -> KIO::FilePreviewJob::slotStatFile(KJob*)
    -> QCryptographicHash::QCryptographicHash(Algorithm)
      -> QCryptographicHashPrivate::EVP::EVP(Algorithm)
        -> [null function pointer] -- SIGSEGV
```

This is KDE Frameworks' file-preview/thumbnail generation
(`KIO::FilePreviewJob`, part of `libKF6KIOGui.so.6`/`libKF6KIOCore.so.6`
on the user's Fedora system) crashing inside Qt6's own
`libQt6Core.so.6`, several frames below any code this project's patch
controls -- `OnMenuLoadFile()` (frame near the top of the stack) is the
only DualDeck-adjacent frame, and it's Azahar's own unmodified code at
that point, calling the same `QFileDialog::getOpenFileName` upstream
Azahar has always called. Crucially, the DualDeck-added
AzaharAdapter/RemoteServerBridge background threads don't exist yet at
the moment of this crash either -- they're only constructed in
`BootGame()`, which runs *after* a ROM is chosen, and this crash happens
*while choosing one*. So this is not a regression, race condition, or
resource-contention effect introduced by this project's patch; the
crash is entirely inside Fedora's own system Qt6/KDE-Frameworks
packages, several frames beneath where DualDeck's code or Azahar's own
code has any influence.

**Fix applied**: rather than continue guessing at environment
variables (four attempts already failed), `OnMenuLoadFile()`
(`src/citra_qt/citra_qt.cpp`) was patched to construct an explicit
`QFileDialog` object instead of using the `QFileDialog::getOpenFileName`
convenience static, and calls `setIconProvider()` with a plain
`QFileIconProvider` before executing it. This forces Qt to use its own
built-in icon/preview logic for the dialog's file listing instead of
whatever icon provider the platform theme would otherwise supply --
bypassing the path that reaches `KIO::FilePreviewJob` entirely,
regardless of which theme Qt's fallback chain ends up selecting (which
is also why the `QT_QPA_PLATFORMTHEME` attempts likely never mattered:
Qt's own theme-selection fallback logic evidently still reaches the
same KDE integration when an explicitly-requested theme name fails to
construct, rather than genuinely disabling it). `QFileIconProvider`
isn't a `QObject`, so it's kept as a stack variable declared before the
`QFileDialog` object (rather than heap-allocated), guaranteeing it
outlives the dialog via C++'s reverse-order destruction rather than
relying on unclear ownership semantics from `setIconProvider()`.

**Verified**: the patched `citra_qt.cpp` compiles cleanly (confirmed via
a real incremental rebuild -- `0` compiler errors, the new code's
symbols present in the resulting binary via `nm`), and the regenerated
`host/azahar-patches/0001-remote-server-integration.patch` still applies
cleanly to a fresh, pristine checkout of the pinned commit.

**Still open -- this is the important caveat**: this has **not** been
confirmed to actually stop the crash on real hardware. This sandbox has
no display or GPU, so there is no way to open Azahar's GUI and click
"Load File" here to observe the fix work. The diagnosis (KIO preview
generation reaching a crash inside Qt's OpenSSL-backed
`QCryptographicHash`) is based on a real backtrace, not speculation, and
the fix directly targets the mechanism that backtrace shows triggering
the preview generation in the first place -- but only testing on the
actual affected machine can confirm it. If it still crashes after this
fix ships, the next most likely explanation is that Qt/KDE's icon
provider override doesn't fully suppress `KIO::FilePreviewJob` the way
its API contract suggests it should on this specific Qt6/KDE Frameworks
version, in which case the underlying bug (a null function pointer
inside `QCryptographicHashPrivate::EVP`, most plausibly an OpenSSL
provider/version issue on the affected Fedora system, though the
system's crypto-policy was confirmed `DEFAULT`, not `FIPS`) would need
a fix from Fedora's own Qt6/KDE-Frameworks/OpenSSL packaging, not
something patchable from DualDeck's side -- worth checking `sudo dnf
update` (especially `qt6-qtbase`, `kf6-kio`, `openssl`) and testing
whether the same crash reproduces on an unrelated, unmodified Azahar
build on the same machine, which would conclusively confirm it's a
system-level issue independent of this project entirely.

**A second, likely-related lead**: the user also reported an SELinux
AVC denial notification (setroubleshoot popup) appearing at the same
time as the crash. This fits the OpenSSL-EVP-null-pointer diagnosis
well: if SELinux denies the process access to an OpenSSL provider
module (e.g. under `/usr/lib64/ossl-modules/`) or its config
(`/etc/pki/tls/openssl.cnf`) -- plausible for a binary launched from a
non-standard, user-home install path
(`~/.config/melonds-remote/install/azahar`) rather than a normal
system-package location -- `EVP_MD_fetch()`/similar could fail and
return null, which Qt's `QCryptographicHash` wrapper doesn't appear to
null-check before calling through, matching the crash exactly. Not yet
confirmed (waiting on the actual AVC denial detail, via
`sudo ausearch -m avc -ts recent` or `sudo sealert -a
/var/log/audit/audit.log`), but if confirmed, it would mean the
`QFileIconProvider` fix above works by luck (avoiding the one code path
that happens to reach the vulnerable call) rather than by addressing
the actual root cause -- any other code path in Azahar that calls
`QCryptographicHash` while SELinux is blocking the same access would
still crash the same way. If that's what the AVC denial confirms, the
real fix would be either an SELinux policy module allowing the
installed binary's context to load OpenSSL's provider modules, or
running `restorecon` on the install directory in case a `cp`-based copy
step lost the correct SELinux context along the way.

## Azahar's build cache was silently serving stale, unpatched binaries across two releases

**The bug**: v0.1.39 and v0.1.40's `host/azahar` binaries turned out to
be byte-for-byte identical (`md5sum` confirmed), despite the source
patch changing between them (the `QT_QPA_PLATFORMTHEME` launcher env
var in v0.1.39, then the real `OnMenuLoadFile()`/`QFileIconProvider`
code fix in v0.1.40) -- meaning **the second fix was never actually
built or shipped**, even though CI reported success and the release
notes named the right commit.

**Root cause**: `.github/workflows/release.yml`'s `actions/cache` step
for Azahar's build keyed its cache **only** on the pinned upstream
commit (`azahar-75134fca...-v1`), which never changes when this
project's own patch file does. Once the first successful build was
cached, every later run restored that same cached `.release-work/
azahar-src` directory (source tree, already patched with whatever
version of the patch existed at caching time, plus its already-built
binary) -- and `scripts/build-release.sh`'s own cache-hit check only
tested whether a binary existed at that path
(`[[ -f ".../build/bin/Release/azahar" ]]`), not whether it actually
matched the current patch. So every subsequent CI run, no matter how
the patch changed, silently skipped re-cloning, re-patching, and
rebuilding entirely -- confirmed by the "Build release package" step's
own duration (~3 minutes for v0.1.40, far too short for Azahar's real
20-40+ minute build from scratch).

**The fix**: both halves of the caching now key on the patch file's
actual content, not just the pinned commit:
- `release.yml`'s cache key gained
  `${{ hashFiles('host/azahar-patches/0001-remote-server-integration.patch') }}`
  (bumped `v1` to `v2` too, to guarantee a clean break from the
  already-poisoned old cache entries).
- `build-release.sh`'s local cache-hit check now runs
  `git apply --reverse --check` with the *current* patch file against
  the cached tree -- the same idempotency technique
  `scripts/patch-existing-emulator.sh` already uses to detect
  already-applied vs. needs-reapplying. Only a tree that reverse-applies
  cleanly (meaning it currently has *exactly* this patch, not some
  earlier version of it) counts as a cache hit; anything else triggers
  a full re-clone, re-patch, and rebuild.

**Verified**: tested both the true-positive case (current patch against
the scratch tree it was authored against: correctly reports a match)
and the true-negative case (reverting one patched file back to its
pristine pre-patch state via `git checkout HEAD --`, simulating a stale
cache holding an older patch version: correctly reports no match,
triggering a rebuild) directly with `git apply --reverse --check`.
`bash -n` on the updated `build-release.sh` passes.

**Practical consequence for this session**: the v0.1.40 release the user
tested does **not** actually contain the `QFileIconProvider` crash fix
-- it's running the exact same pre-fix binary as v0.1.39. A new release
built after this caching fix is the first one that will actually
contain it.

## Azahar Load-File crash, continued: `setIconProvider()` alone wasn't enough

**The problem**: the user tested v0.1.41 (the first release confirmed
to genuinely contain the `QFileIconProvider` fix above, per a real
`md5sum` diff against the stale v0.1.39/v0.1.40 binary) -- it still
crashed identically. A real backtrace + coredump analysis (this time
independently cross-checked by the user against a second AI's review)
also conclusively ruled out the SELinux angle from the earlier entry:
the AVC denials present (`systemd_coredump_t`, `sd-parse-elf`,
`mounton`) are `systemd-coredump`'s own sandboxing while it processes
the crash dump *after* the crash already happened, not something
blocking Azahar itself.

**Why the first code fix didn't work**: constructing a `QFileDialog`
object directly and calling `.exec()` on it does **not**, by itself,
force Qt's fully generic dialog implementation -- Qt still substitutes
a native platform dialog helper unless `QFileDialog::DontUseNativeDialog`
is explicitly set as an option. On this system, that native helper is a
real `KFileWidget` (KDE's own native file browser widget, wrapping KIO's
preview system directly), which has its own separate, KIO-based
icon/preview mechanism entirely -- it doesn't consult
`QFileDialog::setIconProvider()` at all, since that call only affects
Qt's own generic dialog model, not the substituted native widget. So
the previous fix's `setIconProvider()` call was silently having no
effect: Qt was still handing the user a KDE-native dialog underneath,
identical to the unpatched behavior.

**The fix**: `OnMenuLoadFile()` now also calls
`dialog.setOption(QFileDialog::DontUseNativeDialog, true)` before
`.exec()`, in addition to the existing `setIconProvider()` call. This
forces Qt's own generic, non-native dialog widget, which has no
KDE/KIO integration to substitute in at all -- eliminating the
`KFileWidget`/`KIO::FilePreviewJob` code path entirely rather than
merely asking it (ineffectively) to use a different icon provider.

**Verified**: same discipline as the previous attempt -- confirmed via
a real incremental rebuild (`0` compiler errors, the new
`DontUseNativeDialog` reference and other symbols present in the
resulting binary via `nm`), and the regenerated patch still applies
cleanly to a fresh, pristine checkout of the pinned commit. **Not yet
confirmed to fix the crash on real hardware** -- same fundamental
limitation as every attempt before it: this sandbox has no display or
GPU, so there's no way to actually open Azahar's GUI and click "Load
File" here. This is the second attempt at a code-level fix for the same
symptom; if it still doesn't work, the next thing worth trying is
having the user run Azahar under `gdb` with a breakpoint on
`QFileDialog::exec` to directly inspect (via `p dialog.testOption(...)`
or similar) whether `DontUseNativeDialog` is actually taking effect at
runtime, since at this point empirical confirmation on the affected
machine is more valuable than another theory from here.

**Update: confirmed fixed.** The user tested v0.1.42 and confirmed the
Load File dialog no longer crashes and a ROM boots successfully. The
`DontUseNativeDialog` option was indeed the missing piece.

## Client silently hangs on "CONNECTING TO..." for a shared-secret auth failure

**The problem, reported by the user**: after successfully booting a
game in Azahar (the crash fix above), connecting the client got stuck
indefinitely on "CONNECTING TO xxx.xxx..." despite discovery correctly
showing the host's IP/hostname/emulator identity.

**Root cause**: discovery info (IP, hostname, system/adapter identity)
comes from the lightweight UDP broadcast response, which requires no
authentication at all -- seeing it doesn't mean the real TCP handshake
will succeed. `HelloRejectReason` already has a distinct
`AuthenticationFailed` value (`net_server.cpp` sets it via
`constantTimeEquals(hello->authToken, config_.authToken)` whenever the
host requires a static shared secret -- true for 3DS/host-control mode
-- and the client's token doesn't match, including the default case of
not passing `--auth-token` at all, which is what most likely happened
here since there's currently no in-app prompt for it). The client's
first-run setup wizard screen already had a distinct, correct message
for this (`"AUTHENTICATION FAILED"`, added when that wizard was built);
the *other* screen -- the normal discovery/reconnect loop most users
actually go through -- never got the same treatment. Its `switch`
only special-cased `ApprovalRequired` and `AppVersionMismatch`,
falling through to the generic "CONNECTING TO..." for every other
reject reason including `AuthenticationFailed`, `HostBusy`, and
`ProtocolVersionMismatch` -- indistinguishable from a genuine network
hang, and retrying forever wouldn't have fixed it since the token
itself needed to change, not the timing.

**The fix**: `client/src/main.cpp`'s main reconnect-loop status
`switch` now matches the wizard screen's cases:
`AuthenticationFailed` -> "AUTHENTICATION FAILED WITH \<host\> - CHECK
YOUR --auth-token", `ProtocolVersionMismatch` -> a distinct message
(previously fell through to the generic default), `HostBusy` -> "HOST
\<host\> IS BUSY - RETRYING...". `ApprovalRequired`/`AppVersionMismatch`
were already correct and unchanged.

**What the user needs to do right now** (until the client gains an
in-app way to enter this): pass the token shown by the host's picker
via `--auth-token <token>` when launching the client -- either as the
Steam shortcut's Launch Options, or as an argument to
`client/internal/run-client.sh` directly.

**Verified**: compiles cleanly and links (`melonds-remote-client`
target, real rebuild via CMake), full `ctest` suite passes (no
regressions). Not yet verified against a real host requiring
authentication in this sandbox (no way to run two networked processes
against a real Azahar/melonDS instance here), but the message logic
itself is a straightforward, low-risk `switch`-arm addition mirroring
code that's already shipped and working on the wizard screen.

## The shared secret is gone: Azahar/host-control/custom-3DS now get the same zero-typing kdialog approval melonDS's own dialog has

**The problem, from the user directly**: "it should be as seamless as
possible, I should not need to type on the steamdeck, can we make the
string the same based on the network SSID or something?" -- a direct
follow-up to the still-open gap recorded throughout this doc (Phase F,
AzaharAdapter, and the auto-generated-token section above): Azahar,
host-control mode, and custom 3DS emulators all required a static shared
secret (`AZAHAR_REMOTE_AUTH_TOKEN`/`MELONDS_REMOTE_AUTH_TOKEN`) because
their standalone Host Service process had no GUI surface to show an
approve/deny prompt on -- only a console (`approve <id>`/`deny <id>`/
`list` typed at stdin), invisible in Steam Gaming Mode.

Deriving the token from the network SSID was considered and rejected: it
isn't a real secret (anyone on the same Wi-Fi network already knows the
SSID), so it would have made authentication cosmetic rather than
removing the actual gap.

**The real fix**: `NetServerConfig::onPendingRequestsChanged` (an
existing hook, already used by melonDS's own in-process Qt Approve/Deny
dialog and by the console) is now also wired, in the standalone
`melonds-remote-server` binary itself
(`host/remote-server/src/main.cpp`), to a new
`KdialogApprovalHook`/`promptDeviceApprovalViaKdialog()`
(`host/remote-server/src/kdialog_approval_prompt.{h,cpp}`) that pops a
`kdialog --yesno` prompt on the host's own desktop the first time an
unrecognized device connects. Approving or denying calls the exact same
`NetServer::approveDevice()`/`denyDevice()` the console path already
used. This is wired up automatically whenever `--auth-token` is omitted
(device-approval mode, already the code's own documented default) --
`--auth-token` still works as an explicit opt-out for anyone who prefers
a static secret. `--state-dir` now points every mode
(`run-host.sh`'s host-control branch, `run-host-azahar.sh`,
`launch-custom-emulator.sh`'s 3DS case) at the same
`~/.config/melonds-remote/` directory melonDS's own in-process approval
already uses, so approving a device once covers every emulator, not just
the one it first connected through. `scripts/build-release.sh`'s
now-dead `generate_shared_token()`/`get_or_create_shared_token()`/
`generate_token()`/`show_token()` helpers and their forced-token prompts
were removed from the picker and launch scripts entirely.

**Why `fork()`+`execlp()`, never `system()`/`popen()`**: the prompt's
message embeds `clientName`/`address`, both attacker-controlled --
arbitrary strings the connecting device claims about itself. Passing the
formatted message as a single `execlp()` argument, with no shell in
between, means it can never be interpreted as a shell command regardless
of content. kdialog only (no zenity/GTK fallback), matching this
project's established convention -- SteamOS Desktop Mode and Bazzite are
both KDE Plasma.

**A real reentrancy deadlock caught before shipping**: the first draft
of `KdialogApprovalHook::promptAndDecide()` held its own `mutex_` while
calling `server_->approveDevice()`/`denyDevice()`. Those calls
synchronously invoke `DeviceApprovalManager::notifyChangedLocked()`,
which re-enters `onPendingRequestsChanged()` -- and thus tries to
re-lock the same, non-recursive `mutex_` -- on the very same thread.
Fixed by reading `server_` into a local and releasing the lock before
calling either method.

**What this does not do**: if `kdialog` isn't installed or isn't
reachable (headless SSH session, no `$DISPLAY`), the prompt result is
`Unavailable` and the request is left pending -- the console
`approve`/`deny`/`list` commands documented in `--help` still work as a
fallback, same as before this feature existed.

**Verified**: `interpretKdialogExitStatus()`'s pure exit-status logic is
unit-tested (`test_kdialog_approval_prompt.cpp`, all four cases: exit 0,
exit 1, other exit codes, abnormal termination) and passes under
`ctest`. The subprocess wrapper itself
(`promptDeviceApprovalViaKdialog()`) and the real end-to-end popup can
only be verified by hand on a real KDE desktop, which this sandbox
doesn't have -- not yet confirmed against a real Steam Deck.

## Azahar video never displays on the Vulkan renderer backend (real bug, found from real-hardware testing)

**The problem, reported by the user**: on a real Steam Deck, controls and
touch worked correctly through Azahar/DualDeck, but the client showed no
video at all. The user had already switched Azahar's graphics API to
Vulkan because OpenGL didn't render on their system (a real, separate,
and out-of-scope compatibility issue with OpenGL on their specific
hardware/driver stack).

**Root cause**: `AzaharAdapter::captureLoop()`
(`src/citra_qt/remote_server/AzaharAdapter.cpp`) misread the meaning of
`VideoCore::RendererBase::RequestScreenshot()`'s completion-callback
bool. It is **not** a success flag -- reading
`GRenderWindow::CaptureScreenshot()` (`bootmanager.cpp`, the built-in
"save a screenshot" feature that also calls `RequestScreenshot()`) shows
the bool is `invert_y`, passed straight to `GetMirroredImage()` before
saving. `RendererOpenGL::RenderScreenshot()`
(`renderer_opengl.cpp`) always calls the callback with `true` (OpenGL's
`glReadPixels()` returns bottom-up rows, needing a flip), while
`RendererVulkan::RenderScreenshot()` (`renderer_vulkan.cpp`) always
calls it with `false` (Vulkan's rows are already top-down -- no bug
there, it's correct for its own purpose). `captureLoop()` had been
storing that bool directly into a variable named `succeeded` and gating
the whole "keep this frame" branch on it (`if (done && succeeded)`) --
so on Vulkan, every single captured frame was silently discarded, while
on OpenGL every frame was kept but never actually had the needed
vertical flip applied to it, since nothing consumed the bool for that
purpose either. This was never caught earlier because this is the first
time anyone has run Azahar through DualDeck on real display/GPU
hardware -- every note prior to this in this doc about `AzaharAdapter`'s
video path says as much explicitly.

**The fix**: `captureLoop()` now treats the callback simply firing at
all as success (the one real failure case, `RequestScreenshot()`
silently ignoring an already-in-flight request, is still caught by the
existing bounded `doneCv.wait_for()` timing out with `done == false`).
The bool is renamed to `invertY` and used for its actual purpose: when
true, the captured BGRA buffer's rows are flipped in place before being
stored as `latestFrame_`, so the frame delivered to the client is
top-down regardless of which renderer backend captured it. This also
means OpenGL-backed video, if anyone had gotten a full end-to-end test
of it working, may have been upside-down until now -- this fix corrects
both the missing-video-on-Vulkan bug and that latent orientation bug in
one change, since they share the same root cause.

**Verified**: the regenerated `host/azahar-patches/0001-remote-server-integration.patch`
applies cleanly (`git apply --check`) against a fresh checkout of the
pinned Azahar commit, and the full `azahar` binary (CMake target
`citra_meta`) rebuilds successfully with the fix, confirmed via a real
incremental build in this sandbox. Not yet re-verified against real
Steam Deck hardware with either renderer backend -- this sandbox has no
display/GPU stack to actually render a 3DS game and inspect the
resulting video frames end-to-end.

## The real cause of "no Azahar video": the wire protocol was never generalized past DS's fixed 256x192, not a renderer-backend bug

**The problem, from the user, across two follow-ups**: after the
Vulkan `invert_y` fix above shipped (v0.1.45), video still didn't show
-- and, more tellingly, the user reported the Software renderer (their
only other option, since OpenGL doesn't render at all on their
hardware) *also* produced no video, despite being a completely
different code path. That second data point ruled out the renderer
backend as the real cause: something common to all three backends was
wrong, and it wasn't `AzaharAdapter`'s own capture logic at all.

**Root cause, found by reading the whole pipeline, not guessing
further**: this project's wire protocol, `IFrameSource` interface, and
client were never actually generalized past DS's fixed 256x192 bottom
screen, despite `AdapterBridge`'s own header already documenting this
as a known, deliberate limitation ("the wire protocol this feeds is
still fixed at the native 256x192 DS resolution") from before Azahar/3DS
existed. Concretely, three separate hardcoded assumptions, each on its
own enough to silently drop every 3DS frame regardless of what
`AzaharAdapter` correctly captured:

1. `host/remote-server/include/host/frame_source.h`'s `IFrameSource` had
   no concept of width/height at all -- `kFrameWidth`/`kFrameHeight`
   (256/192) were free-standing constants, not queryable per-source.
2. `net_server.cpp` never set `HelloAckPayload::nativeWidth/nativeHeight`
   (a field that already existed and was already serialized/parsed --
   just never populated), so it always defaulted to 256/192 in every
   HelloAck sent to every client, Azahar included.
3. The client had two independent, separately hardcoded 256x192
   assumptions: `net_client.cpp`'s `videoReceiveLoop()` rejected (and
   **closed the video connection entirely**) any packet whose size
   wasn't exactly `256*192*4` bytes, and `main.cpp` created its SDL
   texture once at a fixed 256x192 and never resized it.

A 3DS frame from `AzaharAdapter` is `320*240*4 = 307200` bytes --
completely different from DS's `196608` bytes -- so it failed check #3
on every single frame, regardless of which renderer backend produced
it, which is exactly why Vulkan, OpenGL (as far as it could be tested),
and Software all showed the identical symptom. Touch/controls worked
throughout because they were already correctly generalized in an
earlier phase (issue #28): `mapPointToDSCoords()` outputs a normalized
0..1-equivalent position via `kTouchMaxX/Y`, and `AzaharAdapter::
applyGenericInput()` already rescales that to its own surface's real
`touchRangeX/Y` -- video was simply never given the same treatment.

**The fix**: `IFrameSource` gained a `frameDimensions()` virtual
(default 256x192, so every pre-existing source needs no changes);
`AdapterBridge` overrides it with the target surface's actual declared
width/height (from `AdapterCapabilities`, already correctly populated by
`AzaharAdapter` all along -- nothing there needed to change);
`net_server.cpp` now populates `HelloAckPayload::nativeWidth/nativeHeight`
from it. On the client, `NetClient` gained `hostNativeWidth()`/
`hostNativeHeight()` (atomics, since `videoReceiveLoop()` reads them on
every packet); its payload-size check is now computed from those instead
of a fixed constant; `main.cpp`'s main session loop recreates its SDL
texture (and the matching test-pattern filler buffer) at the host's
actual reported dimensions the moment a connect edge is detected,
instead of assuming DS forever.

**What's still not covered**: the setup wizard's own connectivity-test
screen (`wizardVideoTest()` in `main.cpp`) still checks against a fixed
`kDSWidth`/`kDSHeight` and was deliberately left alone in this fix --
it's a first-run/reconfiguration flow, not the primary gameplay screen,
and generalizing it would mean threading a resizable texture through
`runSetupWizard()`'s whole call chain (`SDL_Texture*` passed by value
today, not by reference) for a screen that's rarely the one actually
being debugged. If a user's very first connection is to a 3DS host, the
wizard's own "NO VIDEO YET" screen will incorrectly stay stuck even once
a real frame is arriving -- the main session afterward is unaffected.

**Verified**: full `ctest` suite passes, including two new host-side
tests (`adapter_bridge_frame_dimensions_matches_ds_surface`/
`_matches_3ds_surface`, using the existing `FakeThreeDsAdapter` fixture's
real 320x240 declaration) and two new real end-to-end client-side tests
(`net_client_reports_host_native_dimensions_from_hello_ack`,
`net_client_receives_a_non_ds_sized_video_frame` -- a real `NetClient`
against a real `NetServer` over loopback sockets, proving a 320x240,
307200-byte frame is now actually delivered through
`NetClient::getLatestFrame()` end to end, where before this fix it would
have been silently dropped and the video connection closed). The SDL
texture-resize logic itself (`main.cpp`) has no automated test coverage
-- this sandbox has no display to actually run the client against --
so it's been read carefully and matches the same edge-detection pattern
the pre-existing, working identity-refresh code right next to it already
uses, but is not yet confirmed against a real Steam Deck.

## Azahar video still black even after the Vulkan/dimension fixes -- capture-loop diagnostics added

**Where this stands**: after both fixes above shipped, the user updated
and confirmed via the host's own log (`NetServer: stats -- ... video:
sent=0 (0.0 fps) dropped=0 ...`, unchanged across the whole session
despite `input: accepted=` climbing steadily) that **zero video frames
ever leave the host**, with a real game actually running in Azahar (not
just sitting at the game list). Since `dropped` never increments either
(it's only ever bumped by a frame-index gap in an already-flowing
stream, per `net_server.cpp`'s `videoLoop()`), this means
`AdapterBridge::getLatestFrame()` never once returned true for the
whole session -- tracing back through `AdapterIpcServer`'s stored
`latestFrames_` and `AdapterIpcClient::writeLoop()` (which only ever
sends a `Frame` message when `AzaharAdapter::latestFrame()` itself
returns true), this points at `AzaharAdapter::captureLoop()`'s own
`RequestScreenshot()` calls never completing at all on this user's
system -- a capture failure that exists *before* either of the two
previous fixes even come into play, and unrelated to both of them.

**Why this needed new instrumentation instead of another guess**:
`captureLoop()` had no logging whatsoever -- a total, permanent capture
failure (every single `RequestScreenshot()` call timing out after
500ms, forever) produces the exact same host-side symptom
(`video: sent=0`) as "the code isn't even trying," with nothing to tell
the two apart. Guessing at a third fix without that visibility would
have repeated the same mistake as the first two rounds.

**What was added**: `captureLoop()` now tracks attempts/successes/
timeouts and logs a summary line to Azahar's own stderr every 5 seconds,
e.g. `AzaharAdapter: capture stats -- attempts=150 succeeded=0
timed_out=150 (last invert_y=n/a)`. This is purely additive
instrumentation -- no behavior change, nothing this session's tests
could regress -- and directly answers the open question: is
`RequestScreenshot()`'s callback ever firing at all on this user's
hardware, or not.

**Next step**: waiting on the user to re-run with this build and share
the new `AzaharAdapter: capture stats --` lines. `timed_out` climbing
while `succeeded` stays at 0 would point at something inside Azahar's
own `RenderScreenshot()` path (Vulkan or Software, whichever they're
using) silently never invoking the completion callback on this specific
system/driver combination -- worth then checking Azahar's own upstream
issue tracker for known `RequestScreenshot()`/screenshot-capture bugs
independent of this project, since at that point it would no longer be
this project's own integration code at fault.

## Found it: AdapterBridge cached its target surface at construction, before any adapter had ever connected

**The data that cracked it**: with both diagnostics from the previous
entry live, the user's next test run showed the full picture at once:
`ModeCoordinator: switching to Emulation mode (system=Nintendo 3DS,
adapter=Azahar)` (mode switch confirmed working) immediately followed
by `AzaharAdapter: capture stats -- attempts=601 succeeded=601
timed_out=0` (Azahar's own Vulkan capture at a **perfect 100% success
rate** -- the invert_y fix holds up completely) -- yet
`NetServer: stats -- ... video: sent=0 (0.0 fps) dropped=0 ...`
unchanged for the whole session. Real frames, successfully captured,
correctly routed to Emulation mode, and still zero ever left the host.

**Root cause**: `AdapterBridge`'s constructor picked and cached its
target surface ID (and, since the earlier dimension fix, its
width/height) from `adapter.capabilities()` a single time, at
construction. Its own header comment even documented this as a real
precondition: *"`adapter` must already have valid capabilities() ...
by the time this constructor runs."* That was true for the mode this
class was originally written for (issue #28 Phase 2's `--adapter-socket`
direct mode, where a local adapter connects before the bridge is ever
built) -- but issue #4 Phase A/D's `--adapter-ipc` coordinator flow
(what host-control mode and Azahar both actually use) constructs
`AdapterBridge` unconditionally at process startup
(`host/remote-server/src/main.cpp`, right after
`melonds_remote::host::AdapterBridge bridge(adapterServer);`), long
before any real adapter has connected over IPC. At that moment
`adapterServer.capabilities()` has an empty `surfaces` list, so
`pickTargetSurface()` returned `""` and `AdapterBridge::targetSurfaceId_`
was locked to that empty string **forever** -- there was no mechanism to
ever re-pick it once a real adapter (with a real surface ID like
`"bottom"`) actually connected later.

`getLatestFrame()` then always called `adapter_.latestFrame("", frame)`
-- an empty string that could never match `AdapterIpcServer`'s
`latestFrames_["bottom"]` map, so it always returned false, regardless
of how successfully Azahar was capturing. Input/buttons kept working
because they don't depend on surface ID at all; touch was subtly also
broken the same way (every touch contact was tagged with surface ID
`""` instead of `"bottom"`), just not something a manual test would
easily notice with only one surface in play.

**The fix**: `AdapterBridge` no longer caches anything from
`capabilities()` at construction. `targetSurfaceId()`,
`getLatestFrame()`, and `frameDimensions()` all re-resolve the target
surface fresh from `adapter_.capabilities()` on every call -- cheap
(a mutex-guarded struct copy, not an IPC round-trip, since
`AdapterIpcServer::capabilities()` already just returns its own
locally-cached copy) and correct regardless of when, or whether, an
adapter has connected yet.

**Verified**: a new fake (`LateConnectingAdapter` in
`test_adapter_bridge.cpp`) starts with zero surfaces and only gets a
real "bottom" surface via an explicit `connectNow()` call made *after*
`AdapterBridge` is constructed -- deliberately reproducing the exact
issue #4 `--adapter-ipc` startup ordering that
`FakeDsAdapter`/`FakeThreeDsAdapter` (both fully populated from their
own construction) never could have caught. Three new tests
(`adapter_bridge_resolves_surface_after_late_adapter_connection`,
`_frame_dimensions_after_late_adapter_connection`,
`_touch_uses_live_surface_id_after_late_connection`) all pass against
the fix -- and, confirmed directly by temporarily reverting the fix and
re-running, all three fail against the original code, proving they
really do catch this exact bug rather than passing vacuously. Full
`ctest` suite passes with the fix in place. Not yet confirmed against
real Azahar/Steam Deck hardware -- pending the user's next test.

## Performance tuning: bandwidth waste fixed, Azahar's capture rate doubled and made configurable

**Context**: once video was confirmed genuinely working end to end (see
the AdapterBridge fix above), the user asked to work on latency and
framerate. Two independent, real changes came out of reading the actual
pipeline rather than guessing:

**1. `NetServer::videoLoop()` was sending duplicate frames.** It called
`frameSource_->getLatestFrame()` on every tick (up to `videoSendFps`,
default 60Hz) and sent whatever came back unconditionally -- it never
compared the returned `frameIndex` against what it last sent.
`getLatestFrame()`'s contract is "return the latest one, whatever it
is" (latest-frame-wins), not "return a new one if there is one," so
whenever the loop's own tick rate outpaced the actual source frame rate
(exactly AzaharAdapter's situation at its old ~30fps capture rate vs.
this loop's 60Hz), roughly half of every video packet sent was a
byte-for-byte duplicate of the one before it -- pure wasted bandwidth,
worth nothing to the client (it just redraws the same texture either
way), and directly reducing the throughput budget actually available
for genuinely new frames. Fixed: skip the send when `frameIndex` is
unchanged from the last one actually sent. Not covered by a new
automated test -- verifying it properly would need a raw-socket test
harness duplicating `net_client.cpp`'s own handshake sequence outside
the `NetClient` class (opening a second video connection through
`NetClient` itself is rejected, since `NetServer` only tracks one
video client at a time), which wasn't judged worth the investment for
a change this simple to verify by reading (a guarded early-exit that
can't suppress the very first frame, since the guard requires a prior
sent index to exist) -- relying on code-review confidence and the
existing end-to-end `NetClient` tests, which already prove new frames
still arrive correctly with this change in place.

**2. AzaharAdapter's capture rate was hardcoded to ~30fps**, a
conservative, explicitly-labeled-as-untested guess made before this
project had ever run on real display/GPU hardware. The AzaharAdapter
capture-loop diagnostics added earlier this session gave real data:
zero `RequestScreenshot()` timeouts, 100% success rate, comfortably
within the 500ms-per-attempt budget at 30fps -- clear headroom. Default
bumped to 60fps, and made overridable via `AZAHAR_REMOTE_CAPTURE_FPS`
(clamped to [1, 60]) so it can be tuned per-machine without a new host
build for every value tried -- a real consideration after how many
rebuild-and-retest cycles the video bug itself took. The capture-stats
diagnostic log line now also reports the currently configured
ms-per-frame target for visibility while tuning.

**What wasn't changed, and why**: `RequestScreenshot()`'s Vulkan path
(`RenderScreenshotWithStagingCopy()`, in `renderer_vulkan.cpp`, not
part of this project's own patch) calls `scheduler.Finish()` -- a full
GPU pipeline sync -- as part of every single capture. At 30fps this
clearly wasn't a problem on the user's hardware (0 timeouts), but
whether it stays cheap at 60fps, and whether it measurably affects
Azahar's own native framerate (not just the streamed copy), is exactly
the kind of thing that needs a real playtest to answer, not more
reasoning from this sandbox (still no display/GPU stack here). That's
the whole reason for making the rate an env var instead of just
hardcoding 60: if 60 turns out to visibly stall the game itself, dial
it back (`AZAHAR_REMOTE_CAPTURE_FPS=45` or `=30`) without waiting on
another release.

**Not touched in this round** (real, larger levers, left for a
follow-up if the above isn't enough): frames are still sent as raw,
uncompressed BGRA8888 over TCP (Azahar's 320x240 frame is 307,200 bytes
-- at a genuine 60fps that's ~18.4MB/s just for video, which could
itself become the bottleneck on a real WiFi link); `AdapterIpcClient`'s
own 16ms poll interval between capture and the Unix-socket send to the
Host Service; and the DS/melonDS side's own frame-source pacing were
all left alone, since the reported problem was Azahar/3DS-specific and
none of these showed up as an actual bottleneck in the data gathered
so far.

**Verified**: full `ctest` suite passes with the `NetServer` dedup
change (existing video-delivery tests, including the two newer 3DS-sized
frame tests from the AdapterBridge fix, still pass -- confirming genuinely
new frames aren't accidentally suppressed). The Azahar-side capture-rate
change compiles cleanly (`citra_meta` target, full rebuild) and the
regenerated patch applies cleanly to a fresh checkout of the pinned
commit. Neither change has been confirmed to actually improve perceived
latency/smoothness on real hardware yet -- that needs the user's own
playtest, which is the whole reason `AZAHAR_REMOTE_CAPTURE_FPS` exists
as a same-build, no-rebuild-needed knob.

## Latency: tightened the two cheap-to-poll relay stages

**Context**: after the bandwidth/framerate fixes above, the user
confirmed "FPS is better. but latency is still noticable." Re-reading
the pipeline end to end (`AzaharAdapter::captureLoop()` ->
`AdapterIpcClient::writeLoop()` -> `AdapterIpcServer` ->
`AdapterBridge::getLatestFrame()` -> `NetServer::videoLoop()` ->
`NetClient`) shows three separate polling stages stacked in series,
each adding up to its own poll interval of worst-case delay before a
newly captured frame reaches the client:

1. `AzaharAdapter::captureLoop()`'s interval -- has a real GPU cost per
   poll (each one is an actual `RequestScreenshot()` call), already
   tuned in the round above and deliberately left alone here.
2. `AdapterIpcClient::writeLoop()`'s poll interval -- cheap: each
   iteration that finds nothing new is just a mutex-guarded
   struct/vector copy and comparison, no IPC message sent.
3. `NetServer::videoLoop()`'s tick rate -- also cheap now, and *only*
   cheap because of the dedup fix above: a tick that finds an unchanged
   `frameIndex` is now a free no-op (no packet sent), where before this
   session's earlier fix it would have sent a duplicate frame on every
   single tick.

Since (2) and (3) are now both free when they find nothing new, there's
no bandwidth cost to polling them far more often than frames actually
arrive -- only a (correspondingly small) reduction in the worst-case
relay delay. Changed:

- `AdapterIpcClient::writeLoop()`'s `kPollInterval`: 16ms -> 4ms, in
  both the native source-of-truth copy
  (`adapter-sdk/ipc/src/adapter_ipc_client.cpp`) and the Azahar-vendored
  copy inside `host/azahar-patches/0001-remote-server-integration.patch`.
  melonDS's own separately-vendored copy of this same file was
  deliberately **not** touched in this pass -- host-control mode is a
  separate, already-experimental path, and the user's reported issue is
  specifically the Azahar/3DS video pipeline.
- `NetServerConfig::videoSendFps`: 60 -> 240, in
  `host/remote-server/include/host/net_server.h`. Its doc comment was
  rewritten to describe it as a polling-responsiveness knob rather than
  a send-rate cap, since that's now what it actually controls.

**Not touched in this round, and likely the larger remaining lever**:
stage 1 above (Azahar's own capture interval, GPU-cost-bound, already
tuned last round) and the physical network link itself -- video is
still raw uncompressed BGRA8888 over TCP, and round-trip time between
host and client (WiFi vs. wired, physical proximity) is entirely outside
this project's control and wasn't something this sandbox could measure.
If tightening the two relay-stage polls here doesn't fully resolve the
report, compressing the video stream and/or asking about network
topology are the next things to try.

**Verified**: full `ctest` suite passes with both native-side changes.
The regenerated Azahar patch applies cleanly to a fresh checkout of the
pinned commit and compiles (`citra_meta` target, full rebuild) with the
diff against the previously committed patch confirmed to touch only the
intended `kPollInterval` lines. Not yet confirmed to actually reduce
perceived latency on real hardware -- that needs the user's own
playtest.

## Rebrand: melonDS Remote -> DualDeck

**Context**: the GitHub repo has been `crimson3076/DualDeck` for a
while, but the product itself hadn't caught up -- window title, every
dialog, Steam shortcuts, binaries, scripts, and `~/.config/melonds-remote*`
config directories all still said "melonDS Remote," a holdover from
before this project grew past melonDS/DS support into a multi-emulator
(DS + 3DS/Azahar) tool. The user hit a concrete symptom (the in-app exit
menu said "EXIT MELONDS ENTIRELY" even when connected to Azahar/3DS) and
asked for full consistency, confirming this should include internal
paths/binaries, not just on-screen text.

**Bug fixed along the way**: `client/src/main.cpp`'s `exitEmulationItems`
was a `const` vector with "EXIT MELONDS ENTIRELY" hardcoded, unlike the
adjacent `settingsMenuItems` lambda pattern which is re-evaluated at
every use site. Converted to the same lambda shape, building the label
from `sessionAdapterName` (already tracked, kept fresh from HelloAck) --
e.g. "EXIT AZAHAR ENTIRELY" when actually connected to Azahar.

**What changed**: window/dialog titles, on-screen text, console
banners, Steam shortcut AppNames (`melonds-remote-client`/`server` ->
`dualdeck-client`/`dualdeck-host-service`, launcher scripts ->
`dualdeck-client.sh`/`dualdeck-host.sh`), CMake project/target names
(`melonds_remote` -> `dualdeck`, `MELONDS_REMOTE_BUILD_*` ->
`DUALDECK_BUILD_*`), the `~/.config/melonds-remote*` directories (host,
client, Decky plugin), the Decky plugin's own name/panel title, and the
melonDS patch's in-window approval dialog + Settings checkbox text.

**Deliberately left alone, with reasons** (matches this session's
established "scoped fix" pattern):
- The C++ namespace `melonds_remote` -- used throughout every source
  file *and* vendored into both emulator patches; renaming it is a huge
  mechanical diff with zero user-visible benefit and would force a full
  symbol-level regeneration of both patches for no reason.
- Env vars baked into the emulator patches (`MELONDS_REMOTE_ENABLE`,
  `MELONDS_REMOTE_VERSION`, `MELONDS_REMOTE_AUTH_TOKEN`,
  `MELONDS_REMOTE_ADAPTER_SOCKET`, `MELONDS_REMOTE_STATE_DIR`,
  `MELONDS_REMOTE_BIND`, `MELONDS_REMOTE_HOST_NAME`,
  `MELONDS_REMOTE_OUT_OF_PROCESS`, `MELONDS_REMOTE_NO_DISCOVERY`,
  `AZAHAR_REMOTE_*`) -- confirmed via `getenv`/`envOr` grep in both
  patches that these are genuinely read by the patched emulator itself,
  not just our own scripts, so renaming them would require another
  patch-regeneration pass for zero user-visible benefit. Our *own*
  binaries' env vars that aren't read by any patch (the client's
  `MELONDS_REMOTE_VERSION` -> `DUALDECK_VERSION`, and
  `MELONDS_REMOTE_HOST_CONTROL` -> `DUALDECK_HOST_CONTROL`, a pure
  shell-script-level flag never read by melonDS) were renamed.
- The release archive's internal package directory naming
  (`melonds-remote-<commit>-linux-x86_64`) and the published tarball
  filename (`melonds-remote-linux-x86_64.tar.gz`) -- kept exactly as-is,
  permanently. This is load-bearing: every already-installed client/host's
  `apply-update.sh` has that exact download URL and extraction glob
  hardcoded (confirmed by reading the actual heredoc,
  `scripts/build-release.sh`'s `client/internal/apply-update.sh`) and
  its only real job is download -> extract -> hand off to the extracted
  archive's own (now dualdeck-branded) `install-steam-shortcut.sh
  --force` -- a generic, not path-specific handoff. So an old install's
  updater fetches a fully-renamed release just fine as long as this one
  external wrapper stays stable. `dualdeck-linux-x86_64.tar.gz` is
  additionally published (same bytes, `release.yml`) as a nicer-looking
  alias for the Releases page and for `DualDeck-Installer.sh` going
  forward -- new/fresh installs use it, existing installs never see it.

**Migration for existing installs**: since the Steam AppName and Exe
path both change simultaneously (e.g. `"melonDS Remote"`/old central
dir -> `"DualDeck"`/new central dir), `steam_shortcut.py`'s existing
Exe-OR-AppName fallback matching can't reliably bridge that compound
change on its own (confirmed by reading `find_matching_entry()` --
matching on the *new* AppName wouldn't find an entry created under the
*old* one). Both `install-steam-shortcut.sh` heredocs (client and host)
now explicitly attempt removal under the old identity first,
best-effort, before upserting under the new one -- this is new code
tested against a hand-crafted fixture (fake `$HOME`, fake Steam
userdata dir, a real old-identity entry created via `steam_shortcut.py`
itself), confirming: exactly one entry afterward (old gone, not
duplicated), and the old central install dir is left untouched.
Config-dir migration (`device_id.txt`, `last_host.txt`,
`settings.conf`, wizard state) is handled independently, directly in
each `defaultXPath()` function in `client/src/device_identity.cpp`/
`discovery_store.cpp`/`client_settings.cpp`/`wizard_state.cpp` -- a
copy-forward-once (never delete/move) from the old
`~/.config/melonds-remote-client/` path if the new
`~/.config/dualdeck-client/` one doesn't exist yet, so it works
regardless of how a user updates (Steam-shortcut reinstall or just a
binary swap). `device_id.txt` in particular must survive byte-for-byte
-- losing it would make the host treat an already-approved device as
brand new. Covered by a new `config_migration_tests` ctest target
(`client/tests/test_config_migration.cpp`). The Decky plugin's own
settings path (`~/.config/melonds-remote-decky/` ->
`~/.config/dualdeck-decky/`) gets the same copy-forward-once treatment
in `main.py`, and a leftover Distrobox container from before the rename
(`melonds-remote-host`) is detected and removed by both
`install-host-distrobox.sh` (before creating the new one) and
`uninstall-steam-shortcut.sh`.

**Verified**: full `ctest` suite passes (including the new migration
test target). Both emulator patches regenerated via the established
scratch-clone workflow -- applied cleanly to a pristine checkout at the
pinned commit, full rebuild (`citra_meta`/melonDS's own target), diffed
against the previously-committed patch to confirm only the intended
branding-string lines changed. The Steam-shortcut migration was verified
end-to-end against a hand-crafted fixture as described above, for both
install and uninstall. **Not verified**: an actual self-update from a
real previously-published (melonDS-Remote-branded) release into this
new build -- the fixture proves the migration logic is correct in
isolation, but hasn't been exercised via a real `apply-update.sh` run
against a real prior release archive.

## Three real-usage bug fixes: setup wizard ordering, Azahar input, 3D darkness (2026-07-22)

Three bugs reported together from real Steam Deck + Fedora-host usage,
after the DualDeck rebrand shipped.

**Setup wizard required a live host connection before controls could be
tested.** `runSetupWizard()`'s step machine (`client/src/main.cpp`) ran
`ChooseMethod -> ManualEntry/FindHost -> Connect -> VideoTest` *before*
`ControllerTest`/`TouchTest`, even though both of those steps are purely
local (confirmed by their signatures: neither takes a `NetClient` at
all). A user who just wanted to confirm their gamepad/touch mapping
worked had to first get a host reachable, approved, and streaming --
and `VideoTest`'s "NO VIDEO YET" state, correct on its own terms, blocked
ever reaching the controller test. Fixed by reordering the `Step` enum
and switch statement to `Welcome -> ControllerTest -> TouchTest ->
ChooseMethod -> ManualEntry/FindHost -> Connect -> VideoTest -> Done`,
so only the two steps that actually need a connection are gated behind
one.

**Azahar (3DS) joystick/circle-pad input did nothing.** Root-caused via
direct code trace, not guesswork: `AzaharAdapter`'s constructor calls
`registerInputEngine()`, which correctly overwrites
`Settings::values.current_input_profile` to route through the
`"melonds_remote"` input engine -- but `HID::Module` (and
`APT::AppletManager`, `IR_USER`, `IR_RST`) had already read the *old*
profile and built their `Input::ButtonDevice`/`AnalogDevice` objects
from it, back when `LoadROM()` powered the system on, before the adapter
was constructed at all (confirmed by reading `hid.cpp`'s
`LoadInputDevices()`, called once from the `Module` constructor). The
override landed in the settings struct but nothing ever re-read it.
Fixed with one line in `citra_qt.cpp`'s `BootGame()`: an extra
`system.ApplySettings()` call right after `remote_server_bridge->start()`
-- the same existing, idempotent "settings changed, rebuild input
devices" entry point already used when a user edits input config live,
which internally calls `ReloadInputDevices()`/`ReloadCameraDevices()` on
all four services (see `core.cpp`).

**3D-rendered bottom-screen content (e.g. Pokemon Alpha Sapphire's
Kyogre intro) rendered too dark, while 2D content was fine.** Two
hypotheses were formed and disproven by reading the actual renderer
code: a `render_3d_mode` pipeline mismatch (disproven --
`FramebufferLayout::render_3d_mode` always defaults to the live
setting), and a cross-thread GPU-state race between the capture thread
and the main render thread (disproven -- `RequestScreenshot()` only
sets flags from the background thread, and `SwapBuffers()` calls
`RenderScreenshot()` synchronously immediately before its own
`RenderToWindow()`, both on the same per-frame thread). The real cause
was on the client: SDL3 defaults a texture's blend mode to
`SDL_BLENDMODE_BLEND` whenever its pixel format carries an alpha channel
(`SDL_PIXELFORMAT_BGRA32` does; confirmed by reading SDL3's own
`SDL_CreateTexture()` source), and `client/src/main.cpp` never called
`SDL_SetTextureBlendMode()` at all. Azahar's Vulkan renderer captures
the raw presentation image byte-for-byte, alpha channel included,
whereas melonDS's own screenshot path forces an alpha-less `GL_RGB8`
renderbuffer (OpenGL spec guarantees reads back as opaque). Azahar's own
screen-blit pipeline (`ApplySecondLayerOpacity` in
`renderer_vulkan.cpp`, used by every `DrawScreens()` call) has a real
constant-alpha blend path, so a captured 3D frame can carry non-`0xFF`
alpha in places -- invisible on the host's own window (a normal window
surface is generally composited as opaque regardless of its alpha byte)
but very visible once streamed and blended by the client against its
window's black background. Fixed by calling
`SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE)` right after each
of the client's three `SDL_CreateTexture()` call sites (startup, and
both branches of the host-resize path) -- this texture only ever holds
an already-composited screen capture, never something meant to be
translucent, so forcing the alpha byte inert is correct for every
source (DS and 3DS alike), not just the specific case that surfaced it.

**Verified**: full `ctest` suite passes; the client rebuilds clean with
the reordered wizard and the blend-mode fix. The Azahar patch was
regenerated via the established scratch-clone workflow -- diffed against
the previously-committed patch to confirm only the intended
`system.ApplySettings()` addition (plus its comment) changed, applied
cleanly to a pristine checkout at the pinned commit, and `citra_qt`
rebuilt clean with it. **Not yet verified**: none of the three fixes
have been confirmed against real hardware yet -- specifically, whether
the 3D-darkness fix actually resolves the reported Kyogre-intro symptom
(the root cause traced through SDL3's and Azahar's own source is solid,
but the fix hasn't been visually confirmed against the original repro),
and whether the Azahar joystick fix restores movement in an actual
game session on real controller hardware.

## Azahar catches up to melonDS: top-screen-only-while-streaming, exit menu, real analog stick (2026-07-22)

Three more real-usage gaps, all specific to Azahar (3DS) sessions --
each one either melonDS already handled correctly, or the client
already had the wire support for but never actually used.

**Azahar's local window didn't show top-screen-only while a client was
streaming.** melonDS has done this since early on (`EmuInstance::
startRemoteServer()`'s connected-callback forces `ScreenSizing::TopOnly`
locally, restoring whatever was configured before on disconnect) --
Azahar never got the equivalent. The reason it was missing, not just an
oversight: melonDS runs its own `NetServer` in-process, so it directly
knows when a client connects. Azahar's adapter (`AzaharAdapter`) always
runs out-of-process, connected to a *separate* Host Service over the
adapter IPC channel -- the process actually running Azahar's Qt window
has no way to know a client connected at all, since that state lives
entirely in the other process's `NetServer`. Fixed by adding a new
message to the adapter IPC protocol itself
(`IpcMessageType::ClientConnectionChanged`, service -> adapter,
1-byte bool payload), wired end to end: `NetServerConfig::
onClientConnectionChanged` (already existed, was simply never
connected to anything) -> `main.cpp` now forwards it to
`AdapterIpcServer::notifyClientConnectionChanged()` (new) ->
`AdapterIpcClient::setConnectionStateCallback()` (new) ->
`RemoteServerBridge` marshals it onto Qt's UI thread via
`QMetaObject::invokeMethod` -> a new `GMainWindow::
OnRemoteClientConnectionChanged(bool)` slot that mirrors melonDS's
save-current-layout/force-`SingleScreen`/restore-on-disconnect sequence
exactly (also saving/forcing `swap_screen`, since `SingleScreen` alone
still shows whichever screen that flag says is primary).

**"Exit ROM"/"Exit Azahar Entirely" from the client's menu did
nothing.** The wire protocol and `GenericEmulatorAction` bitmask
(`GenericAction_QuitSession`/`GenericAction_QuitApplication`) were
already there and already reaching `AzaharAdapter::applyGenericInput()`
correctly (confirmed by reading the whole chain, not assumed) --
`applyGenericInput()` simply never read those two bits at all, only
buttons/sticks/touch. Fixed by adding the same rising-edge-detected
dispatch melonDS's `EmuInstanceInput.cpp` already does for this exact
purpose: `AzaharAdapter` takes two new optional constructor callbacks
(`onQuitSession`/`onQuitApplication`), invoked on a bit's rising edge
(not "is it set this frame", since the client resends its confirmed
choice for a short window to survive UDP loss) via
`QMetaObject::invokeMethod` into `OnStopGame`/`close` respectively --
wired up in `RemoteServerBridge`'s constructor, which now also takes a
`GMainWindow*` for exactly this and the previous fix's marshaling.

**The client's analog stick was never actually sent as analog data --
only as a digital D-pad substitute.** `protocol.h`'s `ControllerState`
has had `leftStickX/Y`/`rightStickX/Y` fields all along, and
`host/adapter_bridge.cpp` already forwards them into `GenericInputState`
for whichever adapter is connected -- but `client/src/main.cpp` never
populated them. Instead, `buildButtonsFromGamepad()`'s "left stick as an
alternate D-pad" (spec 7.3, a real DS-era convenience -- DS has no
analog stick at all) ran unconditionally, so tilting the stick during a
3DS/Azahar session pressed the physical D-Pad instead of moving the
Circle Pad, since `AzaharAdapter::registerInputEngine()` binds those to
two independent 3DS inputs. Fixed by: (1) always sending real stick data
(`leftStickX/Y` from `SDL_GAMEPAD_AXIS_LEFTX/Y`, `rightStickX/Y` from
`RIGHTX/Y`, Y negated to match `hid.cpp`'s `GetStickDirectionState()`
convention of positive-Y-is-up, confirmed by reading it -- SDL's raw
axis convention is the opposite), and (2) gating the D-pad-emulation
fallback to DS sessions only (`sessionSystemId == "nds"`, an explicit
allow-list rather than excluding "3ds" specifically, so a future system
with its own real stick, e.g. Wii U, doesn't inherit it by accident).

**Verified**: full `ctest` suite passes, including two new adapter-sdk
IPC tests for `ClientConnectionChanged` (a real end-to-end
server<->client round trip over a real Unix socket, plus a
no-connected-adapter no-op case) and two new `ipc_protocol` serialize/
parse unit tests. The Azahar patch was regenerated via the established
scratch-clone workflow: diffed against the previously-committed patch
to confirm only these three features' lines changed, applied cleanly to
a pristine checkout at the pinned commit, and `citra_qt` rebuilt clean
with all three. **Not yet verified**: none of the three have been
confirmed against real hardware/a real client-host session yet --
specifically, whether the local screen actually switches to top-only on
connect (and correctly restores on disconnect), whether Exit ROM/Exit
Azahar Entirely actually take effect, and whether the Circle
Pad/C-Stick now move correctly instead of the D-Pad.

## Display scaling and streamed resolution: both now decoupled from fixed constants (2026-07-22)

Two more real-usage gaps reported together, both about resolution
assumptions baked in as fixed constants.

**The client's UI didn't scale to non-Steam-Deck displays (e.g. an ROG
Ally's 1920x1080).** Every UI/touch-hit-test coordinate in
`client/src/main.cpp` is computed against `kWindowWidth`/`kWindowHeight`
(1280x800, Steam Deck's exact panel resolution). `SDL_CreateWindow`'s
`SDL_WINDOW_FULLSCREEN` flag fullscreens at the real display's native
resolution regardless of the size passed to it (confirmed in SDL3's own
header: "fullscreen window at desktop resolution"), so on any other
device the app's own 1280x800-based rendering just occupied the
top-left 1280x800 pixels of a larger real backbuffer -- uncentered,
unscaled, most of the screen left black. Fixed with one call right
after creating the renderer: `SDL_SetRenderLogicalPresentation(renderer,
kWindowWidth, kWindowHeight, SDL_LOGICAL_PRESENTATION_LETTERBOX)`. This
makes SDL do the scale-and-letterbox itself on every subsequent
`SDL_Render*` call, so none of the existing 1280x800 layout math needed
to change -- it still draws into a virtual 1280x800 canvas, which SDL
now maps onto whatever the real window/display size actually is.
LETTERBOX (not STRETCH) preserves the UI's own aspect ratio rather than
distorting bitmap text. Touch coordinates needed no corresponding
change: `event.tfinger.x/y` are already normalized 0..1 fractions of the
real window, independent of its actual pixel size.

**Azahar's streamed bottom-screen resolution was hardcoded to native
(320x240) regardless of the host's own rendering resolution.**
`AzaharAdapter`'s capture layout (`kBottomScreenWidth`/
`kBottomScreenHeight`) never scaled with Azahar's own internal
`resolution_factor` setting, so turning that up (for sharper local 3D
rendering) had no effect at all on what got captured and streamed --
the client still received a native-resolution image. Fixed by adding a
new, independent `AZAHAR_REMOTE_CAPTURE_SCALE` env var (native 1x by
default -- unchanged behavior unless set; clamped to [1, 4], i.e. up to
1280x960), read once at construction and applied to the capture buffer
size, the `SingleFrameLayout()` call, the OpenGL-invert-Y row-mirror
math (this needed fixing too -- it was still computing row width from
the native constants, which would have silently corrupted the image at
any scale above 1x on the OpenGL backend), and the `capabilities()`
call's reported `VideoSurfaceDescriptor` width/height. Deliberately
*not* tied to `resolution_factor` automatically: that setting is about
local 3D rendering sharpness on the host's own display and has no
principled relationship to what's sensible to stream over a LAN, so
this is its own explicit choice rather than an automatic, possibly
bandwidth-surprising side effect of an unrelated setting. `touchRangeX/
Y` in the same descriptor are deliberately left unscaled -- touch
precision is defined by the 3DS's native touch-digitizer coordinate
space, not by how many pixels the video capture happens to produce.
The client needed no changes for this: it already resizes its texture
to whatever native width/height a `HelloAck` reports (existing
host-resize handling in `client/src/main.cpp`), so a higher-resolution
`capabilities()` report alone is enough to stream at higher-than-native
resolution.

Bandwidth at higher capture scales is a known, deliberately deferred
tradeoff (per the request that prompted this) -- not addressed here.

**Verified**: full `ctest` suite passes. The display-scaling fix was
visually confirmed via an Xvfb screenshot at 1920x1080 (previously
top-left-cropped, now correctly letterboxed and centered) and at the
native 1280x800 (unchanged, confirmed no regression). The Azahar patch
was regenerated via the established scratch-clone workflow -- diffed
against the previously-committed patch to confirm only these two
features' lines changed, applied cleanly to a pristine checkout at the
pinned commit, and `citra_qt` rebuilt clean with both. **Not yet
verified**: the streamed-resolution fix hasn't been confirmed on real
hardware at a non-default `AZAHAR_REMOTE_CAPTURE_SCALE` value, and the
display-scaling fix hasn't been confirmed on a real ROG Ally/other
non-Deck device (only simulated via Xvfb at a matching resolution).

## 2026-07-22: Cemu (Nintendo Wii U) integration -- patch written, not yet build-verified

`host/cemu-patches/0001-remote-server-integration.patch` adds a third
real `IEmulatorAdapter` (Cemu, Nintendo Wii U) alongside melonDS and
Azahar -- see `host/cemu-patches/README.md` for the full breakdown of
what the patch does. Two things distinguish this pass from the other
two emulators' integrations, both explicit, user-confirmed trade-offs:

1. **Auto-injected controller mapping**: unlike melonDS/Azahar (which
   register their own input engine directly), Cemu requires a real
   controller-to-VPAD-button mapping to exist. Rather than requiring the
   user to open Controller Settings and configure this by hand, the
   patch calls `add_controller()` + `set_default_mapping()` directly
   against VPAD player 1 the moment a session starts, reusing (as a
   second `case` label, not a new table) the exact mapping table
   `VPADController::set_default_mapping()` already uses for XInput pads.
2. **No local build verification.** Cemu's dependency graph resolves to
   roughly 108 vcpkg packages, most requiring downloads from hosts this
   project's development sandbox cannot reach (only apt mirrors and the
   git-protocol mirror are reliably reachable there -- see this file's
   earlier vcpkg-tool-bootstrap entries for the underlying network
   constraint). Continuing to work around that dependency-by-dependency,
   as was done to bootstrap `vcpkg-tool` itself, was judged not worth it
   for ~100 more packages. This patch was instead written entirely from
   careful reading of Cemu's actual source at the pinned commit (the
   exact video-capture hook, the exact input-provider registration
   timing, the exact add_controller/set_default_mapping ordering
   requirement), with verification deferred to this project's GitHub
   Actions CI pipeline, which runs on a normal, unrestricted-network
   runner. **No compiler has seen this code yet.**

Also new: `Renderer::CaptureSurfaceBGRA()`, a small virtual added to
Cemu's own `Renderer` base class (default no-op, real OpenGL and Vulkan
implementations) for continuous, throttled (`CEMU_REMOTE_CAPTURE_FPS`,
default 30) frame capture -- deliberately separate from Cemu's existing
`HandleScreenshotRequest()`, which is a one-shot, user-triggered
screenshot-to-file feature, not reused or altered by this patch.

Wii U GamePad touchscreen input is out of scope: confirmed by reading
`Cafe/OS/libs/vpad/vpad.cpp`'s `VPADRead()` that Cemu hard-codes
GamePad touch validity to invalid regardless of which controller is
mapped -- no existing Cemu input backend can inject GamePad touch
today, so there's no plumbing for this adapter to hook into without
inventing an entirely new Cemu-side touch pipeline.

See `host/cemu-patches/README.md`'s "What is *not* verified yet"
section for the full, explicit list of what remains unconfirmed
(compilation and any real end-to-end run against a Wii U game).

**Update**: the host launcher (`dualdeck-host.sh`'s "Which system?"
menu now offers "Nintendo Wii U (Cemu, experimental)"),
`scripts/build-release.sh` (clones/patches/builds Cemu at the pinned
commit, packages `host/cemu` + `host/internal/run-host-cemu.sh`
alongside melonDS/Azahar), `scripts/patch-existing-emulator.sh`
(`--system wiiu`, for anyone with their own Cemu checkout), and
`.github/workflows/release.yml` (a cached build step, same pattern as
Azahar's) are all wired up. Still not build-verified locally for the
same sandbox-network reason as before; the next `release.yml` run is
the actual first real build attempt.

## 2026-07-22: Cemu integration -- first successful build + end-to-end run, three findings

Following the entry above, CI eventually produced a successful Cemu
build (after three rounds of CMake include-path/precompiled-header
fixes documented in `host/cemu-patches/README.md`), and a real
end-to-end session followed: a Wii U game booted, a DualDeck client
connected, video streamed, and the auto-wired VPAD player-1 mapping
worked. That session surfaced three issues:

1. **GamePad touchscreen input doesn't register.** Confirmed expected,
   not a bug -- see the "Wii U GamePad touchscreen input is out of
   scope" note above, unchanged by this entry.
2. **Aspect ratio on the GamePad screen was wrong** (client-side bug,
   not specific to this patch). `computeAspectFitRect()`
   (`protocol/src/touch_mapping.cpp`) hardcoded a 4:3 content aspect
   ratio, correct for melonDS/Azahar's 4:3-shaped screens but not the
   Wii U GamePad's 854x480 (16:9) output -- the first non-4:3 surface
   any adapter has streamed. Fixed with an optional `contentAspect`
   parameter (default 4:3, so melonDS/Azahar/existing tests are
   unaffected) and passing the connected host's real aspect ratio
   through at the client's gameplay-loop call site.
3. **Colors came out slightly darker than Cemu's own window.**
   `Renderer::CaptureSurfaceBGRA()`'s original implementation
   deliberately skipped the sRGB<->linear correction
   `HandleScreenshotRequest()` already applies (`SRGBComponentToRGB`/
   `RGBComponentToSRGB`, gated on a `srcUsesSRGB`/`dstUsesSRGB`
   mismatch), reasoning it was cosmetic-only. That reasoning didn't
   hold for a continuously-displayed live stream: a mismatched
   sRGB/linear render target reads visibly darker/lighter once the raw
   bytes are displayed remotely, even though Cemu's own window looks
   right (its normal present path already handles the conversion).
   Fixed by applying the same correction in both `CaptureSurfaceBGRA()`
   implementations (OpenGL and Vulkan), which required adding a
   `padView` parameter to the virtual so it can tell which of
   `LatteGPUState.tvBufferUsesSRGB`/`drcBufferUsesSRGB` applies.

The aspect-ratio fix is client-only and needed no Cemu rebuild. The
color fix went through a CI build (v0.1.61), which succeeded, though
real confirmation it resolves the color difference against actual game
content is still pending -- see `host/cemu-patches/README.md`'s "First
real end-to-end run findings" section for the exact
unverified-vs-verified breakdown.

## 2026-07-22: Cemu integration -- GamePad video required the local GamePad View window

Testing the v0.1.61 build (aspect ratio + color fixes applied)
surfaced one more issue: GamePad video only streamed to a DualDeck
client while Cemu's own "Enable GamePad View" window was also open
locally -- not something a headless/remote-play use case should ever
require.

Root cause: the video-capture hook originally lived alongside Cemu's
own `HandleScreenshotRequest()` call, inside a function that only runs
for the GamePad surface when Cemu's local pad window actually exists
(`g_renderer->IsPadWindowActive()`). That's correct for a one-shot,
user-triggered screenshot of whatever's on screen, but wrong for a
capture path that's supposed to work regardless of what, if anything,
is open locally. Fixed by moving the hook to
`LatteRenderTarget_itHLECopyColorBufferToScanBuffer()`, which hands
over the real TV/DRC scan-buffer texture unconditionally, once per real
frame per surface, with no dependency on any window's existence --
`CaptureSurfaceBGRA()` reads straight from that GPU texture. This also
fixes a subtler, previously-undiscovered issue: the old hook fired from
the on-screen *presentation* path, which includes a local Tab/Ctrl+Tab
(or VPAD "screen active" button) toggle that can swap GamePad content
onto the "TV" backbuffer -- so the old capture could mislabel which
surface was which depending on what a local player was looking at. The
new hook reads the real TV/DRC buffers before any such local toggling,
so the streamed `"tv"`/`"gamepad"` surfaces now always correspond to
the Wii U's actual TV/GamePad outputs.

See `host/cemu-patches/README.md`'s "Second real end-to-end run
findings" section for the full writeup. This went through a CI build
(v0.1.62), which succeeded -- but real testing found it made GamePad
mirroring *worse*, not better: video stopped streaming entirely,
regardless of whether the local GamePad View window was open or
closed.

## 2026-07-22: Cemu integration -- v0.1.62 regression fixed (stale texture read)

Root cause: `LatteRenderTarget_copyToBackbuffer()` (where the capture
hook used to live) always calls `LatteTexture_UpdateDataToLatest()` and
`LatteTC_MarkTextureStillInUse()` before touching its texture --
every other consumer of a Latte texture view in this codebase relies on
that having already happened. The relocated hook, in
`LatteRenderTarget_itHLECopyColorBufferToScanBuffer()`, runs earlier in
the pipeline and did neither, so `CaptureSurfaceBGRA()` could be
reading a texture Cemu's on-demand texture cache hadn't resolved or
uploaded yet -- plausibly an effectively-empty or wrong-layout GPU
texture straight out of `LatteTC_GetTextureSliceViewOrTryCreate()`,
which would explain a hard "nothing streams" failure rather than a
softer staleness/lag symptom. Fixed by adding both calls immediately
before the capture hook, mirroring `copyToBackbuffer()`'s own preamble.

See `host/cemu-patches/README.md`'s "Third real end-to-end run
findings" section for the full writeup. This went through a CI build
(v0.1.63), which succeeded, and real testing confirmed the fix: GamePad
mirroring now works (window open or closed), and the earlier aspect
ratio and color fixes both hold up against real game content. Touch
still doesn't register (unchanged, expected). One new issue surfaced:
face buttons (A/B/X/Y) were all swapped by *label* rather than
*physical position* -- Steam Deck's south-face "A" landed on Wii U's A,
when Xbox-layout south physically corresponds to Nintendo-layout B
(and so on for B/X/Y).

## 2026-07-22: Cemu integration -- face button mapping fixed (Xbox/Nintendo layout swap)

Root cause: `RemoteController::raw_state()` fed each face button
directly into the *Wii U button it was meant to end up on*, but the
shared `VPADController::set_default_mapping()` table it feeds into
already performs the Xbox-layout -> Nintendo-layout physical-position
correction real XInput controllers need (confirmed by reading
`XInputController.cpp`, which populates its raw button slots directly
from `XINPUT_GAMEPAD_A/B/X/Y`'s own hardware bit positions, not from
the destination Wii U button). Feeding the correction's *output* back
in as if it were raw input applied the swap a second time and
cancelled it out. Fixed by mapping each face button to the raw XInput
bit position it physically corresponds to instead, letting the shared
table's correction apply exactly once. D-pad, shoulders, stick clicks,
and start/select were already correct and untouched.

See `host/cemu-patches/README.md`'s "Fourth real end-to-end run
findings" section for the full writeup. Not yet re-verified through a
CI build as of this writing.

## 2026-07-22: Video streaming latency under a bandwidth-constrained link (all adapters)

Reported against the Cemu integration (whose 854x480 GamePad surface is
meaningfully bigger than melonDS's 256x192 or Azahar's 320x240 --
roughly 5-8x more bytes per uncompressed frame), but the underlying
issue and fix are project-wide, not Cemu-specific: switching which
machine acted as host (including moving the host onto wired Ethernet)
only partly helped, and switching client hardware (Steam Deck LCD ->
ROG Ally X) also only partly helped -- neither eliminated it, which
points at the shared network link's actual throughput rather than
either endpoint's CPU/GPU.

Root cause: video frames are sent over a plain TCP socket
(`NetServer::videoLoop()`, `host/remote-server/src/net_server.cpp`),
completely uncompressed. `videoLoop()` always fetches the single
truest-latest frame each tick (no application-level queue), but TCP's
own in-order delivery guarantee means every frame that actually gets
handed to `send()` still has to be delivered in order -- so on a link
too slow to keep up with the raw frame rate, frames don't get dropped,
they queue up in the *kernel's* TCP send buffer (typically hundreds of
KB to a few MB by OS default, enough to hold several whole frames) and
get delivered increasingly late. Latency grows over time under
sustained congestion rather than settling at a fixed one-frame delay --
consistent with "noticeable lag" that persisted across otherwise-faster
hardware on both ends.

Fixed by sizing each video connection's `SO_SNDBUF` to roughly two
frames' worth of bytes (computed from the connected adapter's actual
`frameDimensions()`, so it's correctly sized per-adapter rather than a
fixed constant). This bounds how many stale frames the kernel can
buffer ahead of what's actually reached the network to about one, so
once a slow link's queue fills, `send()` blocks (bounded by the
existing 1-second `SO_SNDTIMEO`) until it actually drains, and the next
loop iteration reaches for whatever's truly latest rather than whatever
was queued -- keeping added latency bounded to roughly one frame's
transmission time instead of growing without limit. Does not eliminate
lag caused by a link that's *persistently* too slow for the raw frame
rate (see "Still out of scope" below) -- only the unbounded growth a
plain default-sized TCP buffer allowed on top of that.

Still out of scope for this fix: real frame compression. The video
pipeline has never compressed frames (raw BGRA end to end) -- viable
for DS/3DS-sized surfaces, increasingly relevant for anything Cemu-
sized or larger on a genuinely bandwidth-limited link (e.g. weaker
Wi-Fi). Adding compression is a protocol-level change (affecting every
adapter, the wire format, and both client and host) big enough to need
its own separate design pass, not folded into this fix.

## 2026-07-22: Cemu integration rebased onto the v2.6 stable release

The Cemu patch was developed and verified through four real
end-to-end rounds against a development-branch commit. Rebased onto
`v2.6` (the latest tagged stable release, 2025-02-06) on request, since
a dev build can carry in-progress work with its own performance
regressions or instability -- not a sound default to build a release
on. Cemu's own public development slowed sharply after the project's
late-2024 acquisition, so the real overlap between `v2.6` and the old
dev-branch base turned out smaller than the raw 278-commit/775-file gap
between them suggested: 30 of the 35 previously-patched files applied
against `v2.6` with line-offset-only changes, no real conflicts,
meaning the specific functions and APIs this patch hooks into are
unchanged between the two. Five files needed real rework (CMake
subdirectory/link-list differences, a `WindowSystem::NotifyGameLoaded()`
-> `gui_notifyGameLoaded()` rename, a missing `#endif` block, and
`Renderer.h` lacking a since-added screenshot-request preamble that
this patch's own addition sits next to). Two more needed content
changes despite applying cleanly as new files: `v2.6` has no
`cemu_use_precompiled_header()` helper yet (replaced with an explicit
`CemuCommon` link, which gets the same effect the same way every other
real module in `v2.6` already does it), and `v2.6`'s `gui/` tree
predates the `wxgui/` subdirectory split (flat `gui/MainWindow.h`
instead) with no bare-`#include` resolution issue to work around there
at all.

One change goes further than anything else in this patch has needed:
`v2.6`'s `MainWindow` has no public `EndEmulation()` method for the
QuitSession (GitHub issue #25) hook to call -- confirmed by reading the
real handler, which inlines the "stop title, return to game list"
sequence directly in a private menu-event handler with no reusable
entry point. Extracted that exact sequence into a new public
`MainWindow::EndEmulation()` method (with the original handler now
calling it too), the first and only change this Cemu integration has
made to Cemu's own `gui/MainWindow.{h,cpp}` -- every other change stays
confined to new files plus small, targeted hook points in existing
ones, same discipline as the melonDS and Azahar patches.

See `host/cemu-patches/README.md`'s "Rebase to the v2.6 stable
release" section for the full file-by-file breakdown.

**First `v2.6` CI build attempt**: got to 305/544 files before
failing -- `VulkanRenderer::CaptureSurfaceBGRA()` called
`baseImageTex->GetDefaultLayout()` (carried over unchanged from the
dev-branch patch) to restore a render target's Vulkan image layout
after reading it via blit; `LatteTextureVk` has no such method at
`v2.6` (a later addition). Root-caused by reading
`HandleScreenshotRequest()`'s own unmodified code earlier in the same
file, which does the exact same "done reading a render target via
blit, put it back" step by hardcoding `VK_IMAGE_LAYOUT_GENERAL` on
both sides of the transition, not deriving it from the texture at all.
Fixed by replacing both `GetDefaultLayout()` calls with that same
literal constant. Still unverified: whether this was the only issue at
this base -- the build hadn't gotten past this file when it stopped.

## 2026-07-22: Video frames are now JPEG-compressed (protocol v8) -- fixes "basically unusable" bandwidth on Cemu

Follow-up to the SO_SNDBUF fix above, which explicitly left "real frame
compression" out of scope. Reported after that fix shipped: "after
testing both clients, connectivity is very poor, and basically unusable
with most games. some 2D menus do not render and 'disconnect' the
client, not allowing controls until the menus have been interacted with
on the host directly."

Root cause (the bandwidth half of that report -- the 2D-menu-freeze half
is a separate, still-open issue, see below): every video frame was sent
completely uncompressed, project-wide, since the very first version of
this pipeline. Fine for DS's 256x192 or 3DS's 320x240 surfaces, but
Cemu's 854x480 GamePad surface is ~9x the 3DS bottom screen's pixel
count -- at 1.64MB/frame uncompressed, even 30fps needs ~394 Mbps and
60fps ~788 Mbps, both far beyond what real Steam Deck/ROG Ally Wi-Fi (or
often even real-world gigabit Ethernet) sustains. The SO_SNDBUF fix
stopped latency from growing *unboundedly* on a too-slow link; it never
could have manufactured bandwidth that isn't there.

Fixed by JPEG-compressing every frame (libjpeg-turbo's turbojpeg API)
before it goes out over the wire, and decompressing it back to raw
BGRA8888 on the client before handing it to the existing texture-upload
path -- a protocol version bump (v7 -> v8, see `protocol.h`'s
`kProtocolVersion` comment) since the wire payload format changed
incompatibly. The encode/decode step lives entirely inside
`NetServer::videoLoop()` (`host/remote-server/src/net_server.cpp`) and
`NetClient::videoReceiveLoop()` (`client/src/net_client.cpp`) -- no
adapter (melonDS/Azahar/Cemu/synthetic/host-control) or any of their
patches needed to change, since they all still just hand raw BGRA to/from
the same `IFrameSource`/`getLatestFrame()` contract as before. Quality is
configurable (`NetServerConfig::videoJpegQuality`, default 80) for
tuning against a given link.

One real, deliberate side effect: JPEG has no alpha channel, so the
alpha byte of every decoded BGRA pixel always comes back as `0xFF`
regardless of what was originally captured there. Harmless in practice
-- `client/src/main.cpp` always sets `SDL_BLENDMODE_NONE` on the video
texture specifically because this pipeline's alpha byte has never been
meaningful transparency data, so it's never read for blending -- but
worth knowing if a future change ever tries to use that channel for
something real.

Two existing end-to-end tests asserted exact byte-for-byte frame
equality between what a fake frame source produced and what the client
received; both were updated to a small per-channel tolerance (and to
skip the now-`0xFF` alpha byte in the client-side test) rather than
exact equality, since JPEG is lossy by design --
`client/tests/test_net_client.cpp`'s
`net_client_receives_a_non_ds_sized_video_frame` and
`host/remote-server/tests/test_net_server_mode_switch.cpp`'s
`set_target_routes_video_to_the_new_frame_source`.

**Still open**: the 2D-menu-freeze half of the original report.
Cemu composites Wii U system overlays (software keyboard, error/message
popups -- `swkbd_render()`/`nn::erreula::render()`) only inside
`LatteRenderTarget_copyToBackbuffer()`, and for the GamePad surface that
function only runs while `g_renderer->IsPadWindowActive()` -- i.e. only
while Cemu's real local "Enable GamePad View" window (with its own live
GPU swapchain) is open. This project's video-capture hook was
deliberately moved earlier in the pipeline (see the "GamePad video
required the local GamePad View window" entry above) specifically to
stop needing that window for normal gameplay, which means it now
captures the raw scan-buffer texture *before* these overlays are
composited in: any ordinary game frame is unaffected, but these specific
system popups are invisible to a remote client, and since Cafe OS blocks
waiting on them to resolve, no input does anything remotely until
someone dismisses the popup locally on the host. `IsPadWindowActive()`
can't simply be forced true without a window, either -- both backends'
implementations (`GLCanvas_HasPadViewOpen()`,
`IsSwapchainInfoValid(false)`) are tied to a real, live GPU swapchain
that `copyToBackbuffer()`'s ImGui-based rendering actually draws into,
not a spoofable flag. Not fixed here: doing so without reopening the
original "unwanted extra window" complaint likely means rendering
`swkbd_render()`/`erreula::render()` into an off-screen capture target
instead of a real window swapchain, a nontrivial Cemu-rendering change
outside this fix's scope.

## 2026-07-23: Azahar capture scale now follows resolution_factor automatically -- the earlier deliberate decoupling was the wrong default

Follow-up to the resolution-decoupling entry directly above. Reported
after real use: "the client resolution does not respect the internal
resolution of azahar. I have it set to 9x on host but client still
looks like it's being sent the native resolution." The behavior was
exactly as designed at the time (`AZAHAR_REMOTE_CAPTURE_SCALE` is
independent of `resolution_factor` on purpose, per the entry above) --
but real usage showed that design choice reads as a bug to anyone who
doesn't already know a second, separate env var exists: raising Azahar's
own "Internal Resolution" graphics setting is the obvious, discoverable
way to ask for a sharper stream, and having it silently do nothing is
worse than the bandwidth risk the decoupling was meant to avoid.

Fixed in `AzaharAdapter.cpp`'s `resolveCaptureScale()`: without
`AZAHAR_REMOTE_CAPTURE_SCALE` explicitly set, it now reads the actual
renderer's effective scale via `system.GPU().Renderer().
GetResolutionScaleFactor()` -- the same accessor `RendererBase` itself
uses (`video_core/renderer_base.cpp`), so `resolution_factor`'s own `0`
("Auto (Window Size)") resolves the same way it would for local
rendering, rather than being reinterpreted here. `AZAHAR_REMOTE_CAPTURE_SCALE`
still exists as an explicit override for anyone who wants a sharper
*local* picture than they want to actually stream. The clamp also moved
from `[1, 4]` to `[1, 10]`, matching `resolution_factor`'s own valid
range (`common/settings.h`) -- the original ceiling's bandwidth
rationale is considerably weaker now that every frame is JPEG-compressed
(see the protocol v8/v9 entries above) rather than sent raw, which is
the whole reason revisiting this ceiling was reasonable to do alongside
that work rather than needing its own separate bandwidth analysis.

Calling `system.GPU().Renderer()` from `AzaharAdapter`'s constructor
(inside a member-initializer, before the constructor body runs) is safe
because `RemoteServerBridge` -- and so this adapter -- is only ever
constructed after `BootGame()` has already loaded the ROM and created
the renderer (`citra_qt.cpp`); `RequestScreenshot()` elsewhere in the
same file already relies on that same precondition.

**Verified**: regenerated via the established scratch-clone workflow --
diffed against the previously-committed patch to confirm only this
function's lines changed, and `citra_meta` (Azahar's actual GUI
executable target, not just the `citra_qt` static library) rebuilt clean
incrementally from the existing build directory. **Not yet verified**:
real hardware confirmation that a client actually receives a
higher-than-native stream with no env var set, once `resolution_factor`
is raised in Azahar's own settings.

## 2026-07-23: Per-client video quality setting (protocol v9) -- JPEG compression was too aggressive for DS/3DS

Follow-up to the JPEG compression entry above. Reported after real use:
"now compression is too much in DS and 3DS, client should have settings
to control video compression amount. allowing full resolution down to
quite compressed." Root cause: `NetServerConfig::videoJpegQuality`'s
default of 80 was tuned for Cemu's much larger, often bandwidth-
constrained GamePad surface -- exactly the case compression was
introduced to fix -- but DS (256x192) and 3DS (320x240) frames are small
enough that bandwidth was never the constraint, so the same fixed
quality just threw away fidelity those sessions had no need to sacrifice.

Fixed by adding `HelloPayload::videoQuality` (protocol v8 -> v9): the
client now requests a quality (1-100) for its own session at handshake
time, or `0` to defer to the host's configured default; an out-of-range
value falls back the same way. `NetServer` stores the effective value in
`currentVideoQuality_`, set once per handshake (this project's existing
single-active-client assumption, same as `authenticatedClientAddr_`) and
read by `videoLoop()` for every frame of that session -- see
`net_server.cpp`. Client-side, `ClientSettings::videoQuality` persists
the choice (`~/.config/dualdeck-client/settings.conf`'s `video_quality=`
line) and a new "VIDEO QUALITY" entry in the Settings menu cycles through
five presets (AUTO, LOW, MEDIUM, HIGH, MAXIMUM = 0/40/65/85/100) --
matching this menu's existing cycle-through-fixed-choices style
(MICROPHONE:, AUTO UPDATE ON LAUNCH:) rather than a continuous slider
this text UI has no widget for. Takes effect on the next connection, not
immediately -- there is no packet type for changing an already-connected
session's compression quality, and reconnecting to pick up a Settings
change is consistent with how e.g. CHANGE HOST already works.

Also fixed alongside the quality knob, since "full resolution" fidelity
was specifically requested: at quality >= 90, `compressFrameBgraToJpeg()`
switches JPEG chroma subsampling from 4:2:0 to 4:4:4. Subsampling is a
structural choice independent of the quality scalar -- even quality 100
with 4:2:0 subsampling visibly softens sharp pixel-art edges/text, which
a "maximum quality" request should not still be capped by. The SO_SNDBUF
sizing estimate (see the JPEG-compression entry above) was widened
correspondingly at quality >= 90, since 4:4:4's larger compressed output
would otherwise be under-estimated by the heuristic tuned for the
default quality's 4:2:0 case.

## 2026-07-23: Cemu GamePad screen flashes between "connecting" and host-control, controls don't work

Reported after the v2.6-based Cemu build was first tested for real:
"Wii U second screen still does not work, it flashes between the
connecting to host and host control screen, without actually showing
the game menu or controls working." This describes the *client's own UI*
oscillating between its "CONNECTING..." state and its HostControl-mode
screen (the one shown when no emulator adapter is connected and the
client is just driving the host's own input directly) -- which, per
`ModeCoordinator`'s design (see the Host Service architecture ADR), only
happens if the host repeatedly gains and loses its adapter connection to
Cemu.

**First diagnosis, overstated -- corrected by the user.** A captured
host-service log (`dualdeck-host.sh 2>&1 | tee ~/dualdeck-host.log`)
showed the expected attempt-connect / connect / switch-to-Emulation /
switch-back-to-HostControl cycle repeating dozens of times, ending in a
real segfault inside `__libc_free`, stack-traced through
`CemuAdapter::onSurfaceRendered()` ->
`LatteCP_itHLECopyColorBufferToScanBuffer()` ->
`LatteCP_ProcessRingbuffer()` (Cemu's own graphics command processor
thread). This was initially reported to the user as "Cemu itself was
crashing" -- explaining the whole cycle. The user correctly pushed back:
"Cemu itself was staying alive, so I doubt it was 'crashing' unless it
was the network plugin or something." Re-reading the same log confirmed
they were right: the segfault trace appears exactly once, at the very
end, after 20+ reconnect cycles that show no crash evidence at all, with
client input flowing continuously and unaffected throughout. The crash
is real (see the Vulkan fix below) but it does not explain the repeated
flashing that was actually reported -- it only explains how the session
eventually ended.

**Second theory, ruled out.** Suspected `RemoteServerBridge` being torn
down and rebuilt on every Cafe OS `LaunchForegroundTitle()`/
`ShutdownTitle()` event (e.g. title-to-title or applet transitions).
Ruled out once the user clarified: "I was loaded into the game's main
menu when it was doing it" -- a single, stable, already-loaded title's
own in-game menu, not a title relaunch.

**Root cause (current best diagnosis): the adapter IPC frame-size cap
was too small for Cemu's real capture resolution.**
`adapter-sdk/ipc/include/.../ipc_protocol.h` defined
`kMaxIpcFramePixelBytes` as 16 MiB, sized off an assumption that Wii U's
TV surface tops out at 1920x1080 (8,294,400 bytes). `CemuAdapter`
actually captures the TV surface at Cemu's *actual* internal render
resolution (`GetEffectiveSize()`), which is not always capped at 1080p.
`parseSurfaceFrame()` rejects any declared frame over the cap as
malformed, and both `AdapterIpcClient::readLoop()` and the server's own
receive loop treat a failed parse as "this connection is over." That
matches every observed symptom: Cemu's process itself is completely
unaffected (matching the user's correction, since this is purely a local
Unix-socket IPC parse-rejection, not a crash); the connection completes
its small Hello/HelloAck handshake, `ModeCoordinator` switches to
Emulation, then the very first oversized TV-surface `Frame` message gets
rejected and the connection drops back to HostControl; and it is
unrelated to which screen is displayed, matching the user's clarification
that it happened while sitting stably in the game's own main menu. The
observed ~1-second reconnect cadence is itself just an artifact of
`RemoteServerBridge::start()`'s reconnect loop resetting its backoff to
1000ms on every successful `connect()` -- not evidence the connection
survived a full second before dying.

**What actually drives the oversized capture, corrected after the first
guess was tested and disproved.** Initially assumed this required a
user-installed resolution-enhancing graphics pack. The user tested this
directly and reported "no graphics packs installed on either" -- while
also reporting the bug is specific to Twilight Princess HD and doesn't
happen with Wind Waker HD on the same setup. That rules out graphics
packs as the mechanism here, but not the underlying cause: some Wii U
titles render their own internal framebuffer above their display output
resolution as part of the *game's own* built-in anti-aliasing/
supersampling, independent of anything Cemu's graphics-pack system adds.
Twilight Princess HD doing this while Wind Waker HD doesn't would explain
the exact split observed. The fix itself is unaffected either way, since
it's keyed off Cemu's real per-frame render-target size
(`GetEffectiveSize()`), not off detecting graphics packs specifically --
raising the cap to 128 MiB covers both causes.

Fixed by raising `kMaxIpcFramePixelBytes` from 16 MiB to 128 MiB (comment
in `ipc_protocol.h` has the full sizing rationale, including that this
is purely local same-machine IPC, so a generous cap costs nothing on the
network). The same 16 MiB cap was independently too small for Azahar
too, now that its capture scale auto-follows `resolution_factor` (see
the entry above) -- at 10x, even the 3DS's smaller screen exceeds it.
Both Cemu's and Azahar's patches vendor their own copy of this header and
were updated identically. **Not yet confirmed against the user's
hardware or against a graphics-pack-enabled game** -- this is the
current best theory, consistent with every log detail gathered so far,
but hasn't been directly validated the way the earlier two theories were
ruled out.

Separately, `VulkanRenderer::CaptureSurfaceBGRA()` allocated and freed a
fresh Vulkan staging buffer on every single call -- the exact same
pattern Cemu's own pre-existing `HandleScreenshotRequest()` uses, but
that function only ever runs once per rare, explicit user action, while
this one runs continuously at up to 60 times a second (both screens
combined) for as long as a client is connected. Sustained allocate/free
churn at that rate is a known way to destabilize a Vulkan driver's
internal allocator, consistent with the one real crash seen after
~15-20 seconds of streaming. This is a real bug, fixed by making the
staging buffer persistent (`VulkanRenderer`'s new
`m_dualDeckCaptureStagingBuffer` members, freed only in its own
destructor) instead of allocating and destroying it every call, and by
fixing `CreateBuffer()`'s previously-unchecked return value on this
path -- but it is a secondary bug, not the explanation for the
repeatedly-reported flashing (see above).

**Not yet verified**: this project cannot build or run Cemu in its own
sandbox (confirmed repeatedly throughout this integration), so both
fixes have only been reasoned through and diffed against the
previously-committed patch, not compiled or tested against a real
device. Needs a CI build followed by a real retest to confirm both that
it compiles and that the flashing and the crash are both actually gone.

## 2026-07-23: Cemu GamePad video is a sheared/torn mess on most titles -- root cause was a stale declared resolution, not a crash or a dropped connection

Reported once the IPC frame-size cap fix above let video actually flow
continuously: three screenshots (Twilight Princess HD, Pokemon Rumble U,
Wind Waker HD, all on the same Steam Deck/Cemu setup) showed Wind Waker
streaming a perfectly clean image while the other two showed a
consistent horizontal comb/interlace-style tearing across the entire
frame, stable for the whole session (not intermittent) -- video was
flowing (FPS counter, CPU/GPU stats all normal), so this is a different
bug from the earlier flashing/dropped-connection reports.

Root cause: `CemuAdapter::capabilities()` declared the GamePad (second
screen, the one actually streamed to the client) surface's resolution as
a hardcoded default -- `kGamePadDefaultWidth`/`kGamePadDefaultHeight`
(854x480, the Wii U GamePad *hardware's* native resolution) -- and
`NetServer` queries that declared value exactly once per connection
(`AdapterBridge::frameDimensions()`), then uses it for the rest of the
session to interpret every subsequent raw captured frame's byte layout
when JPEG-encoding it (width/pitch passed to `tjCompress2`). But a
title's actual internal GamePad-view render resolution -- what
`CaptureSurfaceBGRA()` actually captures, via `GetEffectiveSize()` -- is
not required to be exactly 854x480, and most titles' isn't. Whenever the
real captured buffer's actual width×height differs from the declared
854x480 the encoder was told to assume, every row gets read at
increasingly the wrong offset -- exactly the sheared/torn look in the
screenshots. `CemuAdapter` already computed and stored the real captured
size on every frame (`CapturedSurface::width/height`, updated in
`onSurfaceRendered()`) -- it just never fed that back into
`capabilities()`, contrary to `CemuAdapter.h`'s own top comment, which
already documented the *intended* fallback-to-real-size behavior that
the code never actually implemented.

**First fix, tested and found insufficient**: had `capabilities()`
report the real last-captured width/height for the TV and GamePad
surfaces once at least one frame of each had been captured, falling back
to the compile-time default only before the very first frame -- reasoned
(wrongly) to always resolve in practice by the time a client's Hello
handshake arrives, since Cemu renders continuously from the moment a
title boots. The user retested against Twilight Princess HD once this
shipped and it was **still bugged**, unchanged. Re-examining the actual
connection lifecycle explains why: `RemoteServerBridge` (and the
IPC-connected `CemuAdapter` alongside it) isn't a single long-lived
connection for Cemu's whole process lifetime -- it's rebuilt fresh
inside `LaunchForegroundTitle()` on every single title boot (see
`CafeSystem.cpp`'s hooks). The very first `AdapterIpcClient::connect()`
attempt after that typically succeeds within milliseconds (the Host
Service is already listening), while the title itself takes much
longer -- often seconds -- to initialize its own renderer and produce
its first real GamePad frame. So `capabilities()`'s one-time Hello-time
snapshot was, in practice, *always* taken before `hasFrame` had ever
been set, making the "fall back to a default until the real size is
known" logic effectively dead code: every session's declared value
stayed the wrong hardcoded default for its entire lifetime, exactly
reproducing the original bug.

**Actual fix**: the real per-frame width/height now travels *with the
frame itself*, not as a value negotiated once at Hello time.
`SurfaceFrame` (the adapter contract, `adapter_contract.h`) gained
`width`/`height` fields -- contract version bumped 1 -> 2 -- carried
across the adapter IPC wire format (`ipc_protocol.h`/`.cpp`'s
`serializeSurfaceFrame`/`parseSurfaceFrame`). `CemuAdapter::latestFrame()`
now populates them from the same `CapturedSurface::width/height` every
call (the data was always being captured correctly -- it just wasn't
reaching anywhere that mattered before). On the Host Service side,
`IFrameSource::getLatestFrame()` gained matching `outWidth`/`outHeight`
out-parameters, and `AdapterBridge` forwards the real per-frame values
(falling back to the declared default only if an adapter reports 0x0,
i.e. hasn't been updated for contract v2). `NetServer::videoLoop()` now
re-reads these on every single tick instead of once before the
connection's inner loop, and feeds the *current* frame's real size into
`tjCompress2` for JPEG encoding -- so the encoder can never again be
told the wrong width for the buffer it's actually reading.

The client side needed a matching fix: it used to require an exact match
between a decoded JPEG's real dimensions and the one value HelloAck
negotiated at connect time, closing the connection outright on any
mismatch (which the corrected host-side behavior would now trigger
constantly, since the real per-frame size and the once-negotiated value
legitimately differ). `decompressJpegToBgra()` now decodes at whatever
size the JPEG itself actually declares (already read via
`tjDecompressHeader3()`, previously only used to validate against the
expected size, not to drive the decode). `NetClient` keeps
`hostNativeWidth()`/`hostNativeHeight()` in sync with the most recently
decoded frame's real size, and `main.cpp`'s render loop now checks them
every frame (not just on connect/mode-transition) and recreates its SDL
texture the moment they change -- so a genuine mid-session resolution
change is picked up within a frame or two instead of needing a
reconnect, and Cemu's real capture size never has to match a value
guessed before the title even finished loading.

This required marking `CapturedSurface::mutex` `mutable` (kept from the
first fix, still needed for `capabilities()`'s own fallback reporting).
Azahar and melonDS/DS don't have the underlying bug this fully fixes
(their declared and actual capture sizes were already always
consistent: DS/3DS have one genuinely fixed native resolution, Azahar's
declared size already derives from the same `captureScale_` its actual
capture uses) but both were updated to populate `SurfaceFrame::width/
height` too, for contract-v2 completeness and because the new
per-frame path is what the whole pipeline now relies on.

**Verified**: Azahar and melonDS both rebuild clean in this project's
sandbox (both are real, compilable checkouts here) after this change;
all 6 host/client ctest suites pass. **Cemu still cannot be built or run
in this sandbox** (confirmed repeatedly throughout this integration), so
only that one patch is diff-verified, not compiled -- still needs a CI
build and, ideally this time, a confirmed-successful retest against
Twilight Princess HD and Pokemon Rumble U before calling this resolved.

## 2026-07-23: Discovery silently found zero hosts across a client/host protocol-version mismatch (all adapters, not melonDS-specific)

Reported as "MelonDS is seemingly no longer transmitting a server to
connect to" -- but the actual bug is in the client<->host LAN discovery
path (`DiscoveryRequest`/`DiscoveryResponse`), which every adapter
(melonDS, Azahar, Cemu, synthetic) shares equally; whichever emulator
was running at the time was incidental, not the cause.

`NetServer::discoveryLoop()` and `discovery_client.cpp`'s response
parser each required `header->protocolVersion == kProtocolVersion`
before accepting a `DiscoveryRequest`/`DiscoveryResponse` -- an exact
match, with no logging or fallback on a mismatch, silently treating a
well-formed request/response from a different protocol version exactly
like line noise. `kProtocolVersion` was bumped twice in quick succession
this session (v7 JPEG compression, v8->v9 per-client video quality) --
if a host auto-updates (`check-for-updates.sh`) before its client does,
or vice versa, the two sides end up on different versions, and the
client's host-selection screen shows nothing at all, with no error
anywhere to explain why. This is a strictly worse failure mode than the
version mismatch this project already handles well: the Hello/HelloAck
handshake (performed *after* a client picks a host from the discovery
list) already rejects a real version mismatch with an explicit
`AppVersionMismatch` error shown to the user -- discovery never even
got that far.

Fixed by dropping the protocol-version-equality check from both sides
of discovery specifically (`DiscoveryResponsePayload`'s wire format
hasn't changed across any of these version bumps, so an older or newer
side can always decode it regardless of version). A mismatched host now
still shows up in the list; attempting to connect to it still correctly
surfaces the existing `AppVersionMismatch` error from the Hello/HelloAck
handshake, instead of the host disappearing from view with zero
explanation. Every other packet type's version check is untouched --
this is scoped to discovery only, where the sole purpose is "is there a
host here to try," not compatibility enforcement.

## 2026-07-23: Client-side persistent log file + log forwarding to the host

Reported by the user: latency and Wii U touch problems needed better
diagnosis, and "client should send logs to the host for host debugging
and app development." Before this, the client logged exclusively via
`std::fprintf(stderr, ...)` (~40 call sites across `main.cpp`/
`net_client.cpp`, plus one in `discovery_client.cpp`), deliberately not
stdout (an existing comment explains stdout's buffering risk once
redirected to a file). No log file was ever created for a run, and
`run-client.sh` just `exec`s the binary with no redirection -- in Steam
Big Picture/Gaming Mode, where there is no visible terminal, this meant
every one of those diagnostic lines was completely unrecoverable after
the fact.

**Local log file** (`client/src/client_log.h`/`.cpp`, new files): a
shared `logLine()` function replaces every one of those ~41 call sites
(`std::fprintf(stderr, ...)`/`std::perror(...)`), writing the same
formatted line to stderr (unchanged behavior) and, if `initClientLog()`
succeeded at startup, appending it to `~/.config/dualdeck-client/
client.log` too -- truncated fresh each run rather than appended
forever, matching this client's existing practice of not persisting
more than one run's worth of diagnostic state. Thread-safe (a single
mutex covers both destinations) since `net_client.cpp`'s background
receive threads and `main.cpp`'s main thread all log concurrently.
Falls back to stderr-only, silently, if `$HOME` is unset or the
directory can't be created.

**Log forwarding to the host** (GitHub-issue-style feature, not tied to
a specific numbered issue): a new `PacketType::ClientLog` (client ->
host, TCP control channel, see `docs/protocol.md`'s matching section)
carries one already-formatted log line. `client_log.h` gained
`setLogForwardSink()`, wired up in `main.cpp` right after each
`NetClient` is constructed to call `NetClient::sendClientLog()`, with a
scope-guard clearing it again before that same `NetClient` is destroyed
(picking a new host, or reconnecting, must never leave a sink holding a
reference to an already-destroyed object). `sendClientLog()` is
best-effort and non-blocking by design: it silently drops a line if not
currently connected or its own bounded queue (64 lines) is already
full, since losing a diagnostic message is a vastly smaller problem
than this call stalling a hot path or building an unbounded backlog
while disconnected. The queue is drained every ~50ms by the existing
heartbeat thread (already waking that often) rather than a new thread.

Like `ModeChanged` before it, `ClientLog` is a brand new packet type an
older peer simply never sends/reads -- it does not change any existing
struct's shape, so it did **not** require a `kProtocolVersion` bump
(stayed at 9). The host (`NetServer::controlLoop()`) prints each
received line to its own stderr prefixed with the sending client's
address, and also invokes a new optional `NetServerConfig::
onClientLogLine` hook. This required the control loop's post-handshake
read loop to actually read off a packet's declared payload for the
first time (previously only `Heartbeat`/`Disconnect`, both payload-free,
ever arrived there) -- bounded at 4096 bytes to reject an implausible
declared size before allocating for it.

**Verified**: `protocol/tests/test_client_log.cpp` covers
serialize/parse round-trips, truncation of an overlong line, and every
malformed-input rejection path (short buffer, declared length over the
max, truncated payload, trailing garbage) — mirroring the coverage
`test_mode_changed.cpp` already has for `ModeChanged`. A real end-to-end
test (`client/tests/test_net_client.cpp`'s
`net_client_forwards_a_log_line_to_the_host`) proves a real
`NetClient::sendClientLog()` call reaches a real `NetServer` over a real
loopback socket and arrives via `onClientLogLine` with the exact
address and line, not just that the wire encoding round-trips in
isolation; a second test confirms a line sent while never connected is
silently dropped rather than queued or blocking. All 6 host/client
ctest suites pass. A full GUI-client run (the actual `dualdeck-client`
SDL binary) was not exercised end-to-end in this sandbox -- no display
and no gamepad device nodes are available here, the same limitation
this project has hit for every other client-UI change; the real
`NetClient`/`NetServer` proof above exercises the identical networking
code path the GUI binary uses, just without SDL's windowing/input layer
around it.

## 2026-07-23: Video-latency instrumentation (network+encode+queue, plus local decode time)

Reported alongside the log-forwarding request above: "latency is still
poor. we need ways to diagnose this and get measurements." Before this,
the only latency number this project ever measured was the input path
(`NetServer`'s `latencySampleCount`/`latencySumUs`/`latencyMinUs`/
`latencyMaxUs`, from `ControllerState.clientTimestampUs`) -- there was
no equivalent for video at all, so "latency feels bad" had nothing
concrete to point at beyond frame rate/dropped-frame counters.

`VideoFrame`'s payload (protocol v10, bumped from v9 -- see
`docs/protocol.md`'s matching section) now carries an 8-byte host
wall-clock timestamp before the JPEG bytes, taken in
`net_server.cpp`'s `videoLoop()` immediately before that frame's
encoding begins. `client/src/net_client.cpp`'s `videoReceiveLoop()`
compares it against its own receipt wall-clock time to estimate
network + encode + send-queue latency for the video path -- the same
"assumes synced clocks" caveat the input-latency stat already
documents applies here too, for the same reason (no NTP-style
handshake exists between client and host to correct for clock skew).
Separately, purely client-side `steady_clock` timing (no clock-sync
concern at all, since both ends of that measurement happen on the same
machine) wraps the `decompressJpegToBgra()` call to measure local JPEG
decode time. Both are accumulated into a running min/max/avg (mirroring
`NetServer`'s own periodic-stats bookkeeping shape) and logged via
`logLine()` every 5 seconds -- which, thanks to the log-forwarding
feature above, also reaches the host automatically without any extra
plumbing.

Unlike `ModeChanged`/`ClientLog` (new packet types an older peer can
simply never send/read), this **did** require the `kProtocolVersion`
bump: `VideoFrame`'s existing payload shape changed, so an old client
reading a new-format frame would misinterpret the leading timestamp
bytes as JPEG data (and vice versa) -- silent corruption, not a
graceful ignore.

**Verified**: `protocol/tests/test_video_frame.cpp` covers
`VideoFramePayload` serialize/parse round-trips (including an empty
JPEG portion) and the too-short-for-even-the-timestamp rejection path.
`host/remote-server/tests/test_net_server_mode_switch.cpp`'s existing
real end-to-end video-delivery tests needed updating for the new
prefix (`readVideoFrameFirstByte()` now strips the timestamp via
`parseVideoFramePayload()` before handing the JPEG portion to
`tjDecompress2`) and continue to pass, proving actual pixel content
still round-trips correctly through the new wire format, not just that
the timestamp itself parses. `client/tests/test_net_client.cpp`'s real
frame-delivery tests (`NetClient` decoding an actual resized frame from
a real `NetServer`) also continue to pass unmodified, confirming
`videoReceiveLoop()`'s new timestamp-stripping step doesn't disturb the
decoded pixel content it already asserted on. All 6 host/client ctest
suites pass. Not verified against real measured numbers on real
hardware in this sandbox (no real emulator capture pipeline or network
link to measure) -- the instrumentation itself is proven correct, but
what it will actually report on a real Steam Deck-over-Wi-Fi session
against Cemu/Azahar/melonDS remains to be seen.

## 2026-07-23: Cheap latency-tuning pass (no live measurements yet)

Following the video-latency instrumentation above (built specifically
because no real numbers existed yet to tune against), this pass applied
the one concrete, already-visible-by-inspection latency issue found
while reviewing the video pipeline, and documents what else was
reviewed and deliberately left alone pending real measurements from the
instrumentation now in place.

**Applied: size-aware default JPEG quality.** `NetServerConfig::
videoJpegQuality` (default 80) turned out to be the *only* value ever
actually used for a client that doesn't send its own `HelloPayload::
videoQuality` override (protocol v9) -- there is no `--video-quality`
CLI flag on the host binary at all, so `run-host.sh`/
`run-host-azahar.sh`/`run-host-cemu.sh` all launch with the exact same
compiled-in default regardless of which adapter is actually driving the
session. That default was tuned for DS/3DS-sized surfaces (49k-77k
pixels); applying it unchanged to Cemu's much larger GamePad surface
(854x480, ~410k pixels -- 5-8x more pixels than DS/3DS) produces
proportionally larger JPEGs at the same quality, directly adding to
per-frame encode time, network transmit time, and how often the video
loop's own `SO_SNDBUF` backpressure (see below) actually has to kick
in -- a real, concrete contributor to "latency is poor" on Wii U
sessions specifically, visible from code inspection alone without
needing a live measurement. `net_server.cpp`'s new
`defaultVideoQualityForFrameSize()` (declared in `net_server.h`,
unit-tested directly, same pattern as `mode_coordinator.h`'s
`computeDesiredMode()`) resolves the fallback once per connection from
that connection's own real negotiated frame size (HelloAck's
`nativeWidth`/`nativeHeight`), not the emulated system's identity --
correct for any future large-surface adapter too, not just Cemu. Above
150,000 pixels, the configured default is capped at 60 (never *raised*
if an operator already configured something lower, e.g. for a
known-slow link). This also compounds with the existing quality->=90
chroma-subsampling threshold in `compressFrameBgraToJpeg()`: Cemu's new
effective default (60) stays well under that threshold, so it
automatically keeps using the cheaper 4:2:0 subsampling too, without
any separate change needed there.

**Reviewed and left alone: `CEMU_REMOTE_CAPTURE_FPS`.** Already
defaults to 30 (`host/cemu-patches/`'s `CemuAdapter.cpp`), explicitly
matching this project's own established precedent for a small streamed
display (`AzaharAdapter.cpp`'s identical default/reasoning) rather than
capturing at Cemu's own 60-144fps render rate, which would add a GPU
sync stall to every frame whether or not a client is even watching.
Already reasonable; no evidence from code inspection alone that this
specific number is the problem, and it's already overridable per-launch
via the env var if a real measurement later says otherwise.

**Reviewed and left alone: `SO_SNDBUF` backpressure sizing.**
`net_server.cpp`'s `videoLoop()` sizes the socket send buffer to
roughly two frames' worth of the *estimated compressed* size (not raw
size), so a link too slow to keep up blocks (bounded by a 1s
`SO_SNDTIMEO`) rather than letting several stale frames queue up in the
kernel -- already the right shape of design (bound staleness, don't let
TCP's in-order guarantee silently accumulate latency) per its own
extensive existing comment. The quality fix above shrinks Cemu's actual
compressed frame size, which shrinks this buffer's sizing estimate too
(same formula, smaller input) -- a free, compounding improvement,
without touching this logic directly. Not otherwise changed: tightening
or loosening the "roughly two frames" constant without a real
measurement of what's actually happening on a real link risks trading
one guess for another.

**Not done, and deliberately so**: no attempt to guess at further
tuning (frame-skip aggressiveness beyond what's described above,
adaptive quality that reacts to measured latency in real time, etc.)
without the real numbers the instrumentation above now makes possible
to collect. The next real step is gathering actual `videoReceiveLoop()`
latency/decode-time log output from a real session (Steam Deck over
Wi-Fi, ideally against Cemu specifically) before tuning further.

## 2026-07-31: HostControlAdapter restored -- companion-mode host navigation is a load-bearing part of the orchestration-layer pivot

A same-day commit (`f2e1c97`, on this same branch's history) removed
`HostControlAdapter` as an unused-in-practice feature (see the git log
for its own reasoning) shortly before a separate, concurrent effort on
this exact codebase committed to an architecture pivot where the host
navigating its own Steam Big Picture via a client-driven virtual gamepad
-- i.e. `HostControlAdapter`'s own purpose -- is the foundation "Host
State Model"/"Role-Based Client Behavior" work builds on (see this same
file's 2026-07-31 "Capability-negotiation protocol revision" entries).
Restored via `git revert` of that removal, keeping every other change
from the same run of commits (per-client video quality, Cemu tearing
fixes, client-side log forwarding, video-latency instrumentation, the
size-aware JPEG quality default) intact -- those are all real,
independent improvements with no relationship to this specific removal.

`host_control_adapter.{h,cpp}` and its test are back, `ModeCoordinator`
again points `NetServer` at `HostControlAdapter` (not
`NoAdapterInputSink`/`NoAdapterFrameSource`) during the "no adapter
connected" gap, the manual `hostcontrol`/`resume` console override and
`computeDesiredMode()`'s `manualHostControlOverride` parameter are back,
and the client's `renderHostControlScreen()` shows "HOST CONTROL" again
instead of "WAITING FOR EMULATOR". `scripts/build-release.sh`'s "Host
control only -- no emulator (experimental)" picker entry and
`DUALDECK_HOST_CONTROL` env var are restored too.

This does not retroactively invalidate the original removal's technical
critique (real maintenance surface, never exercised against real
`/dev/uinput` hardware in this project's sandbox, no proven real-world
workflow yet) -- it's a scoping decision, not a disagreement that the
concerns were valid. The orchestration-layer pivot's own roadmap (Phase
C in this file's earlier entries) already treats real host-desktop
capture/navigation as substantial, independently-risked follow-up work,
not something this restoration alone makes production-ready.

## 2026-07-31: Capability-negotiation protocol revision, Phase 0 (foundational, zero wire-format change)

Follow-up to the EmuDeck replace-in-place phase above, on a separate,
explicitly parallel track: an architectural revision requesting a
platform-agnostic, capability-negotiated protocol (generic displays/
input/features instead of DS/Wii-U-specific concepts), a pluggable
`IStreamingBackend` abstraction, a richer host state machine, role-based
client behavior, and telemetry -- see `docs/adr/0001-host-service-and-
adapter-architecture.md` for the architecture this builds on.

**Key finding before any of this was designed**: most of the requested
capability/plugin architecture already exists, unwired to the wire
protocol -- `adapter-sdk`'s `IEmulatorAdapter`/`AdapterCapabilities`/
`GenericInputState`/`VideoSurfaceDescriptor`/`SessionState` (built for
GitHub issue #28) already form a generic, proven, out-of-process-capable
contract (Azahar and Cemu are both out-of-process-only against it
today). ADR 0001 section 4 deliberately chose a DS-compatibility
translator (`AdapterBridge`) over touching the wire protocol, reasoning
that no second real adapter existed yet to validate a generic wire
format against. That reasoning no longer fully holds -- three real
adapters exist now, one (Cemu) already carrying real multi-surface data
that never reaches the client. This reframes the whole effort as
extending an existing, proven internal seam out to the wire protocol,
not inventing a new architecture from scratch. Full design + phased plan
(Phases 0-4, plus explicitly flagged tensions with the literal request,
e.g. true cross-version wire interoperability was explicitly decided
against -- hard-reject on `kProtocolVersion` mismatch stays exactly as
every prior version bump handled it) is not reproduced here in full; see
the design record for `capability_bridge.h`/`host_session_state.h`'s own
header comments for the parts that shipped.

**What Phase 0 actually ships** (foundational, additive, no wire-format
change, verified by a real `-DDUALDECK_BUILD_HOST=ON` build + `ctest` +
both smoke tests, all passing):

- `host/remote-server/include/host/host_session_state.h`/`.cpp`: a new
  `HostSessionState` enum (Idle/Discoverable/Pairing/Connected/
  Streaming/Launching/EmulatorRunning/CompanionModeActive/Suspending/
  Sleeping/Error) plus `isValidTransition()`, mirroring adapter-sdk's
  `SessionState`/`isValidTransition()` pattern exactly. Not yet sent
  over the wire (`HostMode`'s existing 2 values are still what
  `HelloAckPayload`/`ModeChangedPayload` actually carry) -- several
  states (Discoverable/Pairing/Streaming/CompanionModeActive/
  Suspending/Sleeping) have no real signal source in the codebase yet
  and are defined now specifically so wiring them up later doesn't
  require inventing new states under time pressure.
- `ModeCoordinator` gains `computeDesiredHostSessionState()` (a pure
  function alongside the existing `computeDesiredMode()`) and a new
  `currentSessionState()` accessor that reads
  `AdapterIpcServer::currentState()` in addition to the
  `hasConnectedAdapter()` bool `computeDesiredMode()` already used --
  the first real consumer of that adapter-state proxy outside
  adapter-sdk's own tests. Deliberately ignores the adapter's
  last-known `SessionState` once `hasConnectedAdapter()` goes false
  (`resetSessionLocked()` leaves it stale on disconnect rather than
  resetting it -- a real trap this function avoids on purpose, not an
  oversight).
- `protocol.h` gains a new, self-contained "Capability negotiation data
  model" section: `WireDisplayDescriptor`/`WireClientCapabilities`/
  `WireHostCapabilities`/`WireDisplayRole`/`WirePixelFormat`/
  `WireCodec`, mirroring adapter-sdk's `VideoSurfaceDescriptor`/
  `AdapterCapabilities` shapes without `#include`-ing adapter-sdk (which
  already depends on `protocol.h` for `SystemIdentity`/`AdapterIdentity`
  -- the reverse dependency would be circular). No serialize()/parse()
  functions exist for these yet and neither `HelloPayload` nor
  `HelloAckPayload` carries them -- purely a stable data model for a
  later negotiation phase to extend those payloads into, so that phase
  is an additive field-add plus one version bump rather than also
  inventing this shape under time pressure. `WireDisplayRole` keeps the
  existing Nintendo-hardware-flavored values (Top/Bottom/Tv/GamePad)
  rather than renaming them -- renaming would force a matching change in
  every already-shipped adapter's own `capabilities()` for no functional
  gain -- while adding generic Primary/Secondary values alongside them
  for future non-Nintendo-shaped clients.
- `host/remote-server/include/host/capability_bridge.h`/`.cpp`: the
  translation layer between the two shapes above (`toWireDisplayRole()`,
  `toWirePixelFormat()`, `toWireDisplayDescriptor()`,
  `toWireHostCapabilities()`) -- the one place allowed to depend on both
  `adapter-sdk` and `protocol.h`'s new types, same architectural role as
  `AdapterBridge` one level down (translating generic input/frames to
  the DS-specific wire shape) applied one level up (translating generic
  capabilities to the not-yet-sent Wire* shape). Not called by anything
  yet, but the translation logic itself is real and unit-tested now,
  including against Cemu's real two-surface (Tv + GamePad) shape.

**Explicitly not done in this phase** (later phases, per the full design
record): the actual `kProtocolVersion` bump and `Hello`/`HelloAck`
negotiation wiring; a real `GenericServerBridge` alongside `AdapterBridge`
for negotiated-capable clients; role derivation and client-side additive
render branches; `IStreamingBackend` extraction of the existing JPEG
pipeline; real display enhancement (CAS/NIS/integer scaling); telemetry;
and the plugin manifest/launch-hooks/configuration surface for emulator
adapters. None of these are blocked technically by anything in Phase 0 --
they're sequenced later because each is a larger, independently-risky
unit of work in its own right.

## 2026-07-31: EmuDeck replace-in-place installer (Phase A of the orchestration-layer pivot)

Until now, using DualDeck meant installing its own separately-built copy
of melonDS/Azahar/Cemu, with no relationship at all to any emulators or
ROMs a user already had set up via EmuDeck -- a real usability gap,
raised directly: users with an existing EmuDeck library had to manually
point DualDeck at their games instead of just launching them through the
EmuDeck/Steam shortcuts they already had.

`scripts/emudeck-replace-in-place.sh` (new) addresses this by building
DualDeck's already-patched melonDS/Azahar/Cemu the same way
`scripts/build-release.sh` does (both now share the actual build logic
via `scripts/lib/build_emulator.sh` and the pinned-commit constants via
`scripts/lib/pinned_commits.sh`, rather than two copies that could
silently drift apart -- see `build_azahar()`'s own comment for why that
class of bug has bitten this project before), then installs the result
**directly at the exact path EmuDeck's own launcher scripts and Steam
shortcuts already point to** (`~/Applications/*.AppImage`, packaged as a
proper AppImage via `scripts/lib/appimage_pack.sh`), instead of a
separate DualDeck-managed install directory. Nothing about an existing
EmuDeck install or its Steam shortcuts needs to change -- they simply run
DualDeck's remote-streaming-capable binary the next time they're
launched.

**Detection** (`scripts/lib/emudeck_paths.sh`): matches EmuDeck's own
"newest AppImage by mtime matching a name fragment" convention (the same
one EmuDeck's own launcher scripts, e.g. `tools/launchers/cemu.sh`, use)
rather than assuming one fixed filename. Cemu's Flatpak-fallback install
shape (EmuDeck's own AppImage-then-Flatpak-then-Proton priority order) is
detected and cleanly refused rather than silently doing nothing -- a
Flatpak's sandboxing and `flatpak run <app-id>` launch model is a
materially different install shape this phase doesn't attempt to handle.

**melonDS vs. Azahar/Cemu, at the `AppRun` level**: melonDS already has a
persisted Qt-config toggle (`MelonDSRemote.Enable`) and runs its remote
server in-process by default, so its `AppRun` is a trivial passthrough
exec. Azahar and Cemu have no in-process path at all -- they always need
a running Host Service to connect to over adapter-ipc -- so their
`AppRun` first probes the *default shared* adapter socket
(`adapter-sdk/ipc/src/socket_path.cpp`'s `defaultAdapterSocketPath()`)
for an already-running persistent daemon, and only falls back to
spawning a private, ephemeral `dualdeck-host-service` (killed on exit,
exactly like today's packaged `run-host-azahar.sh`/`run-host-cemu.sh`) if
none is found. No persistent daemon exists yet as of this phase (that's
separately-scoped follow-up work) -- today, every launch takes the
ephemeral fallback path, same as before this installer existed. Once a
persistent daemon does exist, every `AppRun`-launched emulator picks it
up automatically with no need to rebuild or reinstall the AppImage.

**Drift detection** (`scripts/lib/appimage_manifest.py`,
`scripts/emudeck-check-drift.sh`): a sidecar JSON manifest
(`<name>.AppImage.dualdeck.json`, deliberately *not* embedded inside the
AppImage itself, so it survives EmuDeck's own "Manage Emulators" updater
doing a straight file replace) records the patched and original sha256
hashes. `emudeck-check-drift.sh` compares the live file against both on
demand; a mismatch against both means EmuDeck's updater replaced the
file with a newer stock build DualDeck has never patched, and needs
re-patching (`--fix`) to regain remote-server capability. This can only
ever detect drift *after* it happens -- if EmuDeck's updater runs and the
user launches the game before the next drift check, they transiently get
an unpatched emulator with no remote-server capability, not a crash or
error. That degrade-to-stock behavior is an accepted tradeoff, not an
oversight.

**Safety**: the very first install backs up the true original AppImage
(`<name>.AppImage.dualdeck-original`, named so it can never be
accidentally picked back up as "the newest install" by the detection
glob above, which only ever matches `*.AppImage`). Every later run
reuses that same backup and the manifest's recorded `original_sha256`
rather than re-deriving "original" from whatever is currently
installed -- otherwise a second run after the first would back up
DualDeck's *own* patched build as if it were the stock original,
permanently losing the ability to restore true stock. If a manifest
exists but its backup is missing, the installer refuses outright rather
than guessing. Requires interactive confirmation unless `--yes` is
given; `--dry-run` performs detection and reports the plan with zero
side effects (in particular, it never triggers a build-dependency
install).

**Not yet verified** (this project's development sandbox has no real
EmuDeck install to test against): whether EmuDeck's actual AppImage
naming/glob convention matches what `emudeck_paths.sh` assumes from
EmuDeck's own public docs/launcher-script source. This needs confirming
against a real EmuDeck install before this installer can be considered
done, not just reasoned through -- see `scripts/lib/emudeck_paths.sh`'s
own header comment.

**Deliberately out of scope for this phase** (see the broader
orchestration-layer roadmap this phase is the first part of): a
persistent host-service daemon (so `AppRun`'s default-socket probe above
actually finds something to connect to on a normal, direct EmuDeck/Steam
shortcut launch); moving melonDS off its in-process default; the host
booting real Steam Big Picture; actual host-desktop screen capture for
HostControl mode (`HostControlAdapter::getLatestFrame()` still always
returns `false`); and the Decky plugin becoming a full discovery/connect
client. Each is substantially more work than this phase, and each is
independently scoped rather than folded in here.

## Things intentionally out of scope for v0.1

Per `SPEC.md` section 21 (explicit non-goals): ROM transfer, cloud saves,
internet play, multiple simultaneous clients, user accounts, remote
desktop/filesystem browsing, Android/Windows/iOS clients, camera
emulation, rumble, voice chat, spectator mode, artwork scraping, cheat
databases, a custom emulator core, or replacing melonDS's rendering.
Voice chat *between users* and streaming the host's own game audio back
to the client remain out of scope (GitHub issue #2's own "Out of scope"
section) even though DS-microphone-to-host input is now implemented.
These are not bugs or gaps in this implementation -- they are
deliberately not attempted yet.
