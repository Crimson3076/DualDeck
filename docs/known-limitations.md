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
