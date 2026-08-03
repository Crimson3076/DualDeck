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

**Verified against a real EmuDeck install** (2026-07-31, Fedora): the
user ran `--dry-run` from the packaged `host/emudeck-integration/`
bundle (see the entry below) against their own real EmuDeck setup and
confirmed detection works correctly -- found `azahar.AppImage` and
`Cemu.AppImage` under `~/Applications/`, correctly reported the backup
paths it would create (`azahar.AppImage.dualdeck-original`,
`Cemu.AppImage.dualdeck-original`, both outside the `*.AppImage`
detection glob as designed), and stopped cleanly with zero side
effects. melonDS wasn't installed via EmuDeck on this machine, so that
detection path (and a real, non-dry-run install of any emulator)
remains unverified. This was previously this phase's single biggest
open risk (this project's own development sandbox has no EmuDeck
install to test against) -- it no longer is, for Azahar/Cemu's naming
convention specifically.

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

## 2026-07-31: EmuDeck integration tool bundled into the packaged release

Reported directly: after downloading a release to test the EmuDeck
replace-in-place installer above, "let's package it so I can just use
the existing script without any other downloads" -- until this point,
`emudeck-replace-in-place.sh` was source-tree-only; using it meant a
separate `git clone` of this repo alongside whatever release archive
was already installed.

`scripts/build-release.sh` now bundles `emudeck-replace-in-place.sh`/
`emudeck-check-drift.sh` and their `scripts/lib/` dependencies (plus
the three emulator patch files they apply) into every release archive,
at `host/emudeck-integration/`, mirroring the exact relative layout
those scripts already expect from a real source checkout
(`<root>/scripts/...`, `<root>/host/<emulator>-patches/...`) -- so
neither script needed any path-handling changes, only a version-
detection fix (see below). This location was chosen deliberately, not
arbitrarily: `install-steam-shortcut.sh`'s and
`install-host-distrobox.sh`'s existing host update/install path
(`cp -a "${host_root}/."`) already recursively copies *any* new
subdirectory under `host/` into `~/.config/dualdeck/install/` -- so
this bundle reaches an existing install via the ordinary "Check for
updates" flow already covered above, with zero changes needed to
either install/update script.

**Fixed alongside**: `emudeck-replace-in-place.sh` computed its own
version via `git describe`/`git rev-parse`, which would have hard-
failed under `set -e` when run from a packaged release archive (no
`.git` directory at all). Now prefers a `VERSION` file (written by the
packaging step above, same `version_tag` every other part of the
release reports) when present, falling back to `git describe`/
`git rev-parse` only from a real source checkout, and never hard-fails
either way.

**Verified**: bundled the packaging step's output in isolation (without
running the full multi-hour emulator build) against a mock
`~/Applications/` directory -- `--help` and `--dry-run` both run
correctly from the bundled `host/emudeck-integration/scripts/`
location, confirming `repo_root`/patch-path resolution works
unmodified from that location, and the version-detection fix correctly
reads the bundled `VERSION` file instead of attempting `git`. Not yet
verified as part of an actual full release build/download/"Check for
updates" cycle end to end.

## 2026-07-31: Fedora Azahar build fix -- missing qt6-qtbase-private-devel

First real end-to-end run of `emudeck-replace-in-place.sh` on real
Fedora hardware (following the detection verification above) hit a
genuine build failure compiling Azahar: `cmake` failed configuring
`src/citra_qt` with "Failed to find required Qt component GuiPrivate"
-- `Qt6GuiPrivateConfig.cmake` wasn't present anywhere on the system.

Root cause: the apt (Debian/Ubuntu) build-dependency list has carried
`qt6-base-private-dev` for Azahar's `Qt6::GuiPrivate` requirement, but
the dnf (Fedora) list never got the equivalent package added -- exactly
the class of "the two lists silently drifted apart" bug this project
has hit more than once this same day (the wayland-client/turbojpeg
dnf-list bugs earlier this session were the same shape). Confirmed via
web search (not guessed, after getting a Fedora package name wrong
once already this session) that `qt6-qtbase-private-devel` is the
correct package -- it's what actually ships
`Qt6GuiPrivateConfig.cmake` on Fedora.

Added to all three places this dependency list is duplicated:
`scripts/build-release.sh`'s top-level `ensure_packages "build"` call,
its Distrobox-container dnf install list (for immutable/Bazzite hosts),
and `scripts/emudeck-replace-in-place.sh`'s own copy of the same list.
This was previously the last explicitly-flagged "Fedora/Arch names are
best-effort and unverified" gap for Azahar's build dependencies (see
`docs/azahar-integration-analysis.md`) -- confirmed and fixed against
real hardware rather than reasoned through.

**Not yet verified**: whether this was the *only* remaining Fedora
dependency gap in Azahar's build, or whether more turn up once the
build gets further with this fix applied. Cemu's build (much larger
dependency graph, ~108 vcpkg packages) hadn't been reached yet at the
time of this fix.

## 2026-07-31: Persistent build cache for emudeck-replace-in-place.sh -- drift re-patches skip recompilation

`emudeck-check-drift.sh --fix` re-invokes `emudeck-replace-in-place.sh`
whenever EmuDeck's own "Manage Emulators" updater silently replaces an
installed AppImage with a newer stock build. Until now that meant a
full recompile every time -- expensive for melonDS/Azahar and very
expensive for Cemu -- even though DualDeck's own patched binary is
usually still exactly what's needed, since EmuDeck updating its stock
AppImage has no bearing on what DualDeck itself builds.

Added `scripts/lib/build_cache.sh`: a persistent, cross-invocation
cache under `~/.cache/dualdeck/emudeck-builds/<emulator>/`, storing the
raw compiled binary from `build_melonds()`/`build_azahar()`/
`build_cemu()` (not the packaged AppImage -- packaging/`AppRun`
generation is cheap and always re-run fresh) keyed on a fingerprint of
`sha256(patch file):pinned commit`. `replace_in_place_one()` in
`scripts/emudeck-replace-in-place.sh` now calls `try_cached_build()`
before each `build_*` call and `save_build_cache()` after a fresh
build, for all three emulators uniformly.

This is deliberately a *different* cache from `build_azahar()`/
`build_cemu()`'s own existing cache-hit check in
`scripts/lib/build_emulator.sh` (a `git apply --reverse --check`
against that single run's own scratch `work_dir`, deleted on exit by
this script's `EXIT` trap) -- that one only avoids re-cloning within
one invocation; this one is what actually makes a *later* invocation
(e.g. a drift-triggered re-patch, potentially days or weeks after the
first install) skip rebuilding entirely.

The fingerprint is intentionally decoupled from whatever stock version
EmuDeck's updater grabbed: a cache hit is correct precisely when
neither DualDeck's own pinned commit (`scripts/lib/pinned_commits.sh`)
nor the patch file content has changed, regardless of what changed on
EmuDeck's side. If DualDeck itself bumps a pinned commit or edits a
patch, the fingerprint changes and the next build is a normal full
compile that then repopulates the cache.

Verified in isolation (mocking `save_build_cache`/`try_cached_build`
against a scratch `$HOME`, not a real emulator build): cache miss on
first call, hit after a save with the same patch+commit, independent
copy returned (mutating it doesn't corrupt the cache entry), miss after
either the commit or the patch content changes, and separate cache
namespaces per emulator. Bundled into the release archive alongside the
rest of `host/emudeck-integration/` (`scripts/build-release.sh`).

**Not yet verified**: a real cache hit on real EmuDeck-drift hardware
end to end (this was implemented and unit-tested in isolation, ahead of
a real drift event to exercise it against).

## 2026-07-31: EmuDeck replace-in-place installer is now a DualDeck Host launcher menu option

Previously the only way to run `emudeck-replace-in-place.sh` from a
packaged release was to open a terminal and `cd` into
`host/emudeck-integration/scripts/` manually -- undiscoverable next to
`dualdeck-host.sh`'s existing "the one thing to double-click" menu
(GitHub issue #10's own goal, which every other host action already
follows). Added a "Patch my EmuDeck-installed emulators (experimental)"
choice to `dualdeck-host.sh`'s top-level menu (`choose_action()`, both
the kdialog and terminal-numbered-prompt versions), wired to a new
`internal/launch-emudeck-integration.sh`.

This menu choice is a different shape from every other one already
there: it's a long (minutes, worse for Cemu), verbose source build with
its own per-emulator y/N confirmation read from the terminal, not a
quick kdialog-driven action. `launch-emudeck-integration.sh` handles
both ways `dualdeck-host.sh` itself can end up running:
- If a terminal is already attached (`[[ -t 1 ]]`) -- e.g. launched
  from a terminal, or a file manager that runs `.sh` files "in a
  terminal" -- it execs the bundled `emudeck-replace-in-place.sh`
  directly in that same terminal, same as every other menu choice.
- If not -- the common case when launched from Steam/Gaming Mode, which
  attaches no terminal at all -- it re-launches itself inside a
  terminal emulator window (tries `konsole`, `xterm`,
  `x-terminal-emulator`, `gnome-terminal`, in that order; SteamOS and
  Bazzite are both KDE Plasma, so `konsole` is expected to be the
  common case) so the build output and confirmation prompts are
  actually visible. If none of those exist, falls back to a kdialog
  error message giving the exact command to run manually, rather than
  silently hanging waiting for terminal input nobody can see (the same
  "surface failures visibly" posture as this script's own `on_error`
  trap and every other `dualdeck-host.sh` action).

**Not yet verified**: launching this specific menu choice from inside a
real Steam Gaming Mode session on real hardware (confirmed only via
`bash -n` and extracting/syntax-checking the generated heredoc script
in isolation) -- in particular, whether `konsole -e` reliably opens and
stays focused when invoked from a process Steam itself launched.

## 2026-07-31: Real-hardware feedback (Bazzite HTPC) -- curl-pipe installer, EmuDeck launcher UX, both fixed

First real install on a Bazzite HTPC via `DualDeck-Installer.sh` surfaced two concrete UX problems, both fixed:

**1. Installer required a manual download + `chmod +x`.** Painful to do
with a Steam Controller instead of a keyboard. `DualDeck-Installer.sh`
was already safe to `curl | bash` (`set -uo pipefail`, no `-e`, deliberately
chosen previously) but this wasn't documented, and its terminal-fallback
`read -rp` prompts (`confirm()`, `choose_action()`) read from plain
stdin -- which is the pipe carrying the script's own source when run via
`curl | bash`, already exhausted by the time bash reaches the prompt.
Fixed by redirecting those reads from `/dev/tty` instead (the standard
fix every curl-pipe installer needs, e.g. rustup does the same), and
added the one-liner to `README.md` as the primary documented install
method, alongside `-s -- --host`/`--client`/etc. to skip the menu
entirely for anyone who'd rather not navigate it with a controller at
all. Downloading and running the script locally still works identically
for anyone who'd rather review it first.

**Found and fixed while testing this**: `read ... < /dev/tty` redirection
itself fails outright (not just returns empty) when there's no
controlling terminal at all -- and referencing a `local` variable that a
failed `read` never assigned is a hard "unbound variable" abort under
this script's `set -u`, not a graceful empty-string fallback. Both
`reply` (`confirm()`) and `choice` (`choose_action()`) are now
pre-initialized (`local reply=""` / `local choice=""`) with `|| true`
after the read, so a missing controlling terminal degrades to the
default branch instead of crashing. Caught by actually testing the
no-tty case, not just reasoning about it.

**2. The "Patch my EmuDeck-installed emulators" launcher menu choice
opened a terminal that closed almost immediately, with no way to read
the output, and no way to pick which emulator(s) to patch.**
`internal/launch-emudeck-integration.sh` (added earlier this same day)
always ran `emudeck-replace-in-place.sh` with no `--emulator` filter and
relaunched it inside a fresh terminal emulator with nothing to keep that
window open once the tool exited -- so a fast exit (e.g. no matching
emulators found, or an early failure) closed the window before there
was anything to read. Fixed:
- Added a `choose_emulators()` step (kdialog `--separate-output
  --checklist`, all three checked by default; numbered terminal fallback
  otherwise) that turns the selection into `--emulator` flags instead of
  always patching everything found.
- Added a "keep this window open when finished" prompt (kdialog
  `--yesno`, defaulting to yes; terminal fallback `[Y/n]`). When yes, the
  tool is run through a small generated wrapper script that captures its
  exit status, prints a pass/fail summary, and pauses on "Press Enter to
  close this window..." regardless of success or failure -- the wrapper
  deliberately does **not** use `set -e` itself, since with it a failing
  tool invocation would skip straight past the pause instead of leaving
  the error visible, which was the entire point of the fix.

Verified in isolation (mocking the tool, `kdialog`, and terminal
emulators, not real emulator builds): the no-tty/no-kdialog default path
(all three emulators, defaults safely instead of crashing), a real pty
walking through both terminal-fallback prompts interactively, and a fake
`kdialog` exercising the checklist + yesno=No path, including confirming
the wrapper's exit status still propagates correctly when `keep_open=0`.

**Not yet verified**: this exact flow on real Bazzite hardware with a
real terminal emulator (konsole) and a real EmuDeck install -- isolation
testing covers the logic, not the real environment.

## 2026-07-31: Steam is now restarted automatically when a shortcut write needs it closed

`steam_shortcut.py` refuses to write `shortcuts.vdf` while Steam is
running and a write is actually needed (it caches the file in memory
and can silently clobber the change on its next save) -- previously
this just told the user to close Steam manually and try again. On a
Steam-Controller-only setup that's a real trap: once Steam is closed,
there's no other way to interact with the desktop to reopen it (real
user report, 2026-07-31, Bazzite HTPC: "I cannot interface with the
steam controller if steam is closed... they require a mouse to reopen
it").

Added `scripts/lib/steam_restart_helper.sh` (bundled into both
`host/internal/` and `client/internal/`, alongside `steam_shortcut.py`
itself), wired into all four of `install-steam-shortcut.sh`/
`uninstall-steam-shortcut.sh` (host and client). On the specific "Steam
appears to be running" refusal, it now: tells the user via a kdialog
passive popup, asks Steam to quit (`steam -shutdown`), waits up to 30s,
retries the write once Steam is confirmed gone, and relaunches Steam --
all from a `setsid`-detached background process, so it survives even if
the calling script is itself a descendant of Steam (e.g. reached via a
Steam shortcut in Gaming Mode) and would otherwise be torn down when
Steam quits. Any *other* steam_shortcut.py failure (corrupt vdf,
permissions) is left completely alone -- real exit code and stderr
propagate to the caller's existing `on_error`/kdialog handling
unchanged.

**Deliberately automatic with no confirmation prompt** -- an explicit
user choice made aware of a real, sourced tradeoff: Bazzite has
documented upstream GitHub issues of Steam hanging on "Shutting down
Steam" when quit from Gaming Mode. If that happens here, the 30s wait
times out, the shortcut write is skipped entirely (not risked against a
Steam that might still be alive and about to clobber it), and Steam is
relaunched anyway so a stuck shutdown doesn't also leave input broken --
verified this exact timeout path in isolation (see below). Set
`DUALDECK_NO_STEAM_AUTORESTART=1` in the environment to disable this
behavior entirely and fall back to the original plain refusal message,
if this ever proves troublesome on a particular machine.

**Bug caught by testing, not shipped**: the first version of
`run_steam_shortcut_with_restart()` captured `steam_shortcut.py`'s exit
code via `if cmd; then ... fi; local exit_code=$?` -- an `if` statement
whose condition is false and has no `else` branch exits 0 *itself*
(POSIX), which silently discarded the real exit code and made *any*
unrelated failure (a genuinely corrupt `shortcuts.vdf`, a permissions
error) get reported back to the caller as success. Only found by
actually testing an unrelated-failure case, not by reasoning about the
code -- fixed with the safe `cmd || exit_code=$?` idiom instead, which
doesn't have this pitfall.

Verified in isolation (mocking `steam`, `pgrep`, and `steam_shortcut.py`
via env-var-parameterized fakes, not a real Steam install): the happy
path (Steam quits, write retried, Steam relaunched, correct args
propagated through the detached subshell), the 30s-timeout path (skips
the retry, still relaunches, logs exactly why), an unrelated failure
passing the real exit code straight through untouched, a retry that
itself fails for a different reason (correct exit code logged, Steam
still relaunched), and the `DUALDECK_NO_STEAM_AUTORESTART=1` escape
hatch. Also ran all four generated `install`/`uninstall-steam-shortcut.sh`
scripts standalone with `--dry-run` against a synthetic `$HOME` to
confirm the sourcing/wiring itself is correct (fails cleanly on "no
Steam userdata directory," as expected outside a real Steam install,
with no syntax or unbound-variable errors reaching that point).

**Not yet verified**: this exact flow against a real, running Steam
client on real Bazzite/SteamOS hardware -- isolation testing covers the
logic against mocked `steam`/`pgrep`, not Steam's actual real-world
shutdown behavior, which is exactly the thing documented elsewhere as
inconsistent on Bazzite.

## 2026-08-01: Host firewall ports are opened automatically during install

Real user report (Bazzite HTPC): the client couldn't find or connect to
the host at all, even launching an emulator directly via the DualDeck
Host GUI (not just through the EmuDeck/SteamRomManager path) -- and the
user explicitly didn't want to run the manual `firewall-cmd` command
`docs/bazzite-host-setup.md`'s Firewall section documents ("the script
should have it built in if possible"). Bazzite/Fedora ship `firewalld`
active by default, which blocks all five of these ports
(`host/remote-server/include/host/net_server.h`'s `NetServerConfig`
defaults) until explicitly opened: 8760/tcp control, 8761/udp input,
8762/tcp video, 8763/udp discovery, 8765/udp audio -- discovery being
blocked alone is enough to fully explain "doesn't show up on the
client at all," and audio (8765) was never even listed in the existing
manual doc until now.

Added `scripts/lib/host_firewall.sh` (`ensure_host_firewall_ports()`,
bundled into `host/internal/` only -- this is a host-side concern, the
client doesn't need it), wired into both
`host/internal/install-steam-shortcut.sh` and
`host/internal/install-host-distrobox.sh` (the latter both as a
sub-step of the former on immutable systems, and standalone for anyone
running it directly -- firewalld runs at the host OS level, not
per-container, so it's the same fix either way; a harmless idempotent
double-call when reached through both). Supports `firewalld`
(Bazzite/Fedora's default) and `ufw` (Debian/Ubuntu-based hosts); no
raw iptables/nftables handling, since neither of this project's
documented host platforms needs it. Deliberately **best-effort and
never fatal to the install**: any failure (no supported firewall
manager, sudo declined, `firewall-cmd`/`ufw` erroring) logs a clear
message pointing at the still-present manual fallback in
`docs/bazzite-host-setup.md` and returns non-zero, but the calling
install script always continues (`ensure_host_firewall_ports || true`)
-- getting the Steam shortcut installed matters more than this one
convenience step succeeding. Skipped entirely in `--dry-run` (same
"zero side effects" guarantee `emudeck-replace-in-place.sh --dry-run`
already has).

Verified in isolation (mocking `firewall-cmd`/`ufw`, not a real
firewall): all five ports/protocols opened correctly via firewalld
(`--permanent` + `--reload`) and via ufw, the "no supported firewall
manager" fallback message, a `firewall-cmd` failure being logged but
not aborting the caller, `--dry-run` never invoking `firewall-cmd` at
all, and a full end-to-end run of the real generated
`install-steam-shortcut.sh` against a synthetic `$HOME` with a fake
`firewall-cmd` -- confirmed the shortcut still installs successfully
even when the fake firewall-cmd is made to fail.

**Not yet verified**: against a real `firewalld`/`ufw` on real
hardware -- isolation testing covers the logic, not whether
`sudo firewall-cmd`/`sudo ufw` actually prompts and succeeds
correctly when this runs without an attached terminal (e.g. re-running
"Add to Steam" from inside Gaming Mode, where `dualdeck-host.sh` calls
`install-steam-shortcut.sh` directly with no terminal-relaunch wrapper,
unlike the EmuDeck integration menu choice) -- this is a pre-existing
risk shared with `ensure_packages()`'s own `sudo apt/dnf/pacman
install` calls, not something newly introduced here, but not yet fixed
either.

## 2026-08-01: melonDS's patched EmuDeck AppImage never actually enabled its remote server

Real user report: Azahar and Cemu, patched via `emudeck-replace-in-place.sh`
on a Fedora laptop, worked correctly end to end, but melonDS on the same
machine never streamed at all. Root cause: `generate_apprun_melonds()`
(`scripts/emudeck-replace-in-place.sh`) generated a trivial passthrough
`AppRun` that never set `MELONDS_REMOTE_ENABLE` -- melonDS only starts
its in-process remote server if that env var is set *or* its own
persisted Settings checkbox ("Enable melonDS Remote") is already
checked, and nobody checks that on a fresh EmuDeck install. Every
*other* melonDS launch path in this codebase (`run-host.sh`,
`launch-custom-emulator.sh`, `install-host-distrobox.sh`'s Distrobox
exec) already exports `MELONDS_REMOTE_ENABLE=1` -- this was the one
launch path that didn't. Fixed by exporting it (and
`MELONDS_REMOTE_VERSION`, matching those other paths) in the generated
`AppRun`, verified by generating one in isolation and inspecting its
contents.

## 2026-08-01: emudeck-replace-in-place.sh now builds inside Distrobox on Bazzite/immutable systems

Real user report: running the EmuDeck integration tool on a Bazzite
HTPC failed immediately at the dependency-check step with
`ensure_packages()`'s existing immutable-system refusal ("Auto-
installing build packages onto an immutable base isn't done unattended
here"). This tool had no Distrobox fallback at all, unlike
`install-host-distrobox.sh` (the *separate*, pre-existing Bazzite
fallback for DualDeck's own non-EmuDeck host install) -- but that one
only installs *runtime* libraries to launch an already-compiled binary,
a fundamentally lighter problem than what this tool needs: a full build
toolchain (cmake, ninja, gcc-c++, the entire Qt6/X11/Wayland `-devel`
set, ~30-40 packages) to actually compile melonDS/Azahar/Cemu from
source.

Design (planned in detail before implementing, given the real risk of
getting a system-level environment change wrong):
- `scripts/lib/ensure-packages.sh` gained `is_immutable_system()`,
  factored out of `ensure_packages()`'s own existing rpm-ostree/
  `/run/ostree-booted` detection (byte-identical behavior for every
  existing caller -- pure refactor, verified by confirming the refusal
  message is still exactly what it was before).
- `scripts/emudeck-replace-in-place.sh` gained
  `run_in_distrobox_build_container()`: on an immutable system (and
  only if `DUALDECK_INSIDE_EMUDECK_BUILD_CONTAINER` isn't already `1`,
  preventing recursion), creates/reuses a **dedicated** Distrobox
  container (`dualdeck-emudeck-build`) and `exec`s the entire script
  again inside it with the original arguments. Deliberately a full
  self-re-exec rather than wrapping individual build steps: `$HOME` is
  bind-mounted into a Distrobox container at the same absolute path by
  default, so the persistent build cache
  (`~/.cache/dualdeck/emudeck-builds/`), the cached `appimagetool`,
  EmuDeck's own `~/Applications/`, and this tool's own log file all
  resolve identically inside and outside the container -- meaning the
  exact same detection/confirm/build/package/install code that already
  works on a regular Linux host now also works correctly, unmodified,
  once re-run inside the container. Once inside, `is_immutable_system()`
  is naturally false (a plain `fedora:latest` image is never immutable),
  so `ensure_packages()` takes its normal `dnf` branch with a real
  toolchain available.
- Deliberately a **new, dedicated** container rather than reusing
  `dualdeck-host` (`install-host-distrobox.sh`'s own container): the
  build toolchain's footprint (~several hundred MB of `-devel`
  packages) has nothing to do with `dualdeck-host`'s runtime-only,
  long-lived launch container, and someone who only ever uses the
  EmuDeck integration tool shouldn't need `dualdeck-host` created at
  all, or vice versa. Treated as disposable/cache-like (no paired
  uninstaller, unlike `dualdeck-host`/`uninstall-host-distrobox.sh`) --
  `distrobox rm dualdeck-emudeck-build --force` reclaims its disk space
  manually if ever needed, matching how the persistent build-binary
  cache also has no dedicated cleanup script today.
- `--dry-run` is completely unaffected -- the whole check lives inside
  the existing `if [[ "${dry_run}" -ne 1 ]]` block, so dry-run never
  even calls `command -v distrobox`, preserving the "--dry-run has zero
  side effects" guarantee exactly as before.

Verified in isolation (mocking `rpm-ostree`/`distrobox`/`dnf`/`sudo`,
not a real container): the container dispatch fires only on a simulated
immutable system and constructs the exact expected `distrobox create`/
`distrobox enter` command line (container name, image, env var guard,
absolute self-path, original arguments forwarded unchanged); the
recursion guard actually prevents a second dispatch when already
"inside" the container; a full re-exec walkthrough with the guard set
and no `rpm-ostree` on `PATH` (simulating genuinely running inside the
container) proceeds normally through the dependency-install step and
completes; `--dry-run` never touches `distrobox` at all even on a
simulated immutable host; and the "distrobox not found" error path
matches `install-host-distrobox.sh`'s existing wording.

**Not yet verified**: against a real Distrobox/podman container on real
Bazzite hardware -- in particular, whether `sudo` is genuinely
passwordless inside a freshly-created Distrobox container (assumed by
analogy to `install-host-distrobox.sh`'s own already-unverified-on-
real-hardware Distrobox launch line, not independently confirmed here),
whether `distrobox enter`'s tty/interactive-prompt passthrough correctly
reaches this tool's own `read -r -p` per-emulator y/N confirmation
prompts, and whether the full melonDS/Azahar/Cemu source builds
(a much bigger compile than the small host/remote-server prototype
`docs/bazzite-host-setup.md`'s Distrobox section was originally written
against) actually succeed inside a freshly-created `fedora:latest`
container at all. `scripts/patch-existing-emulator.sh` has the identical
immutable-system gap and could reuse this same pattern later -- not
done here, out of scope for this fix.

**Update (real Bazzite hardware test, same day)**: container creation
itself worked ("Container Setup Complete!"), but the re-exec into it
failed immediately with `env: error while loading shared libraries:
libGL.so.1: cannot open shared object file: No such file or directory`
(exit 127) -- `env`, the very first thing exec'd inside the container,
never even started. Cause: this tool is launched from a Steam shortcut
(its own intended entry point), and Steam exports `LD_PRELOAD` (its
overlay-injection libs, `gameoverlayrenderer.so`/`libextest.so`) into
the whole process tree. `distrobox enter` forwards the caller's
environment into the container by default; on the host the 64-bit
overlay lib loads fine and the 32-bit entry is silently skipped
(harmless "wrong ELF class" warnings, visible throughout this run's
output), but inside a freshly-created, not-yet-provisioned
`fedora:latest` container the 64-bit overlay lib's own dependency,
`libGL.so.1`, doesn't exist yet (mesa isn't installed until the
`ensure_packages "build"` call further down this same script), so
`ld.so`'s preload failed hard and took `env` down with it. Fixed by
`unset LD_PRELOAD LD_LIBRARY_PATH` in
`run_in_distrobox_build_container()` immediately before the `exec
distrobox enter` call (`LD_LIBRARY_PATH` unset defensively for the same
reason -- Steam also points it at its own bundled runtime). Verified in
isolation by injecting both vars into the test harness and confirming
via a logging fake `distrobox` that they're present for the (harmless)
`distrobox create` call but gone by the time `distrobox enter` runs --
the one call that actually execs something inside the container.

**Same-day followup: the identical bug existed in the actual host
launch path, not just this build tool, and was very likely why Bazzite
never worked as a host at all.** Real user report: after reinstalling
with the firewall auto-open fix (see the entry above), a client still
saw nothing when trying to connect to the Bazzite host -- not
"discovered but refused to connect," just nothing. Diagnostics from the
Bazzite machine (`ss -tulnp`, `firewall-cmd --list-ports` against the
actually-active zone, `ps aux`) showed: no process was listening on any
of `net_server.h`'s five ports at all, firewalld's active zone already
had a much wider port range open (`1025-65535/tcp+udp`, ruling out
firewall as the blocker), and `ps aux` showed only the Distrobox
container's own supervisor processes (`conmon`/`crun`) -- no melonDS
process, running or crashed-and-restarted, anywhere. Root cause: the
exact same unfiltered-`LD_PRELOAD` bug as
`run_in_distrobox_build_container()` above, in a second, independent
`distrobox enter` call site --
`host/internal/install-host-distrobox.sh`'s final launch line (`exec
distrobox enter "${container_name}" -- env MELONDS_REMOTE_ENABLE=1 ...
"${central_install_dir}/melonDS" "$@"`, embedded in
`scripts/build-release.sh`, not a standalone file in the repo). Since
this script's normal entry point *is* a Steam shortcut,
`env`/`sudo`/melonDS never got a chance to start: `env` crashed on the
same missing `libGL.so.1` immediately, silently (Steam's Big Picture
launcher doesn't surface a crashed shortcut's stderr anywhere a user
would see it), so no host process ever existed to open a socket --
which explains "nothing appeared on the client" far better than a
firewall problem would (a firewall issue would still let local `ss`
show the port bound, just unreachable remotely). Fixed the same way:
`unset LD_PRELOAD LD_LIBRARY_PATH` added once, right after this
script's existing `command -v distrobox` check, covering both of its
`distrobox enter` call sites (the `sudo dnf install` runtime-library
step, which likely already tolerated this via `sudo`'s own
`env_reset` policy stripping `LD_PRELOAD` before `dnf` ran, and the
final unguarded `env ... melonDS` launch, which had no such
protection). Verified the same way as the build-tool fix: extracted
the generated `install-host-distrobox.sh` heredoc into an isolated test
harness mirroring its real `host/internal/` layout, injected a fake
Steam-style `LD_PRELOAD`/`LD_LIBRARY_PATH`, and confirmed via a logging
fake `distrobox` that all four calls it makes (`list`, `create`, `enter
... dnf install`, and the final `enter ... env MELONDS_REMOTE_ENABLE=1
... melonDS`) now run with both variables unset. **Not yet verified**
against real Bazzite hardware -- the diagnostics that led here came
from the user's real machine, but this specific fix hasn't been
retested there yet.

## 2026-08-01: All three host patches embed a frozen protocol copy 3 versions stale -- likely the real reason nothing has ever fully connected

Real user report, found while chasing the Bazzite host-connectivity saga
above to its actual conclusion: after every other fix in this file
landed (host launches cleanly, firewall open, correct ports listening,
client reaches it), the client still got rejected with "PROTOCOL
VERSION MISMATCH" -- `HelloRejectReason::ProtocolVersionMismatch`,
reason code 1 -- against a host that had *just* been built from the
same v0.1.83 release the client was on. Both a stale-client and a
stale-host theory were ruled out (full wipe-and-reinstall of the
client, direct terminal launch of the freshly-built host) before the
real cause turned up: `host/melonds-patches/0001-remote-server-
integration.patch` -- and, confirmed by the same grep,
`host/azahar-patches/` and `host/cemu-patches/`'s patches too --
doesn't reference this repo's live `protocol/`, `adapter-sdk/`, and
`host/remote-server/` directories at all. Each patch file *embeds its
own copy* of that entire subsystem as brand-new files added by the
diff itself (`src/frontend/qt_sdl/remote_server/{protocol,adapter_sdk,
host}/...` for melonDS's patch, an analogous vendored tree for the
other two), frozen at whatever state existed whenever that patch was
last regenerated -- `inline constexpr uint16_t kProtocolVersion = 7;`
in all three, vs. `10` in the live header. `scripts/lib/
build_emulator.sh`'s `build_melonds()`/`build_azahar()`/`build_cemu()`
(shared by both the official release pipeline and
`emudeck-replace-in-place.sh`) just `git apply` this patch against a
pinned upstream commit -- there is no step anywhere that keeps the
embedded copy in sync with the live shared library as it evolves, so
every host build from any of the three patches has been three real
protocol bumps behind every client build for however long this drift
has existed:
- v8: `VideoFrame`'s payload changed from raw BGRA8888 to a JPEG-
  compressed image (libjpeg-turbo) -- a new build dependency the
  frozen copy's `CMakeLists.txt` hunk was never given.
- v9: `HelloPayload` gained a client-driven `videoQuality` field.
- v10: `VideoFrame` gained an 8-byte capture timestamp prepended
  before the JPEG bytes, for latency instrumentation.

None of these are a version-number tweak -- an old client/host talking
v7 would either misinterpret new-format bytes as something else
(v10's timestamp corrupting JPEG decode) or simply never compile
against the new dependency (v8's JPEG codec needs libjpeg-turbo
linked, which the frozen `CMakeLists.txt` copy predates). This is very
likely the actual explanation for the entire connectivity saga
documented above, not just the Bazzite-specific pieces -- **any** host
built from any of these three patches, on any machine, has been wire-
incompatible with the current client the whole time, including
whatever earlier point in this same troubleshooting session Azahar/
Cemu on the Fedora laptop were reported as "working": that was very
likely a client that hadn't yet auto-updated past v7 itself, not
genuine version-10 compatibility.

**How the drift happened**: several earlier entries in this file
(the AdapterBridge/adapter-sdk IPC work, GitHub issue #28) explicitly
confirmed via `git diff --stat` that `host/melonds-patches/` was left
untouched as evidence of clean scoping -- true and correct at the
time (`kProtocolVersion` was `6` then), but nobody ever went back to
resync the patch as the live shared library kept moving (6 -> 7 -> 8
-> 9 -> 10) in later, unrelated changes. "The patch is untouched" was
being read as a good sign in each individual change without anyone
tracking that it was simultaneously falling further behind with every
one of those live-library changes elsewhere.

**Fixed for melonDS**: regenerated
`host/melonds-patches/0001-remote-server-integration.patch` by cloning
melonDS at its pinned commit, applying the old patch as a starting
point, replacing the vendored `protocol/`/`adapter_sdk/`/
`host/{net_server,device_approval_manager,adapter_bridge}.*`/
`host/{emulator_input_sink,frame_source,mic_audio_sink}.h` files with
the live top-level content verbatim (byte-for-byte copies, same
relative paths), and adding the `CMakeLists.txt` hunk needed for
libjpeg-turbo (`find_path`/`find_library`/`TurboJPEG::TurboJPEG`
imported target, mirroring the top-level `CMakeLists.txt`'s own
discovery) since v8's JPEG compression was a dependency the frozen v7
copy never needed. One real interface break surfaced by an actual
compile (not caught by inspection alone): `IFrameSource::
getLatestFrame()` gained `outWidth`/`outHeight` params (a real bugfix
that shipped between v7 and v10, for `AzaharAdapter`'s non-DS-sized
frames) that melonDS's `MelonDSFrameSource`/`MelonDSAdapter` glue
still called/implemented with the old 2-arg signature -- fixed by
having `MelonDSFrameSource::getLatestFrame()` echo `kFrameWidth`/
`kFrameHeight` (DS's frame size is fixed, matching how
`SyntheticFrameSource` already handles this per `frame_source.h`'s own
comment) and updating `MelonDSAdapter::latestFrame()`'s call site.

Verified for real, not just "it compiled": a completely clean
from-scratch build (fresh `git clone` of melonDS at the pinned commit,
`git apply` of the regenerated patch, `cmake -S -B` configure,
`cmake --build`) succeeded end to end via `scripts/lib/
build_emulator.sh`'s actual `build_melonds()` function (the same code
path `build-release.sh` and `emudeck-replace-in-place.sh` both use),
and the resulting binary's embedded `protocol.h` copy confirmed at
`kProtocolVersion = 10`, matching the live header, via both a direct
grep of the applied patch and `strings` on the compiled binary showing
the v10-era `net_server.cpp` log lines. **Not yet verified against a
real client on real hardware** -- this was built and compile-verified
in a sandboxed CI-like environment (no real melonDS GUI/GPU available
there), not yet re-tested end-to-end against an actual Steam Deck
client by the user.

**Same-session followup: Azahar and Cemu fixed too, plus a real
regression guard.** Both had the identical bug (same
`kProtocolVersion = 7` embedded copy), but a much smaller footprint
than melonDS's -- neither vendors `net_server.cpp`/
`device_approval_manager.*`/`adapter_bridge.*`/`frame_source.h`/etc. at
all, since Azahar/Cemu run in **out-of-process host-control mode**
(their `AzaharAdapter.cpp`/`CemuAdapter.cpp` talk over `adapter_sdk`'s
Unix-socket IPC to the separate `dualdeck-host-service` binary, which
already always builds fresh from live source -- see the
`ensure_host_service_binary()` fix below). Only `protocol.h`,
`protocol.cpp`, `adapter_contract.h`, and `ipc_protocol.h` differed at
all between their frozen copies and live; verified file-by-file (`diff`
against live) that every change was either byte-identical, comment-only
(both `adapter_contract.h`'s `SurfaceFrame` doc and
`ipc_protocol.h`'s `kMaxIpcFramePixelBytes` rationale, no actual
constants/signatures touched), or purely additive to `protocol.h` (new
`ClientLog` packet type, new `Wire*` capability/display-descriptor
types, no removed or changed declarations) -- confirmed neither
`AzaharAdapter.cpp` nor `CemuAdapter.cpp` reference `frame_source.h`/
`IFrameSource`/`getLatestFrame()` at all (the one interface break that
*did* require a real code fix for melonDS), so this specific class of
break doesn't apply to either. Regenerated the same way (clone at
pinned commit, apply old patch, swap in live files, regenerate,
`git apply --check` against a fresh clone) but **without** a full
from-scratch compile given Azahar/Cemu's build cost (36 submodules /
~108 vcpkg packages respectively, tens of minutes to hours) weighed
against how low-risk the actual diff was -- the release pipeline's own
CI build of both from source is the real final check this specific
change still needs.

Added `scripts/check-patch-protocol-sync.sh` (new CI job,
`patch-protocol-sync` in `.github/workflows/ci.yml`) as the "some
mechanism" this entry called for: greps `kProtocolVersion` out of the
live header and each of the three patches, fails loudly on any
mismatch. Deliberately a narrow tripwire, not a full sync guarantee --
it only catches a bumped `kProtocolVersion` that never got propagated
(exactly what happened here), not a content change to `protocol.h` or
any of the other vendored files that doesn't come with a version bump.

**Second real bug found on real Bazzite hardware, same day**: with the
above fixes actually live (confirmed via a from-scratch Azahar rebuild
recompiling `remote_server/adapter_sdk/src/protocol.cpp` etc., and
`dualdeck-host-service` linking successfully -- both working exactly as
intended), the next step failed: packing the rebuilt Azahar into a new
AppImage via `appimagetool` errored with `file command is missing but
required, please install it`. `scripts/lib/appimage_pack.sh`'s
`ensure_appimagetool()` downloads the `appimagetool` binary itself (not
distro-packaged) but never accounted for its runtime dependency on the
`file`/libmagic command, which isn't part of a minimal Fedora Distrobox
container. Fixed by adding `file` to all three (`apt`/`dnf`/`pacman`)
package lists in `emudeck-replace-in-place.sh`'s `ensure_packages
"build"` call -- the generic list, not an azahar/cemu-specific one,
since AppImage repacking applies to any emulator this tool handles, not
just those two. `build-release.sh` (the official release pipeline) has
its own, separate `ensure_packages "build"` call but never invokes
`appimagetool` itself (its output is a plain tarball, not AppImages),
so it isn't affected by this gap.

**Third real bug found on real Bazzite hardware, same day**: with the
`file` dependency fixed, Azahar's AppImage repack itself fully
succeeded end to end (`appimagetool` ran clean, `azahar: installed
DualDeck's patched build at ...azahar.AppImage` printed) -- but the
script then crashed immediately after with `appdir: unbound variable`
at `replace_in_place_one`'s own definition line, right as that function
returned. Root cause: `scripts/lib/appimage_pack.sh`'s `pack_appimage()`
sets `trap 'rm -rf "${appdir}"' RETURN` to clean up its temp AppDir --
but a `trap ... RETURN` set inside a function in bash is **not**
scoped to that function's own return; it's global shell state that
fires on the return of the *next* function call anywhere in the script,
by which point `appdir` (a `local` inside `pack_appimage`) is out of
scope entirely, crashing under `set -u` on whatever unrelated function
happens to return next -- in this case, `pack_appimage`'s own caller,
immediately after a fully successful repack. Reproduced in isolation
with a minimal two-function script before touching the real file, to
confirm the theory rather than guess: `inner()` sets the trap and
returns cleanly, but `outer()`'s very next return crashes with the
identical `appdir: unbound variable` message. Fixed by making the trap
self-clearing (`trap 'rm -rf "${appdir}"; trap - RETURN' RETURN`) so it
only ever fires once, for `pack_appimage`'s own return -- re-verified
against the same repro (now completes cleanly across two simulated
emulator iterations) since this bug is generic to `pack_appimage()`
itself, not azahar-specific, and would eventually have hit melonDS/cemu
through this same code path too (melonDS's earlier successful test in
this file's own history went through `install-host-distrobox.sh`
instead, a different code path that never calls `pack_appimage()` at
all, so it never happened to exercise this bug).

**Fourth issue, same day, Cemu specifically**: attempting Cemu next
(after Azahar's full success above), its vcpkg-based dependency build
failed partway through on `openssl:x64-linux` -- `./Configure` exits 2
with a preceding warning vcpkg's own `openssl` portfile prints
unprompted: "openssl requires Linux kernel headers from the system
package manager" (with install hints for Alpine/Ubuntu, but not
Fedora). A minimal `fedora:latest` Distrobox image doesn't ship
`kernel-headers` by default the way a typical Debian/Ubuntu install
usually already has `linux-libc-dev` pulled in transitively. Added
`kernel-headers` (Fedora) / `linux-libc-dev` (Debian/Ubuntu) /
`linux-headers` (Arch) to the existing `"cemu build"` extra
`ensure_packages` call (Cemu-specific, not the generic list, since
neither melonDS nor Azahar touch vcpkg/openssl at all). **Not yet
verified against a real from-scratch Cemu build** -- Cemu's ~108-package
vcpkg dependency graph takes well over an hour to build from scratch,
long enough that re-running it just to confirm this one specific fix
wasn't done as part of this same investigation; next real Cemu attempt
on real hardware is the actual verification this still needs.

**Fifth issue, same day, and the most fundamental one yet**: with the
build/repack fixes above, Azahar's patched AppImage *installed*
successfully for the first time -- but then failed to launch at all,
manually or via Steam. Real terminal output from `~/Applications/
azahar.AppImage`: `dualdeck-host-service: error while loading shared
libraries: libturbojpeg.so.0: cannot open shared object file`, then
`azahar: /lib64/libm.so.6: version 'GLIBC_2.43' not found`, `libstdc+
+.so.6: version 'GLIBCXX_3.4.35' not found`, and `libQt6Core.so.6:
version 'Qt_6.11' not found`. Root cause: these binaries are compiled
inside the Distrobox build container (`fedora:latest`, deliberately
always current), but the resulting AppImage is launched directly on
the **host** system afterward -- that's the entire point of "replace
in place," EmuDeck's own Steam shortcuts exec the `.AppImage` file
unmodified, with no idea it needs to run inside any container. A real
AppImage is supposed to be self-contained precisely so build/run
environment differences like this can't matter; `pack_appimage()`
(`scripts/lib/appimage_pack.sh`) was copying only the raw binary and
relying entirely on whatever the launching host happens to already
have -- fine as long as build and run environments matched closely
enough, which stopped being true the moment Azahar's repack first
fully succeeded and got tested against the actual host. This was
always going to affect all three emulators equally (nothing Azahar-
specific about it), it just took this long in the investigation for
any of them to get past every earlier blocker and actually reach a
real launch attempt.

Fixed with the standard portable-AppImage technique: a new
`bundle_library_dependencies()` in `appimage_pack.sh` runs `ldd`
(which already resolves the *full transitive* closure, not just direct
dependencies -- one pass is enough) against the main binary and every
extra bundled binary (`dualdeck-host-service` for Azahar/Cemu), copying
every resolved `.so` file into `AppDir/usr/lib/`. Skips exactly two
non-file cases: the kernel-provided `linux-vdso.so.1` (ldd prints an
address for it but there's no real file), and the dynamic linker/
loader itself (`ld-linux-x86-64.so.2`, invoked by the kernel directly
via the binary's `PT_INTERP` before any `LD_LIBRARY_PATH` the AppRun
sets could matter -- properly fixing that too would mean explicitly
re-invoking a bundled loader with `--library-path`, not attempted
here). Deliberately bundles glibc-family libraries too
(`libc.so.6`/`libm.so.6`/etc.), unusual for a hand-rolled packaging
script but exactly what the observed `GLIBC_2.43` mismatch needs --
standard practice for AppImages built on a newer-than-target base.
Both `generate_apprun_melonds()` and `generate_apprun_out_of_process()`
in `emudeck-replace-in-place.sh` now export `LD_LIBRARY_PATH` to
prefer `${HERE}/usr/lib` before doing anything else, so ld.so picks up
the bundled copies ahead of whatever the host does or doesn't have.

Verified for real, not just by inspection: compiled a small test binary
linked against `libz`, ran it through the actual `pack_appimage()`
function end to end (dependency bundling, `appimagetool` packaging, the
works), confirmed the resulting `.AppImage` actually runs and its
extracted `AppDir/usr/lib/` contains the bundled `libc.so.6`/
`libz.so.1` with `AppRun` correctly setting `LD_LIBRARY_PATH` to find
them. Also separately verified `bundle_library_dependencies()` against
`curl` (a real binary with a much larger, 30-library transitive
dependency tree spanning glibc, OpenSSL, Kerberos, LDAP, etc.) to
confirm the transitive-closure assumption holds and the ldd-output
parsing handles a busy, realistic case correctly. **Not yet verified
against the actual Azahar/melonDS/Cemu AppImages on real Bazzite
hardware** -- next real attempt on real hardware is the true
end-to-end confirmation this specific fix still needs.

**Cemu's openssl failure (fourth issue above) recurred after the
kernel-headers fix, still unexplained**: same exact vcpkg failure,
same generic "openssl requires Linux kernel headers" message, even
though `ensure_packages` now reports the whole `"cemu build"` list
(kernel-headers included) as already satisfied -- so either that
message is a stock warning vcpkg's openssl portfile prints
unconditionally on Linux regardless of whether headers are actually
present (plausible: `enable-capieng`, a Windows-only CryptoAPI engine
flag, showing up in the `./Configure` invocation for a `linux-x86_64`
target looks like it could be a triplet-config quirk unrelated to
kernel headers at all, though this is speculation, not confirmed), or
kernel-headers alone isn't sufficient. The one piece of information
that would actually explain this --
`.../buildtrees/openssl/config-x64-linux-dbg-err.log`, referenced by
path in the CMake error but never dumped to the console -- was
unreachable: `emudeck-replace-in-place.sh`'s `work_dir` was deleted
unconditionally on exit via its `EXIT` trap, success or failure alike,
so by the time anyone could go look, both the failure and the one file
that would explain it were already gone. Fixed the trap to preserve
`work_dir` (and print its path) specifically on failure, still cleaned
up normally on success -- verified in isolation (a minimal trap-only
repro, both the success-cleans-up and failure-preserves-and-reports
paths). This doesn't fix the openssl build itself -- it's a
prerequisite for being able to *see* the real error on the next
attempt, which is what's actually needed to diagnose this properly
instead of guessing at more package names.

**Sixth issue: the preserved-work-dir fix worked, and the real error
turned out to be the LD_PRELOAD leak again, in a third place, on a
third kind of machine.** Real user report, a genuinely different
machine this time: a native (non-immutable) Fedora 43 install, never
touching Distrobox at all -- confirmed by the log jumping straight to
`== Checking build dependencies ==` with no `rpm-ostree`/Distrobox
messages first. The preserved-work-dir fix worked exactly as intended
and surfaced the real error: `baseline.json:1:1: error: Unexpected
character; expected value`, with the "value" in question being
literally `ERROR: ld.so: object '.../gameoverlayrenderer.so' ...
ignored.` -- Steam's own LD_PRELOAD warning text, prepended to what
should have been pure JSON. Cemu's vcpkg dependency install runs `git
show <rev>:baseline.json` as a child process to read a version-pinning
file; git's own dynamic loader inherits the same Steam LD_PRELOAD this
whole script's environment has had all along (since it's normally
launched from a Steam shortcut), prints its own ld.so warning, and
vcpkg's captured output ends up with that warning text mixed into what
it expected to be clean JSON -- failing the whole build over something
that has nothing to do with Cemu, vcpkg, or anything this script does
on purpose.

The two earlier LD_PRELOAD fixes (`run_in_distrobox_build_container()`
here, and `install-host-distrobox.sh` separately) were both scoped to
one specific `distrobox enter` call site each -- correct for what they
were fixing at the time, but this third instance proves the actual
problem is broader: *any* child process anywhere in this script's
execution, on *any* machine (immutable or not, Distrobox or not), can
have Steam's LD_PRELOAD leak into it and corrupt its output or crash
it outright depending on what that specific tool does with a stray
ld.so warning line. Fixed properly this time: `unset LD_PRELOAD
LD_LIBRARY_PATH` moved to the very first thing this script does --
before argument parsing, before `is_immutable_system()` is ever
checked, before literally any child process (native build tooling, or
a re-exec into Distrobox) can inherit either variable. The now-
redundant `unset` inside `run_in_distrobox_build_container()` was
removed (its own comment updated to point at the top-level one instead
of repeating it) rather than left as silent, confusing duplication.

Verified for real: ran the script (via `source`, dry-run-style) with a
fake `LD_PRELOAD` injected, confirmed the only "wrong ELF class"
warnings that appear are from bash's own process startup itself
(unavoidable -- the dynamic loader reads `LD_PRELOAD` before even the
first line of any script can run), and that zero such warnings appear
from any subprocess the script itself spawns afterward (package-
manager calls, etc.) -- confirming the leak is genuinely closed for
every child process this script creates, not just the one call site
each previous fix addressed. **Not yet verified against a real Cemu
vcpkg build reaching past this exact point on real hardware** -- next
attempt is the actual end-to-end confirmation.

## 2026-08-01: emudeck-replace-in-place.sh no longer builds anything locally -- it downloads prebuilt, patched AppImages from the release instead

Direct follow-up to the entries immediately above (Distrobox build path,
melonDS's missing `MELONDS_REMOTE_ENABLE`, the frozen-protocol-version
bug, the preserved-work-dir fix, and three separate real-hardware
`LD_PRELOAD` leaks). Every one of those was a symptom of the same root
cause: `emudeck-replace-in-place.sh` cloned and compiled melonDS/Azahar/
Cemu from source on whatever machine happened to run it -- a genuinely
uncontrolled build environment (immutable-vs-not, Distrobox-or-not, 30+
system packages, vcpkg for Cemu, whatever Steam happened to have injected
into the environment that session). Real users kept hitting a long tail
of build-environment failures that had nothing to do with DualDeck's own
patch content, each one costing a multi-minute-to-multi-hour build cycle
to even reproduce.

Prompted by the user's own question mid-session: "are we compiling on top
of the existing builds? or are we compiling new ones and swapping out the
executables? I want to simplify this process if we can, instead of making
this more complicated than it needs to be." The answer: CI (`release.yml`
via `build-release.sh`) already builds all three emulators from source on
every release, in a clean, controlled, reproducible environment. There is
no reason a second, worse copy of that same build should also happen on
every individual user's machine just to apply the exact same patch.

**What changed:**

- `build-release.sh` now packages the patched melonDS/Azahar/Cemu binaries
  it already builds into three additional release assets --
  `dualdeck-melonds-patched-linux-x86_64.AppImage`,
  `dualdeck-azahar-patched-linux-x86_64.AppImage`,
  `dualdeck-cemu-patched-linux-x86_64.AppImage` -- using the same
  `pack_appimage()`/`bundle_library_dependencies()` machinery
  (`scripts/lib/appimage_pack.sh`) this tool always used, just run once in
  CI instead of once per user. Their `AppRun` generators
  (`generate_apprun_melonds`, `generate_apprun_out_of_process`) moved out
  of `emudeck-replace-in-place.sh` into a new `scripts/lib/
  apprun_templates.sh`, since only `build-release.sh` calls them now.
  `release.yml`'s asset glob picked up `release-out/*.AppImage` to publish
  them. All three assets get `SHA256SUMS` entries alongside the existing
  archives.
- Caught during this packaging work, before it could reach a real build:
  Cemu's own build output is named `Cemu_release`, not `cemu`, but
  `pack_appimage()` names the AppDir file after the binary's basename and
  the generated `AppRun` execs `usr/bin/cemu` literally -- staged into a
  renamed copy first, exactly the same fix the old per-user build path
  already carried for the same reason (its own comment called this "load-
  bearing"). Verified end-to-end with fake binaries (real ELF files linked
  against `libz` so `ldd`-based bundling has something real to walk)
  before trusting the design abstractly -- all three fake AppImages built
  and ran correctly via `--appimage-extract-and-run`.
- `emudeck-replace-in-place.sh` was rewritten to drop entirely: sourcing
  `pinned_commits.sh`/`ensure-packages.sh`/`build_emulator.sh`/
  `build_cache.sh`/`appimage_pack.sh`; `is_immutable_system()`/
  `run_in_distrobox_build_container()` and the Distrobox build container;
  the 30+-package `ensure_packages "build"`/`"azahar build"`/
  `"cemu build"` calls; `ensure_host_service_binary()`'s on-demand `cmake`
  configure; and the persistent `~/.cache/dualdeck/emudeck-builds/` build
  cache (moot once there's nothing to cache a build of). In their place:
  `download_patched_appimage()`, which downloads
  `dualdeck-<emulator>-patched-linux-x86_64.AppImage` from
  `https://github.com/Crimson3076/DualDeck/releases/latest/download/...`
  and verifies it against a downloaded `SHA256SUMS` before installing
  anything -- the exact same download-then-verify convention
  `DualDeck-Installer.sh` already established for the client/host
  archives (`curl -fsSL`, `sha256sum -c --ignore-missing`), including its
  `DUALDECK_INSTALLER_DOWNLOAD_BASE`-style test override
  (`DUALDECK_REPLACE_DOWNLOAD_BASE` here). Everything else -- EmuDeck
  AppImage detection (`scripts/lib/emudeck_paths.sh`), the backup-then-
  install/manifest/drift-detection logic (`scripts/lib/
  appimage_manifest.py`), the confirmation prompt, `--dry-run`, `--yes`,
  logging to `~/.config/dualdeck/emudeck-replace.log` -- is unchanged.
  `scripts/emudeck-check-drift.sh` needed zero changes: it only calls the
  manifest checker and delegates re-patching to
  `emudeck-replace-in-place.sh --fix`, so it re-downloads instead of
  rebuilding automatically.
- A real bug caught by end-to-end testing against a local fixture HTTP
  server (fake `~/Applications/*.AppImage` files, a fake release-assets
  directory served over `python3 -m http.server`, `DUALDECK_REPLACE_
  DOWNLOAD_BASE` pointed at it): `download_patched_appimage()`'s own
  progress messages (`echo "== ${emulator}: downloading ..."`) were
  written to stdout, but the function is called as `path="$(download_
  patched_appimage ...)"` -- command substitution captures *all* of a
  function's stdout, so the "returned path" was actually the entire
  multi-line progress log with the real path stuck on the end, and the
  subsequent `cp "${downloaded_appimage}" ...` failed with "No such file
  or directory" against that garbled multi-line string. Fixed by sending
  every progress/status line in `download_patched_appimage()` and
  `ensure_sha256sums_downloaded()` to stderr, leaving stdout carrying only
  the final path -- the same stdout/stderr discipline every other
  path-returning function in this codebase already follows, just missed
  here on the first pass.
- `build-release.sh`'s "Bundled EmuDeck integration tool"
  (`host/emudeck-integration/`) packaging step shrank to match: it no
  longer bundles the build-toolchain library files, the three emulator
  patch files, or a full copy of `CMakeLists.txt`/`protocol`/
  `adapter-sdk`/`host/remote-server` (previously needed so `cmake` could
  configure `dualdeck-host-service` on demand from inside the packaged
  copy) -- only `scripts/lib/emudeck_paths.sh` and `scripts/lib/
  appimage_manifest.py` are still needed. Its bundled `README.md` and the
  `dualdeck-host.sh` launcher wrapper's user-facing text were both updated
  from "long source build" language to "downloads prebuilt AppImages."

**Verified:** syntax-checked; full end-to-end run against a local fixture
HTTP server covering first-time install (backup created, manifest
written, correct content installed), re-install when already
`matches_patched` (no redundant backup churn), drift re-patch when the
installed file was replaced by something else (backup preserved from the
*original* install, not the drifted file), checksum-mismatch refusal
(tampered asset correctly rejected, original file left untouched), `--
dry-run` (zero side effects), no-EmuDeck-installed skip messages for all
three emulators, `-h`/`--help`, and a rejected `--emulator` value.

**Real-hardware confirmation (2026-08-01, same day, Fedora "Crimson"
machine):** ran against the actual `v0.1.92` GitHub release this change
published -- Cemu detected at `~/Applications/Cemu.AppImage`, original
backed up, `dualdeck-cemu-patched-linux-x86_64.AppImage` downloaded from
the real release and checksum-verified against the real `SHA256SUMS`
asset, installed, manifest written. The only console noise (`ld.so:
... wrong ELF class`, `pid ... skipping destruction (fork without
exec?)`) is Steam's own overlay-injection libraries reacting to process
startup/forking, present before this script's first line ever runs and
unrelated to it -- not a regression. This closes the "not yet verified
against a real GitHub Releases download / real EmuDeck hardware" gap
above for Cemu specifically; melonDS and Azahar still want their own
confirmation runs, and Cemu's installed AppImage still needs an actual
in-game launch to confirm the remote server itself works, not just that
installation succeeded.

## 2026-08-01: Cemu never launched at all after replace-in-place -- resources/gameProfiles were silently dropped from every packaged copy

Direct follow-up to the entry immediately above (the prebuilt-AppImage
rearchitecture) and its own real-hardware confirmation entry: the user
installed the new `v0.1.92` Cemu AppImage successfully (that part worked,
per the confirmation above), then reported Cemu "now does not launch in
any capacity, not via steam or any other shortcut."

**Root cause, found by downloading the actual published AppImage and
testing it directly** (extracted it, ran `usr/bin/cemu` by hand under
Xvfb since this sandbox has no real display/GPU): the process started,
stayed alive, opened a socket, sat in a normal event-loop poll -- but
never produced a window or any output at all, indefinitely. Checked
Cemu's own upstream `src/CMakeLists.txt` directly (`cemu-project/Cemu`
tag `v2.6`) rather than guessing: it ships static `resources/` and
`gameProfiles/` directories committed in the source tree under `bin/`,
alongside where `Cemu_release` itself gets built. The macOS build step
explicitly copies both (`file(COPY ...)`/`add_custom_command` from
`${CMAKE_SOURCE_DIR}/bin/{gameProfiles,resources}`) into the `.app`
bundle's `SharedSupport/` -- *because* macOS relocates the binary away
from that directory. Linux never needed an equivalent copy step, for the
opposite reason: `Cemu_release` already lands directly inside that same
`bin/` directory, so `resources/`/`gameProfiles/` are already sitting
right next to it with zero extra work, as long as nobody moves the
binary somewhere else afterward.

Every place this project ever packaged Cemu for distribution did exactly
that -- moved the binary somewhere else afterward, without its two
sibling directories: `pack_appimage()` (`scripts/lib/appimage_pack.sh`)
copied only the raw binary plus its `ldd`-resolved shared library
dependencies into `AppDir/usr/bin/`; `build-release.sh`'s plain
`host/cemu` copy (used by `host/internal/run-host-cemu.sh`, the non-
EmuDeck DualDeck-managed launch path) did the same. Cemu can't finish
initializing its GUI without `resources/` (translations, icons/theming
assets for its GTK-backed UI) -- it doesn't crash or print an error, it
just never gets far enough to show a window, which is consistent with
both the sandbox repro above and the user's "does not launch in any
capacity, no error" report. This bug predates this session's prebuilt-
AppImage change -- it existed in the exact same `pack_appimage()` the
old locally-compiled `emudeck-replace-in-place.sh` already used -- but
was never actually caught before because nobody had launched a packaged
copy of Cemu end to end until this real-hardware test; every earlier
"Cemu builds and runs" verification (2026-07-22 entries) ran the freshly
built `${cemu_src}/bin/Cemu_release` in place, where its sibling
directories were still naturally present.

**Fix:**

- `pack_appimage()` gained a new optional `extra_dirs` parameter
  (colon-separated directory paths, each copied into `AppDir/usr/bin/`
  under its own basename -- the same directory the main binary and
  `extra_binaries` already land in, matching where Cemu expects to find
  them relative to its own executable).
- `build-release.sh`'s Cemu AppImage packaging call now passes
  `${cemu_bin_dir}/resources:${cemu_bin_dir}/gameProfiles` (derived from
  `dirname("${cemu_bin}")`, i.e. `${cemu_src}/bin` -- no changes needed
  to `build_emulator.sh` itself, since that directory already has both
  as build-time siblings).
- The plain `host/cemu` copy (the non-AppImage, DualDeck-managed launch
  path `run-host-cemu.sh` uses) got the identical fix: both directories
  are now also copied to `host/resources`/`host/gameProfiles`, siblings
  of `host/cemu`, from the same source. melonDS and Azahar need no
  equivalent directories -- their own upstream builds don't ship any.

**Verified:** `pack_appimage()`'s new `extra_dirs` bundling tested
end-to-end with a fake binary (real ELF linked against `libz`, matching
this project's existing fake-binary AppImage test convention) plus fake
`resources/foo/bar.txt` and `gameProfiles/profile.ini` fixtures -- the
built AppImage's `AppRun` confirmed both landed at
`usr/bin/resources/foo/bar.txt` and `usr/bin/gameProfiles/profile.ini`
exactly as expected, then successfully exec'd the fake binary.

**Not yet verified:** against real Cemu itself on real hardware -- this
fix hasn't shipped in a release yet, and the actual GUI-initialization
failure mode (missing resources causing no window, no error) was
diagnosed by reading Cemu's own CMakeLists.txt and reasoning from a
partial sandbox repro (no real GPU/desktop available here), not by
confirming a real Cemu instance actually shows its main window once
`resources/`/`gameProfiles/` are present. The next real-hardware
replace-in-place run against the release this fix ships in is the actual
confirmation still needed.

## 2026-08-01: Fixed resources/gameProfiles, Cemu still crashed on launch -- bundling glibc itself broke `mkdir`

Direct follow-up to the entry immediately above. The user re-ran the
patched `v0.1.93` Cemu AppImage via EmuDeck's own `cemu.sh` launcher
(the real launch path, not a manual terminal test) and got a new, much
earlier failure:

```
DualDeck: no persistent host service running yet -- starting a private one for this session
mkdir: symbol lookup error: /tmp/.mount_Cemu.ADdJHpA/usr/lib/libc.so.6: undefined symbol: __nptl_change_stack_perm, version GLIBC_PRIVATE
```

Note what's crashing: `mkdir`, a completely unrelated system utility the
AppRun script calls to create its run directory, not Cemu itself --
before Cemu was ever even reached.

**Root cause:** `bundle_library_dependencies()` (`scripts/lib/
appimage_pack.sh`) bundles *every* `ldd`-reported dependency into the
AppImage, glibc included -- a deliberate earlier decision (see that
function's own header comment), made to fix a *different* problem: a
Distrobox build container having *newer* glibc/Qt symbols than a bare
host. That reasoning doesn't hold for glibc specifically. glibc's own
internal ABI between `ld.so` (the dynamic linker/interpreter -- always
the *host's* copy; `PT_INTERP` is a fixed absolute path, entirely
unaffected by `LD_LIBRARY_PATH`) and `libc.so.6` relies on private,
version-pinned symbols (`GLIBC_PRIVATE`, e.g.
`__nptl_change_stack_perm`) that are only guaranteed to match *within
one specific glibc build* -- never across two independently built ones.
Pairing the host's own `ld.so` with a *different* bundled `libc.so.6`
isn't "an older or newer libc," it's an ABI mismatch, and every AppRun
template exported `LD_LIBRARY_PATH` globally for the *entire script*,
not just the emulator/host-service invocations -- so *any* plain system
command the script called afterward (here, `mkdir -p`) inherited the
bundled, mismatched `libc.so.6` and crashed on a private symbol the
host's own `ld.so` doesn't provide the same way.

**Fix, two independent layers:**

1. `bundle_library_dependencies()` now excludes glibc's own component
   libraries from bundling outright (`libc.so.*`, `libm.so.*`,
   `libpthread.so.*`, `libdl.so.*`, `librt.so.*`, `libresolv.so.*`,
   `libnsl.so.*`, `libutil.so.*`, `libnss_*.so.*`) via a new
   `_is_never_bundle_library()` blacklist -- standard practice for
   portable Linux apps (linuxdeploy and similar tools maintain the same
   list for the same reason): these must always come from the host's
   own matched `ld.so`, never from `LD_LIBRARY_PATH`. This reintroduces
   a narrower version of the original too-old-host-glibc risk in
   theory, but CI's build environment (GitHub's Ubuntu runner) is
   realistically no newer than the rolling-release Fedora-based hosts
   (Bazzite, SteamOS) this project actually targets -- unlike the old
   Distrobox-container scenario this bundling was first written for --
   and a working host `mkdir` is a harder requirement than a
   hypothetical newer-glibc symbol.
2. `scripts/lib/apprun_templates.sh`'s both generators (melonDS's and
   the out-of-process one Azahar/Cemu share) no longer `export
   LD_LIBRARY_PATH` for the whole script -- it's kept as a plain,
   unexported variable and passed only via `env LD_LIBRARY_PATH=...` on
   the specific commands that actually need the bundled libraries
   (`dualdeck-host-service`, and the final emulator `exec`). Every other
   command each AppRun runs (the `python3` socket probe, `mkdir`, `rm`)
   now always sees the host's own, completely unmodified environment.
   Defense in depth on top of (1): even if some *other* bundled library
   ever collides with a system tool's own dependency in the future, only
   the two commands that need bundling are exposed to it.

**Verified:** `bundle_library_dependencies()` tested directly against a
real fake binary linked against `libz` -- confirmed `libz.so.1` still
gets bundled (a real, legitimate dependency) while `libc.so.6` (present
in the same `ldd` output) is correctly excluded. Both generated AppRun
scripts confirmed to no longer `export LD_LIBRARY_PATH` at all (`grep`).
Full pack-and-run test with fake `cemu`/`dualdeck-host-service` binaries
(both linked against `libz`) reproduced the exact failing shape of the
real AppRun template end to end -- the `mkdir -p` call that used to crash
now succeeds silently, the fake host-service spawns in the background
without error, and the final `exec` still correctly finds its bundled
`libz` dependency.

**Not yet verified:** against the real Cemu binary on real hardware --
same caveat as the entry above, this hasn't shipped in a release yet.

## 2026-08-01: Cemu's window title now says "DualDeck"

User request, right after the previous two Cemu fixes finally got it
launching via `emudeck-replace-in-place.sh`: since the replaced
`Cemu.AppImage` sits at the exact same path EmuDeck's own Steam shortcut
already points to, there was no visible way to tell whether the
currently-installed copy was DualDeck's patched build or the original
stock one it replaced -- both look and launch identically from Steam.

Added two small hunks to `host/cemu-patches/0001-remote-server-
integration.patch` (found by reading Cemu's real `v2.6` source directly,
not guessed): `guiWrapper.cpp`'s `gui_updateWindowTitles()` (the
continuously-updated title covering idle/loading/in-game states, all of
which build on the same starting string) and `MainWindow.cpp`'s
`GetInitialWindowTitle()` (the title before that function's first run).
Both now append `" - DualDeck"` to the existing version string instead
of replacing it, so e.g. `Cemu 2.6` becomes `Cemu 2.6 - DualDeck` --
purely cosmetic, no functional change, and deliberately scoped to just
the two window-title call sites rather than the shared version-string
macro itself (which is also used for HTTP User-Agent headers, a startup
log line, a Windows crash-dump header, and Discord Rich Presence -- none
of those needed touching, and touching the macro instead of the two call
sites would have changed all of them by accident). See
`host/cemu-patches/README.md`'s matching entry for the full source-level
detail.

**Verified:** both hunks apply cleanly via `git apply --check` against a
fresh, throwaway shallow clone of the real `cemu-project/Cemu` `v2.6`
tag (not just re-applying on top of the existing committed patch) --
this caught and fixed a genuine hunk-header line-count bug (`+58,10`
should have been `+58,11`) before it could break CI. **Not yet
verified** inside an actual running Cemu window -- needs a real build
and a real launch to confirm the title reads as expected once this
ships.

## 2026-08-01: Client hangs on "connecting to host" and can't exit -- two compounding bugs

Real user report, right after all three emulators finally launched
successfully via Steam/EmuDeck for the first time (the resources/
gameProfiles, glibc-bundling, and window-title fixes above): "the
client detects the host, and hangs on connecting to host, and now the
client hangs when trying to change host or exit, I need to manually
exit via steam."

Two independent, compounding bugs, found by reading the actual
connect/exit code paths rather than guessing:

**Bug 1 -- `NetClient::connect()` (`client/src/net_client.cpp`) could
block forever, and `disconnect()` couldn't interrupt it.** The raw
`::connect()` calls (control and video sockets) and the `recvExact()`
calls reading Hello/HelloAck had no timeout at all -- a host that's
reachable at the IP level but never responds (exactly what a firewalled
port with a silent DROP rule looks like) could block for the OS's own
TCP retry timeout, sometimes well over a minute, or indefinitely if the
raw TCP connect itself succeeds but nothing ever replies. Worse:
`connect()` holds `connectMutex_` for its *entire* body, and
`disconnect()` needs that same mutex before it can even reach the
`shutdown()` call that would otherwise unblock a stuck `recv()`.
`client/src/main.cpp`'s exit/change-host path does `shuttingDown = true;
reconnectThread.join(); net.disconnect();` -- if `reconnectThread` was
stuck inside a blocking `connect()`/`recvExact()` call, `shuttingDown`
was never checked there, so `reconnectThread.join()` blocked until that
syscall returned on its own (which, per the above, might be never) --
freezing the whole app with no way out but killing it externally,
exactly the reported symptom.

Fixed with a timeout bounded to the handshake specifically, not the
persistent per-session receive loops (which legitimately rely on
blocking-forever `recv()` for an otherwise-idle-but-healthy connection,
e.g. HostControl mode with no video frames): `connectWithTimeout()` (the
standard non-blocking-connect-then-`poll()` pattern, since
`SO_SNDTIMEO`/`SO_RCVTIMEO` have no effect on the initial `connect()`
syscall itself on Linux) bounds both TCP connects to
`kHandshakeTimeoutMs` (5s); `SO_RCVTIMEO` is set on the control socket
around the Hello/HelloAck `recvExact()` calls and explicitly cleared
back to "block forever" the moment the handshake actually succeeds, right
before `controlReceiveLoop()`/`videoReceiveLoop()` start relying on that
blocking-forever behavior. Worst-case exit delay is now bounded to
roughly `kHandshakeTimeoutMs` (a connect attempt in flight right when
the user hits exit), not indefinite.

**Verified:** all 9 pre-existing real end-to-end `NetClient` tests
(`client/tests/test_net_client.cpp`, a real `NetClient` against a real
`NetServer` over real loopback sockets) still pass unmodified -- the
fix doesn't change behavior for any working connection. Added a new
10th test, `net_client_connect_times_out_when_host_never_replies`: a
real TCP listener that accepts the connection (so the raw connect()
succeeds, isolating the `recvExact()`-timeout half of the fix
specifically) but never reads or replies, asserting `connect()` now
returns `false` within 7 seconds (a generous margin over the 5s bound)
instead of never returning at all. Full suite (10/10) runs in ~5.7s
wall-clock, confirming the new test's own timeout is what bounds it, not
a hang.

**Bug 2 -- the new AppRun-spawned ephemeral host-service never opened
this host's firewall.** `install-steam-shortcut.sh`/
`install-host-distrobox.sh` both call `scripts/lib/host_firewall.sh`'s
`ensure_host_firewall_ports()` during install, but the newer
`emudeck-replace-in-place.sh` path -- where a patched AppImage launched
directly via EmuDeck's own Steam shortcut spawns its own private,
ephemeral `dualdeck-host-service` (see `scripts/lib/
apprun_templates.sh`) -- never did. On a host with `firewalld`/`ufw`
active (Bazzite's default), this meant the client could discover the
host (LAN broadcast discovery uses a different, apparently-already-open
port) but never actually connect to its control/video ports --
precisely the kind of silently-dropped connection Bug 1 above turned
into a full hang instead of a clean failure.

Fixed by calling `ensure_host_firewall_ports` once from
`emudeck-replace-in-place.sh` itself, at install time, after any
emulator was actually installed this run (skipped entirely on
`--dry-run` or when nothing was installed) -- matching every other
host-side install path's "open ports when something is installed, not
on every subsequent game launch" convention, since this needs `sudo`
and has no business prompting for a password in the middle of a Steam
game launch. `host_firewall.sh` is now also bundled into the packaged
`host/emudeck-integration/scripts/lib/` release archive alongside
`emudeck_paths.sh`/`appimage_manifest.py`.

**Verified:** end-to-end against local fixtures -- confirmed the
firewall step runs after a real (non-dry-run) install and is skipped
entirely on `--dry-run`; confirmed the firewalld code path invokes
`firewall-cmd --permanent --add-port=...` for all five expected ports
followed by `--reload`, using a fake `firewall-cmd`/`sudo` in `PATH` (no
real firewall state touched by the test itself); confirmed the
"no supported firewall manager found" fallback path doesn't fail the
overall install (`|| true`, matching existing precedent) on a machine
with neither `firewalld` nor `ufw`.

**Not yet verified:** either fix against the user's actual real
hardware -- both are freshly diagnosed and fixed from source-level
investigation plus hermetic tests, not yet confirmed against a real
firewalled Bazzite host and a real client connecting to a real
AppRun-spawned ephemeral host-service end to end.

## 2026-08-01: "Is Host Control always running in the background now?" -- yes, a real bug: the private host-service spawned per-launch was never actually cleaned up

Real user question/report, right after the connect/exit-hang and
firewall fixes above: "even when emulator is not open, it allows me to
attempt to connect with Host Control, but it gets stuck on connecting."
Both halves turned out to be the same root cause.

**Root cause:** `generate_apprun_out_of_process()`'s AppRun template
(`scripts/lib/apprun_templates.sh`) spawns a private, ephemeral
`dualdeck-host-service` in the background when no persistent daemon is
already running, and sets `trap 'kill "${host_service_pid}" ...' EXIT`
so that private instance dies when the AppRun script itself exits --
but the script's very next command was `exec ... "${HERE}/usr/bin/
__REALBIN__" "$@"`. `exec` *replaces* the current process image with
the target binary's -- there is no shell process left afterward to ever
run an `EXIT` trap, regardless of how or when the emulator eventually
exits. Confirmed directly with a two-line repro (`trap 'echo fired' EXIT;
exec /bin/echo hi` -- the trap output never appears). Every single
game launch through this path left its own private `dualdeck-host-
service` running forever afterward, still fully alive and listening,
discoverable by any client's LAN broadcast and happy to accept a
connection into `HostControl` mode (the fallback state for "no adapter
currently registered," which an orphaned, emulator-less host-service is
exactly) -- exactly "Host Control always running in the background."

The "gets stuck on connecting" half follows from the same bug rather
than being separate: `main.cpp` already has a dedicated
`renderHostControlScreen()` for a *successfully* connected HostControl
session (checked once `nowConnected && nowHostMode ==
HostMode::HostControl`) -- a client that actually completes the
handshake into HostControl mode does *not* get stuck showing
"CONNECTING..."; it shows that dedicated screen instead (which, per
`HostControlAdapter::getLatestFrame()`'s hard-coded `false` -- see the
Phase C2 entries below -- never gets real video, but is a distinct UI
state, not a hang). A client stuck on the literal "CONNECTING..." text
means the connection itself never completed -- consistent with the
orphaned host-service being in a broken/half-torn-down state by the
time a client happened to find it, or simply compounding with whichever
of the connect-timeout/firewall fixes above the build under test did or
didn't yet include.

**Fix:** matches a pattern this codebase already established elsewhere
for the exact same reason -- `build-release.sh`'s own generated
`run-host-azahar.sh`/`run-host-cemu.sh` already avoid `exec`ing their
final binary, each with a "Not exec'd: ... this trap needs to still be
able to run" comment -- `generate_apprun_out_of_process()`'s AppRun
template just never got the same treatment when it was written this
session. Changed the final line from `exec env LD_LIBRARY_PATH=... ...
"__REALBIN__" "$@"` to a plain (non-`exec`'d) `env LD_LIBRARY_PATH=...
... "__REALBIN__" "$@"`, so this shell (and its `EXIT` trap) is still
alive once the emulator actually exits, however it exits -- a clean
shutdown or a signal both still trigger a bash `EXIT` trap; only an
uncatchable `SIGKILL` to the whole process group would bypass it, the
same inherent, unavoidable limit every other wrapper script here already
accepts. melonDS's own AppRun template needs no equivalent change -- it
runs its remote server in-process and never spawns a separate
host-service to begin with.

**Verified:** end-to-end with fake binaries -- a fake `dualdeck-host-
service` that just sleeps forever (so it can only ever stop if actually
killed) plus a fake emulator binary that exits quickly, packaged into a
real AppImage and actually run via `--appimage-extract-and-run`.
Confirmed no `dualdeck-host-service` process remains running after the
fake emulator exits and the AppRun script itself completes -- before
this fix, using the exact same test harness, the process would have
stayed running indefinitely (matches the isolated two-line `exec`/trap
repro above).

**Not yet verified:** against real hardware -- specifically, that a
freshly-launched real Cemu/Azahar no longer leaves a lingering
`dualdeck-host-service` process after being closed via Steam, and that a
deliberate Host Control connection attempt (once nothing stale is left
to accidentally connect to) either reaches `renderHostControlScreen()`
cleanly or fails fast, rather than hanging.

## 2026-08-01: Four more real-hardware findings after all three emulators finally launched -- melonDS silent bind failure, Cemu touch/controls, Azahar window, and what Host Control actually does

Real user report, the first round of testing after every packaging fix
above (resources/gameProfiles, glibc-bundling, window-title, connect/
exit-hang, firewall, orphaned-host-service): "MelonDS does not run a
server, cannot connect. Cemu works, no touch screen or controls work
however. Azahar games boot and show running in Steam... but the window
does not appear at all. What does Host Control even do?"

**Host Control (not a bug -- an honest scope gap):** today it only
injects a virtual gamepad on the host; there is no video (`HostControl
Adapter::getLatestFrame()` is hard-coded to return nothing -- no host-
desktop screen-capture code exists anywhere in this codebase yet) and no
mouse/touchpad-to-mouse mapping. Connecting gets a blank/placeholder
screen with a silently-active virtual controller, not the "Deck as a
Steam-Controller-for-the-host-desktop" experience the name implies.
Building that out (host screen capture + mouse injection) is a
substantial net-new feature, not a quick fix -- deliberately not
attempted here without explicit direction to build it.

**melonDS -- "does not run a server, cannot connect":** `EmuInstance::
startRemoteServer()`'s call to `RemoteServerBridge::start()` returned
`void` and unconditionally logged "remote server enabled" regardless of
whether the underlying `NetServer::start()` actually succeeded --
`NetServer::start()` already logs a bind failure, but only to stderr,
invisible under a Steam/EmuDeck launch. Most likely trigger: a leftover
`dualdeck-host-service` process (from before the orphaned-host-service
fix above shipped) still bound to the same default ports (8760-8765)
melonDS's own in-process server also tries to bind -- a real port
conflict, not a defect in melonDS's own patch. Fixed by giving
`NetServer` (`host/remote-server/include/host/net_server.h`, shared
core code) a public `isRunning()` accessor, threading a real `bool`
return value through `RemoteServerBridge::start()` (both constructors),
and having `startRemoteServer()` log a clear error and show a status-bar
message on failure instead of silently claiming success. See
`host/melonds-patches/README.md`'s matching entry.

**Cemu -- "no touch screen or controls work":** two different things.
Touch is confirmed pre-existing and deliberately out of scope (Cemu
itself hard-codes GamePad touch validity to invalid, no plumbing exists
to hook into -- unchanged since the patch's first draft). Controller
buttons/sticks: `CemuAdapter`'s constructor only auto-wires onto VPAD
player 1 if `get_vpad_controller(0)` returns non-null -- if Controller
Settings has player 1 set to anything other than "Wii U GamePad" (Pro/
Classic/Wiimote), that returns null and remote input is silently inert
for the whole session, with video streaming completely normally either
way -- previously undiagnosable from a user report alone. Fixed by
logging a clear line in that branch. This doesn't fix a misconfigured
Controller Settings by itself (nothing is actually broken if that's the
cause -- the logic is working as designed); it turns a silent failure
into an actionable one. See `host/cemu-patches/README.md`'s matching
entry, including the important caveat that regular controller input has
had **no real end-to-end confirmation since the v2.6 rebase** -- if the
new log line doesn't appear and input still doesn't work, the real bug
is elsewhere in the input pipeline and still needs to be found.

**Azahar -- "window does not appear at all":** investigated directly,
the same way the Cemu resources/gameProfiles bug was originally found --
downloaded the actual published `dualdeck-azahar-patched-linux-x86_64.
AppImage`, extracted it, and ran the real binary under Xvfb with the
exact bundled `LD_LIBRARY_PATH` the real `AppRun` sets. **The window
renders completely normally** -- confirmed with a real screenshot
showing Azahar's full main-menu UI (File/Emulation/View/Multiplayer/
Tools/Help, room list, OpenGL/volume/room-connection status bar), no
crash, no "Qt platform plugin" error, no missing library. This rules out
a packaging/bundling defect in the AppImage itself as the cause. Also
directly ruled out: the same-session AppRun `exec`-removal fix (the
orphaned-host-service fix above) -- Azahar and Cemu share the byte-
identical `generate_apprun_out_of_process()` template, and Cemu's window
works fine per the same report, so a template-level regression would
have to affect both identically. **Root cause not yet identified** --
since the AppImage itself demonstrably works in isolation, this is most
likely something specific to the real Steam/EmuDeck/gamescope launch
environment (window manager/compositor interaction, focus/promotion
behavior for a windowed Qt app vs. Cemu's differently-toolkited wx/GTK
window) rather than a defect in this project's own code, but this is
not confirmed. **Needs more information from the user to diagnose
further**: does anything appear even briefly before disappearing, is
Steam in Desktop or Gaming Mode, does a window switcher/alt-tab reveal
an Azahar window that exists but isn't focused/visible.

## 2026-08-01: melonDS was never actually running DualDeck's patch -- EmuDeck installed it as a Flatpak, not an AppImage

Real user report, with the actual real launcher scripts pasted in full
(`~/Emulation/tools/launchers/{cemu,azahar,melonds}.sh`): "Cemu is the
only Emulator that launches from the Steam Game Shortcuts and the
DualDeck host shortcut... MelonDS Opens, but does not open into a Rom,
must be manually selected... Azahar does not open at all."

**Root cause for melonDS, found in the pasted script itself:**
`melonds.sh`'s entire content is
```bash
#!/bin/bash
exec flatpak run net.kuribo64.melonDS --boot=never "$@"
```
EmuDeck installed melonDS as a **Flatpak**
(`net.kuribo64.melonDS`), not an AppImage under `~/Applications` --
`emudeck-replace-in-place.sh` only ever looks for and patches AppImages
there, so it has been silently doing nothing for melonDS on this
configuration the entire time. Every melonDS fix earlier in this
document (the bind-failure visibility fix in particular) is real and
correct, but was never actually exercised on this user's machine --
they were always running completely stock, unpatched melonDS via
Flatpak. The separately-reported "opens but doesn't load the ROM
automatically" is unrelated to DualDeck entirely -- that's EmuDeck's
own `--boot=never` flag on the Flatpak invocation.

**Fix, after asking the user how invasive a fix they wanted (their
answer: automate it fully within the script, no manual steps):**
`scripts/lib/emudeck_paths.sh` gained `emudeck_launchers_dir()`
(`$HOME/Emulation/tools/launchers`, confirmed against the real pasted
scripts) and `find_emudeck_melonds_flatpak_launcher()` (detects the
exact Flatpak-exec pattern). `scripts/emudeck-replace-in-place.sh`
gained `bootstrap_melonds_flatpak_launcher()`: when no melonDS AppImage
is found but this Flatpak launcher is, it downloads the patched
AppImage fresh to `~/Applications/melonDS.AppImage`, backs up
`melonds.sh`, and rewrites its one `flatpak run net.kuribo64.melonDS`
line to exec the new AppImage instead -- dropping `--boot=never` in the
process (an AppImage launch takes the ROM path as a plain positional
argument, the same convention `azahar.sh`/`cemu.sh` already use, so this
also fixes the ROM-doesn't-load report as a side effect). Self-limiting
to one run: once the AppImage exists at that path, every future
invocation of this script takes the normal, already-established
replace-in-place path automatically, since `find_emudeck_melonds_
appimage()` now finds it -- this bootstrap function never runs again
after the first time.

A real bug caught during testing: the normal replace-in-place flow
refuses to proceed if a manifest exists but `<appimage>.dualdeck-
original` is missing (a safety net against exactly this kind of
corruption) -- but the bootstrap path has no real *original AppImage*
to back up (the true original state is the Flatpak launcher, already
preserved separately), so the very next run after a successful
bootstrap would have hit that same safety refusal. Fixed by writing a
small, honest placeholder file there explaining there's no original
AppImage and pointing at the real launcher-script backup instead of
leaving the safety check with nothing to find.

**Verified:** end-to-end against local fixtures reproducing the user's
exact real `melonds.sh` content -- confirmed the bootstrap installs the
AppImage, rewrites the launcher, and backs up the original; confirmed a
second run correctly self-heals into the normal replace-in-place path
with no error; confirmed `--dry-run` has zero side effects (no
download, no file changes) for this path too.

**Cemu confirmed genuinely working** via both the Steam/EmuDeck game
shortcut and the DualDeck host shortcut's "open Cemu" -- validates the
resources/gameProfiles, glibc-bundling, and orphaned-host-service fixes
above against the real launch paths, not just this project's own test
harness.

**Azahar -- further investigation, still unresolved:** re-tested using
the *exact* invocation `azahar.sh` actually uses (`"${exe[@]}"` -- the
raw `.AppImage` file executed directly, no `--appimage-extract-and-run`,
unlike the earlier extracted-binary test above) against a real download
of the published AppImage. This surfaced a real, previously-untested
condition -- `Error: No suitable fusermount binary found on the $PATH`
(this sandbox has `/dev/fuse` but no `fusermount`/`fusermount3` binary)
-- but the AppImage's own embedded runtime handled it as a warning, not
a fatal error: execution continued normally through the entire AppRun
sequence (private host-service spawned, `NetServer` listening on all
five expected ports) and into Qt initialization (the same two harmless
warnings -- `XDG_RUNTIME_DIR not set`, `QPixmap::scaled: Pixmap is a
null pixmap` -- seen in the earlier successful extracted-binary test).
The process was still alive when a `timeout 8` killed it; nothing
crashed on its own within that window. This rules out the missing-
fusermount condition as the cause and further supports that the AppImage
itself is not defective -- whatever's happening is specific to the real
Steam/EmuDeck/gamescope launch environment on the user's actual
hardware, which cannot be reproduced in this sandbox. **Needs real
diagnostic output to make further progress**: running `~/Emulation/
tools/launchers/azahar.sh` directly from a terminal (bypassing Steam
entirely, the same technique that diagnosed the melonDS/Cemu issues
earlier) would capture the actual crash reason instead of Steam's
opaque Play/Stop/Play cycling.

## 2026-08-01: melonDS and Azahar both abort instantly -- Qt platform plugins were never bundled

Real user report, with actual crash output captured by running both
launcher scripts directly from a terminal (exactly the diagnostic step
asked for): both melonDS and Azahar crash identically, immediately
after the AppRun sequence otherwise completes normally (private host-
service spawned, `NetServer` listening on all expected ports):

```
qt.qpa.plugin: Could not find the Qt platform plugin "wayland" in ""
qt.qpa.plugin: Could not find the Qt platform plugin "xcb" in ""
This application failed to start because no Qt platform plugin could be initialized.

.../AppRun: line 93: ... Aborted (core dumped) env LD_LIBRARY_PATH=... "${HERE}/usr/bin/azahar" "$@"
```

**Root cause:** `bundle_library_dependencies()` (`scripts/lib/
appimage_pack.sh`) only ever bundles `ldd`-reported *linked*
dependencies -- Qt's platform plugins (`libqxcb.so`, the Wayland ones)
are `dlopen()`'d at runtime based on `QT_PLUGIN_PATH`/Qt's own compiled-
in fallback path, invisible to `ldd`, so they were never bundled into
either AppImage at all. Every previous test of the packaged AppImages in
this document's earlier entries happened to run inside environments
(this project's own build/test sandbox) that coincidentally had a fully
matching system Qt6 install providing a working fallback -- masking the
gap entirely. Real Bazzite/EmuDeck HTPC machines have no reason to have
a system Qt6 GUI stack installed at all, so the fallback that had been
silently saving every earlier test simply doesn't exist there, and Qt
aborts outright with an empty search-path list ("in \"\""). **Cemu
(wxWidgets/GTK, not Qt) was completely unaffected** -- exactly matching
the user's own observation that Cemu was "unaffected and still working
everywhere" while both Qt-based emulators broke identically, which is
what actually pinned this down as a Qt-specific packaging gap rather
than anything AppRun-, exec-, or launcher-specific.

**Fix:** `scripts/lib/appimage_pack.sh` gained `find_qt6_plugins_dir()`
(tries `qtpaths6 --query QT_INSTALL_PLUGINS` first, falls back to
searching common distro install prefixes). `build-release.sh`'s
packaging step stages the real `platforms/` plugin subdirectory once
(containing `libqxcb.so` plus whatever Wayland platform plugins are
available) and bundles it into both the melonDS and Azahar AppImages via
`pack_appimage()`'s existing `extra_dirs` mechanism (the same one
already proven for Cemu's `resources`/`gameProfiles`) -- landing at
`AppDir/usr/bin/platforms/`. Both AppRun templates
(`scripts/lib/apprun_templates.sh`) now export `QT_PLUGIN_PATH="${HERE}
/usr/bin"` (the *parent* of the bundled `platforms/` directory, matching
how Qt expects this variable to be laid out) before executing the real
binary -- harmless for Cemu, which never reads it. Also added
`qt6-wayland`/`qt6-qtwayland` to the build-dependency list (`ensure_
packages "build"` in `build-release.sh`) -- Debian/Ubuntu's `qt6-base-
dev` alone only provides the X11/xcb platform plugin, not Wayland-native
support, so this ensures the bundled `platforms/` directory covers both
session types rather than relying on XWayland compatibility alone.

**Verified:** built a real, minimal Qt6 GUI app (`QApplication` +
`QLabel`), packaged it through the actual `pack_appimage()`/AppRun
pipeline with the real bundled `platforms/` directory, and ran it with
`QT_PLUGIN_PATH`/`QT_QPA_PLATFORM_PLUGIN_PATH` explicitly unset in the
outer environment under Xvfb -- initialized cleanly (`QT_INIT_OK`), no
platform-plugin error. A same-machine negative-control test (identical
app, packaged without the bundled plugins) did **not** reproduce the
original crash here, because this sandbox's own system Qt6 install
happens to provide a working compiled-in fallback the user's real
Bazzite/Fedora machine doesn't have -- the real crash log (an empty `in
""` search path, meaning no fallback existed at all) is what actually
confirms the mechanism, not this sandbox's imperfect negative
reproduction. The positive test is what matters: explicitly bundling and
pointing at real platform plugins works regardless of what a given
machine's fallback would or wouldn't have found on its own.

**Not yet verified:** against the real hardware that reported this bug
-- needs an actual re-test of both melonDS and Azahar once this ships.

## 2026-08-01: melonDS still aborted after the Qt-plugin-bundling fix shipped -- "Could not LOAD" instead of "Could not find"

The very re-test the previous entry called for came back with a *different*
crash, on the same real Bazzite/Fedora HTPC:

```
qt.qpa.plugin: Could not load the Qt platform plugin "wayland" in "" even though it was found.
qt.qpa.plugin: Could not load the Qt platform plugin "xcb" in "" even though it was found.
This application failed to start because no Qt platform plugin could be initialized.

Available platform plugins are: eglfs, linuxfb, minimal, minimalegl, offscreen, vkkhrdisplay, vnc, wayland-egl, wayland, xcb.
```

Progress, not a regression: `QT_PLUGIN_PATH` now correctly points at the
bundled `platforms/` directory and Qt's plugin loader finds `libqxcb.so`
there -- the previous fix worked. But loading a plugin means `dlopen()`ing
its own `.so` file, which pulls in *that file's own* shared-library
dependencies, completely separately from the main `melonDS`/`azahar`
binary's dependency graph. Confirmed directly: `ldd .../qt6/plugins/
platforms/libqxcb.so` lists a large chain never bundled by the previous
fix -- `libQt6XcbQpa.so.6`, a dozen `libxcb-*.so`, `libxkbcommon.so`,
`libEGL.so`, `libfontconfig.so`, and more. `bundle_library_dependencies()`
only ever ran against the *main binary* passed into `pack_appimage()` --
a `dlopen()`'d plugin is a separate ELF file loaded later, invisible to
`ldd <main binary>` no matter how thorough that one pass is, so none of
`libqxcb.so`'s own dependencies were ever bundled in the first place.

**Fix:** `pack_appimage()`'s `extra_dirs` handling (`scripts/lib/
appimage_pack.sh`) now scans every extra dir it copies in for actual
shared-library files (`*.so`, `*.so.N`) via `find` and runs
`bundle_library_dependencies()` against each one it finds, exactly like it
already does for the main binary. A no-op for Cemu's `resources/
gameProfiles` extra dir (plain data, no `.so` files in there at all) --
load-bearing for melonDS/Azahar's bundled `platforms/` plugin directory,
where every `.so` file is itself a real, dependency-laden shared object.

**Verified:** rebuilt the same real, minimal Qt6 GUI app used in the
previous entry, packaged it through the now-fixed `pack_appimage()`
pipeline with the same bundled `platforms/` directory, and confirmed
`libQt6XcbQpa.so.6` plus the full xcb/xkbcommon/fontconfig chain now
appear in the AppImage's bundled `usr/lib` (112 libraries total, versus
far fewer before this fix). Ran the resulting AppImage under a fresh Xvfb
display with `QT_PLUGIN_PATH`/`QT_QPA_PLATFORM_PLUGIN_PATH` explicitly
unset in the *outer* environment (so nothing outside the AppImage's own
AppRun could be silently supplying a working plugin path) -- initialized
cleanly (`QT_INIT_OK`, exit 0), no platform-plugin error.

**Not yet verified:** against the real hardware that reported this bug --
needs an actual re-test of both melonDS and Azahar once this ships.

## 2026-08-01: melonDS and Azahar launch cleanly with the fix above, but produce no window on the host at all

Real re-test on the same Bazzite/Fedora HTPC, both from Steam and from a
terminal directly (ruling out a Steam/gamescope launch wrapper -- none
was configured anyway): both emulators start with no crash, `NetServer`
listening normally, and the client connects and streams video
successfully. But **no window, taskbar entry, or anything else appears
on the host's own display at all** -- confirmed by the user looking
directly at the host's monitor, not just going by log output.

**Root cause:** melonDS's frame capture for the client
(`GLBottomScreenCapture`) and Azahar/Cemu's equivalent (`AdapterBridge`)
both read frames directly from an OpenGL render target -- this works
whether or not the emulator's own window is actually mapped onto a
display, so a healthy client video stream is not proof the host window
exists. The previous fix bundled `platforms/` (the QPA plugin itself:
`libqxcb.so`, `libqwayland-egl.so`) plus *that plugin's own* linked
dependencies -- enough for Qt to load a platform plugin without
aborting. But actually creating and mapping a GL-backed *window* needs
several more Qt plugin categories that were never bundled at all:
`xcbglintegrations` (GLX/EGL integration Qt needs for a GL-backed window
over X11/XWayland) and, on a Wayland session, `wayland-shell-integration`
(the xdg-shell protocol code that actually asks the compositor to map a
toplevel window -- without it, Qt can construct a window object and keep
rendering into it internally, but never actually tells the compositor to
show anything), `wayland-decoration-client`, and `wayland-graphics-
integration-client`. None of these are QPA platform plugins themselves,
so their absence doesn't produce Qt's usual "Could not find/load
platform plugin" abort -- window creation just silently no-ops, which is
exactly why this surfaced as a second, completely different-looking bug
after the first fix rather than as a variant of the same error. Modern
Fedora/Bazzite desktop sessions default to Wayland, matching why this
wasn't caught by the previous fix's positive verification (done under
Xvfb, which is X11-only).

**Fix:** `scripts/build-release.sh`'s AppImage-packaging step now stages
and bundles `xcbglintegrations`, `wayland-shell-integration`,
`wayland-decoration-client`, and `wayland-graphics-integration-client`
alongside `platforms/` (all under the same `QT_PLUGIN_PATH` root the
AppRun scripts already export), reusing the existing per-`.so`
dependency-bundling logic from the previous fix for each of these too.
`xcbglintegrations` is required (build fails without it, matching
`platforms/`'s existing behavior); the three Wayland-only categories are
bundled if present on the build machine and skipped gracefully
otherwise, since XCB/XWayland support alone is enough to fix the
originally reported bug even on a build machine with no Wayland Qt
packages installed.

**Verified:** built a real `QOpenGLWidget`-based Qt6 app (closer to
melonDS/Azahar's actual GL-backed rendering than the previous entry's
plain `QLabel` test), packaged it through the fixed pipeline with all
five plugin categories bundled, and ran it under Xvfb -- `xwininfo -root
-tree` showed a real, mapped 320x240 window, not just a clean process
exit. **Could not verify the Wayland-specific half of this fix at all**:
this sandbox has no Wayland compositor available to run against, and a
same-machine X11 negative-control test (repackaging the identical app
with only `platforms/`, no `xcbglintegrations`) *also* produced a mapped
window under Xvfb here -- meaning this sandbox's Mesa/GL setup has a
fallback that masks whatever's actually happening on the user's real
Wayland desktop session, the same category of sandbox limitation flagged
in this document's earlier Qt-plugin entries. The reasoning for the
Wayland half rests on directly confirming (via a plain directory listing
of a real Qt6 install) that `wayland-shell-integration` et al. exist as
separate, never-bundled plugin categories, and on how Qt's Wayland QPA
backend is documented to use them -- not on a reproduced-and-fixed local
failure.

**Not yet verified:** against the real hardware that reported this bug --
this is the one to watch most closely on the next re-test, since the
Wayland half of the fix has no sandbox verification behind it at all.

## 2026-08-01: Host Control gained touchpad-as-mouse support (protocol v11)

Real user question, after Azahar's Steam-shortcut launch was finally
confirmed working: "host control should be able to basically let me use
my steam deck as a steam controller, the touchpads should work as a
mouse, etc. but it currently does none of that." Investigating
`host::HostControlAdapter` confirmed it: only ever translated gamepad
buttons into a virtual Xbox-360-style `uinput` device -- there was no
touchpad/mouse support anywhere in the wire protocol or either side's
code, and video capture for Host Control mode remains a hard-coded stub
(`getLatestFrame()` always returns `false` -- see this project's own
planning notes: a from-scratch host-screen-capture subsystem is
explicitly scoped as later, larger work, not part of this).

Scoped with the user to exactly: touchpad-as-mouse + the existing
gamepad navigation, no video. Big-Picture auto-launch and full remote-
desktop video streaming remain unbuilt, tracked separately.

**What shipped:**
- `protocol.h`/`protocol.cpp`: `ControllerState` gained `mouseDeltaX`,
  `mouseDeltaY` (relative motion, `int16_t`) and `mouseButtons` (a new
  `MouseButton` bitmask: left/right). A genuine wire-shape change --
  `kProtocolVersion` bumped 10 -> 11, `kControllerStateWireSize` 29 -> 34
  bytes. See `docs/protocol.md`'s "ControllerState payload" section for
  the full field table.
- `client/src/main.cpp`: captures `SDL_EVENT_MOUSE_MOTION`'s relative
  `xrel`/`yrel` (not the absolute `x`/`y` the existing touch-screen-via-
  trackpad feature already uses -- these are two independent input
  interpretations of the same underlying SDL mouse events, coexisting in
  the same event-handling switch without conflict) and
  `SDL_EVENT_MOUSE_BUTTON_DOWN`/`UP` for left/right clicks. Accumulates
  deltas between the ~120Hz send ticks, clamps to `int16_t` range on
  send (saturates rather than wraps on an unusually large single-tick
  delta), and resets to 0 after every send -- a dropped UDP packet's
  motion is genuinely lost, not retried, the same accepted trade-off
  every other best-effort field on this channel already has. Filters out
  `SDL_TOUCH_MOUSEID`-sourced synthetic events (a real touchscreen tap
  shouldn't also move the host's cursor), matching the existing filter
  the touch-screen-via-trackpad code already uses for the same reason.
- `host::HostControlAdapter`: gained a second, independent `uinput`
  device (`EV_REL` + `REL_X`/`REL_Y`, `EV_KEY` + `BTN_LEFT`/`BTN_RIGHT`)
  rather than folding mouse capability into the existing virtual gamepad
  device -- mixing relative-motion semantics into a device that
  advertises itself via the Xbox 360 vendor/product ID would confuse
  desktop environments' own device-type heuristics. `isMouseDeviceReady()`
  is independent of the existing `isDeviceReady()` (gamepad) -- degrades
  gracefully and independently if only one of the two `uinput` device
  creations fails.

**Verified:** `dualdeck_protocol_tests` (104 cases, including new
round-trip/negative-delta/default-zero tests for the new fields) and
`melonds_remote_host_tests` (79 cases, including new
`translateMouseState()` pure-logic tests mirroring the existing
`translateControllerState()` ones, plus updated degrade-gracefully
coverage for `isMouseDeviceReady()`) both pass. Built SDL3 (release-3.2.16,
this project's own pinned tag) from source in-sandbox specifically to
compile-verify `client/src/main.cpp`'s new event-handling code (normally
untestable here -- `DUALDECK_BUILD_CLIENT` needs a real SDL3 install this
sandbox doesn't have by default) -- built clean with zero warnings even
under this project's strict `-Wall -Wextra -Wpedantic -Wconversion
-Wshadow` flags.

**Not yet verified:** against real hardware -- no `/dev/uinput` access in
this sandbox (documented limitation carried since Host Control's original
gamepad-only milestone), and SDL's synthesis of `xrel`/`yrel` from a
Steam Deck touchpad specifically configured as a mouse (via Steam
Input's "Trackpad" binding or Gaming Mode's own default) hasn't been
exercised against a real Steam Input session at all -- only reasoned
about from SDL's documented relative-mouse-motion event shape, which
this project's own precedent (see the two Qt-plugin entries above) has
repeatedly shown can still hide a real-hardware surprise.

## 2026-08-01: Client now shows its own version, and a protocol mismatch now names the host's version

Real user report, testing the Host Control mouse fix above: hit "protocol
version mismatch" and had no way to tell which side (client or host) was
actually out of date -- "the client should show which version it is on,
in the event it fails to auto update, I have no way of telling other
than uninstalling and reinstalling." A real gap: `run-client.sh` already
auto-updates the client on every launch (falling back silently to the
current version if that fails -- offline, GitHub unreachable, a partial
download), so a stale install was already possible with zero on-screen
indication.

**What shipped:**
- `client/src/main.cpp`: the two picker screens every launch reaches
  regardless of whether a host is ever found (`renderDiscoverySearching()`,
  `renderDiscoveryList()`) now stamp this client's own `DUALDECK_VERSION`
  in small, dim text in the bottom-left corner (`renderClientVersionStamp()`).
  Skipped entirely when empty (a from-source dev build run without
  `run-client.sh` setting the env var), rather than showing a misleading
  blank.
- Both on-screen `ProtocolVersionMismatch` messages (the setup wizard's
  connect step, and the main picker's post-selection connect screen) now
  include the host's actual version -- `NetServer` already sends its real
  `appVersion` in every `HelloAck` unconditionally, even a rejected one
  (confirmed by reading `net_server.cpp`), and the client already stores
  it (`hostAppVersion_`) before checking whether the handshake was
  accepted -- so this was already safely available and just wasn't being
  shown, exactly like the pre-existing `AppVersionMismatch` message
  already does for the app-version (not protocol-version) case.

**Verified:** rebuilt SDL3 from source again to compile-check
`client/src/main.cpp` (clean, zero warnings), all three test suites still
pass unchanged (this is a display-only change, no logic/protocol
behavior touched). Ran the actual `dualdeck-client` binary under Xvfb
with `DUALDECK_VERSION=v0.1.104` and confirmed via a screenshot that
"V0.1.104" renders correctly in the discovery screen's corner.

**Not yet verified:** against real hardware -- specifically whether this
now gives the user enough information to resolve their actual protocol-
mismatch report from real testing.

## 2026-08-01: Touchpad-as-mouse still didn't move the host cursor -- SDL_EVENT_MOUSE_MOTION never fires without a specific Steam Input binding

Real re-test, after the protocol mismatch above was resolved: gamepad
navigation in Host Control mode works, but the touchpad still didn't
move the host's cursor at all. The user's own guess was that this
needed emulating a Steam Controller instead of an Xbox 360 one on the
host side -- worth addressing directly since it's not actually the
mechanism: the virtual gamepad (Xbox 360 identity) and the virtual mouse
`host::HostControlAdapter` creates are already two separate `uinput`
devices (see the touchpad-as-mouse entry above) -- mouse motion on the
host never passed through the gamepad's identity at all, so changing it
wouldn't have changed anything.

**Root cause:** the previous fix captured the touchpad via
`SDL_EVENT_MOUSE_MOTION`, which only fires if Steam Input's *currently
bound control scheme* maps a touchpad to "as mouse" specifically. Most
gamepad-style control schemes (including whatever this client's own
default Steam Input template is) don't do that at all, so the client
never received a single mouse-motion event from the touchpad --
nothing was ever reaching the host to move, regardless of anything on
the host side.

**Fix:** `client/src/main.cpp` now additionally captures
`SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN`/`_MOTION`/`_UP` -- SDL's dedicated
gamepad-touchpad API (the same mechanism PS4/PS5 controller touchpad
support uses), which reads the Deck's touchpads directly as raw touch
data *independent* of whatever control scheme is currently bound; Steam
Input passes this through unconditionally. Reports absolute, normalized
(0..1) per-(touchpad, finger) position, converted to a relative delta
against each finger's own previously-seen position (no delta on the
first position seen after a touch begins, to avoid a spurious jump).
`SDL_EVENT_MOUSE_MOTION` is kept alongside this, not removed -- harmless
if it never fires, still useful for a real desktop mouse in Desktop Mode
or a control scheme that does bind "as mouse." Clicks gained a second
source too: `SDL_GAMEPAD_BUTTON_TOUCHPAD` (the touchpad's own physical
press-down, same button PS4/PS5 controllers report), polled alongside
the existing gamepad button/stick reads and composed with (not
replacing) the event-driven mouse-button click state.

**Verified:** rebuilt SDL3 from source again to compile-check the new
event handling (clean, zero warnings under this project's strict
flags), all three test suites still pass unchanged (protocol/host sides
weren't touched by this fix at all -- it's entirely client-side input
capture).

**Not yet verified:** against real hardware -- this sandbox has no Steam
Deck (or any physical gamepad with a touchpad) to generate real
`SDL_EVENT_GAMEPAD_TOUCHPAD_*` events against, so this rests on reading
SDL's documented API contract and Valve's documented Steam Input
touchpad-passthrough behavior, not a reproduced-and-fixed local test --
the same category of limitation flagged repeatedly elsewhere in this
document. The `kTouchpadMouseSensitivity` scale factor is an arbitrary
starting guess with no real-hardware feel-tuning behind it either.

## 2026-08-01: "Host control only" launched melonDS anyway, which immediately ended Host Control mode before the user could ever interact with it

Real user report, after the touchpad-API fix above still didn't produce
any observable effect on the host at all: the actual host log revealed
the real cause, unrelated to input capture entirely --

```
melonds-remote: connected to Host Service
ModeCoordinator: switching to Emulation mode (system=Nintendo DS, adapter=melonDS)
```

**Root cause:** `run-host.sh`'s `DUALDECK_HOST_CONTROL=1` branch started
the standalone Host Service correctly, but then *also* launched melonDS
immediately afterward (as an out-of-process adapter, so it would be
"ready" the instant a ROM was picked). `ModeCoordinator` has no concept
of "an adapter is connected but idle, no ROM loaded yet" -- it switches
to Emulation mode the moment *any* adapter connects, full stop. melonDS's
out-of-process bridge connects within a fraction of a second of
starting, so every single "Host control only" launch flipped out of
Host Control mode almost immediately, regardless of whether a ROM was
ever loaded. The client then sat silently in Emulation mode waiting for
video frames that would never arrive (no ROM = nothing for melonDS to
render) -- indistinguishable, from the user's side, from "the touchpad
and buttons just don't work," since Host Control's own screen/behavior
was never actually reachable long enough to test.

**Fix:** `scripts/build-release.sh`'s generated `run-host.sh` no longer
launches melonDS at all in this branch -- "Host control only -- no
emulator" now means exactly that. The standalone Host Service runs in
the foreground (`exec`'d directly, no backgrounding/trap needed anymore
since there's no child emulator process to worry about orphaning) until
Ctrl+C. Launching an emulator to actually play something is a separate
action (its own Steam shortcut) that starts its own separate session,
rather than automatically taking over this one -- genuinely
emulator-agnostic hand-off (one persistent daemon, with every emulator's
launch path connecting to the *same* socket so Host Control mode hands
off automatically the moment any of them is opened) needs the
persistent-daemon work described in this document's Phase B entry, which
doesn't exist yet. This is a real, current limitation, not silently
glossed over -- printed directly to the user when Host Control mode
starts.

**Verified:** extracted the generated script body and ran it standalone
against a stub `dualdeck-host-service` (a script that prints the same
"running" banner and exits cleanly on SIGTERM) -- confirmed melonDS is
never invoked at all, the Host Service runs in the foreground with the
correct arguments, and `SIGTERM` (Ctrl+C's equivalent) is delivered
directly to it and produces a clean exit, since `exec` replaces the
shell entirely with no orphaned child process risk.

**Not yet verified:** against real hardware -- this is the one that
actually matters: does gamepad/mouse input now register once the
session genuinely stays in Host Control mode, since the previous two
"fixes" (touchpad capture method, protocol version visibility) were
real and necessary but couldn't have worked while this bug made Host
Control mode unreachable in the first place.

## 2026-08-01: CI caught what the local build couldn't -- the vendored patch protocol copies and two smoke tests were still frozen at v10

The Host Control fix above (a pure `scripts/build-release.sh` bash
change, no protocol/host code touched) still broke CI: this repo's own
`check-patch-protocol-sync.sh` tripwire (added specifically to catch
this exact failure mode after it happened once before, see this
document's "frozen protocol copy" entry) caught that the `mouseDeltaX`/
`mouseDeltaY`/`mouseButtons` work several commits back bumped
`kProtocolVersion` to 11 in the live header, but never touched the three
patches' own *vendored* full copies of `protocol.h` (melonDS, Azahar,
and Cemu each embed their own complete copy inside their integration
patch rather than referencing the shared `protocol/` directory -- see
that same earlier entry for why). All three were still frozen at v10.
Separately, `tests/smoke_test.py` and `tests/device_approval_smoke_test.py`
each hardcode their own `VERSION` constant for the raw packets they
construct (no build step in either script that could read the live
header instead) -- also still 10, which made every Hello either script
sent get rejected as a protocol-version mismatch instead of exercising
whatever that test case actually meant to check; the real CI failure
this produced was an `AssertionError: expected AuthenticationFailed, got
1` (`1` being `ProtocolVersionMismatch`, not the auth-failure case the
test intended).

**Fix:** regenerated all three patches' embedded `protocol.h` copies
from the live header (same "new file" hunk shape, just the current
752-line content and an updated `@@ -0,0 +1,N @@` line count). Bumped
`VERSION` to 11 in both smoke test scripts, and extended
`smoke_test.py`'s `controller_state_payload()` struct-pack format
(`"<IQHHhhhhBHH"` -> `"<IQHHhhhhBHHhhB"`) to match `ControllerState`'s
new 34-byte wire size -- it was still packing the old 29-byte shape,
which would have failed the very next packet send once the version
constant was fixed and this test actually got that far.

**Verified:** `check-patch-protocol-sync.sh` now reports all three
patches in sync; re-cloned real upstream source at each pinned commit
(melonDS/Azahar/Cemu) and confirmed `git apply --check` still succeeds
against all three regenerated patches. Built `dualdeck-host-service`
locally and ran both `tests/smoke_test.py` and
`tests/device_approval_smoke_test.py` directly against it --both now
print PASSED end to end (not just up to the point the stale version
constant used to derail them).

**Lesson for next time:** any future `kProtocolVersion` bump needs all
five of these updated in the same change: the three vendored patch
copies, `smoke_test.py`, and `device_approval_smoke_test.py` -- none of
which failed locally in this session's own build+test verification
before pushing, since none of that ran through this repo's own CI
workflow (`ci.yml`) or exercised the vendored patch copies at all.

**Also worth noting:** a release (v0.1.107) was published from the
commit immediately before this fix -- its patched melonDS build was
compiled against the still-frozen v10 vendored `protocol.h` copy, while
the client/host-service were built from the live v11 code. Since
melonDS's in-process `NetServer` (built from that vendored copy) checks
`header.protocolVersion == kProtocolVersion` before accepting a
handshake, a v11 client connecting to that specific build would fail
with a real, correct `ProtocolVersionMismatch` -- not a false alarm, an
actual mismatch this v10-vendored build has. Azahar/Cemu's out-of-process
adapters don't run `NetServer` themselves (the standalone
`dualdeck-host-service`, built from live v11 code, does), so they were
unaffected by this specific build's vendored-copy staleness.

## 2026-08-01: Touchpad still silent in a genuinely-staying-in-Host-Control session -- added diagnostics rather than guess again

Real re-test, after the "stop launching melonDS" fix: gamepad input is
now recognized on the host (a real Xbox-identity virtual controller
shows as connected -- `HostControlAdapter`'s uinput device is working
correctly), but the touchpad still produces no motion at all. The
user's renewed suggestion was to change the *host's* virtual gamepad to
identify as a Steam Controller instead of Xbox 360, since Steam
Controllers have touchpads -- worth addressing directly: this wouldn't
help, because the host's virtual gamepad is a pure output device other
host-side apps read; it has no bearing on whether the *client*
successfully captures touchpad motion from the Deck's own hardware in
the first place. That capture (`SDL_EVENT_GAMEPAD_TOUCHPAD_*`) happens
entirely client-side, before anything reaches the host.

That said, the underlying intuition -- that *device identity* affects
touchpad visibility -- is correct, just aimed at the wrong device: SDL
only exposes gamepad-touchpad capability (`SDL_GetNumGamepadTouchpads()`
returning nonzero) if it identifies the specific connected gamepad
instance as touchpad-capable in its own mapping database, keyed off
whatever vendor/product ID Steam Input presents to SDL on the Deck
itself -- entirely outside DualDeck's control, and not something this
sandbox (no physical Steam Deck) can verify by reasoning alone.

**What shipped:** rather than guess at a third capture mechanism with no
way to verify it, `client/src/main.cpp` now logs, the moment a gamepad
connects, its SDL-reported name and `SDL_GetNumGamepadTouchpads()` count
(and finger-slot count per touchpad found) -- and separately logs the
very first raw `SDL_EVENT_GAMEPAD_TOUCHPAD_*` event ever received,
*before* any of this code's own filtering, so a real client log
(`~/.config/dualdeck-client/client.log`) will show definitively whether
SDL sees zero touchpads on this exact gamepad (a mapping/identification
problem, entirely outside this project) versus sees touchpads but never
receives an event (a different, still-unknown problem) versus receives
events that this code's own bounds-checking is incorrectly discarding
(a real, fixable bug in the code added earlier).

**Verified:** rebuilt SDL3 to compile-check the added logging (clean,
zero warnings), all three test suites pass unchanged (logging-only
change).

**Not yet verified:** against real hardware -- this is the actual
diagnostic step needed before another guess is worth making; the client
log from a real re-test is the next thing to look at.

## 2026-08-02: Persistent, Steam-decoupled Host Control daemon + auto mode-switching + update-reset, and a Steam Controller touchpad HID experiment

Real re-test feedback after the previous entry's diagnostics shipped, two
reports in one message: (1) in Steam Big Picture mode, controller/stick
and even the touchscreen worked, but the touchpads still didn't move
anything on the host -- "it needs to function exactly like a Steam
Controller would"; (2) launching a Cemu/Azahar game from Big Picture
never switched off Host Control mode and never streamed the game, because
Host Control today only runs as the literal foreground process of the
"DualDeck Host" Steam shortcut -- Steam has no separate PID/session layer
to watch, so it considers "the game" running for as long as that one
process tree lives, with no way to toggle it off short of killing the
shortcut, and no way for a second, later-launched emulator to ever be
noticed by the same host-control session.

**Root causes, confirmed by reading the code, not assumed:**

- `AdapterIpcServer`'s well-known default socket
  (`adapter-sdk/ipc/src/socket_path.cpp`'s `defaultAdapterSocketPath()`,
  `$XDG_RUNTIME_DIR/dualdeck/adapter.sock`) was already probed by the
  AppImage/EmuDeck out-of-process launch path, but not by any of
  DualDeck's own packaged launcher scripts (`run-host-azahar.sh`,
  `run-host-cemu.sh`), which each always spawned and killed their own
  private, per-launch Host Service instead.
- `ModeCoordinator::pollLoop()` already does exactly the auto-switching
  this needs -- mode is Emulation iff `hasConnectedAdapter()`, polled
  every 100ms -- and needed zero code changes. The only real gap was that
  nothing persistent was ever listening on the shared socket for a
  later-launched emulator to find.
- Nothing decoupled Host Control's process lifetime from Steam's shortcut
  tracking. `run-host.sh`'s `DUALDECK_HOST_CONTROL=1` branch `exec`s
  straight into `dualdeck-host-service`, so that process *is* what Steam
  watches -- there was never a background-service layer to toggle
  independently.

**What shipped:**

- **A real persistent daemon.** A new `systemd --user` unit
  (`dualdeck-host-control.service`, `Type=simple`, `Restart=on-failure`,
  `WantedBy=default.target` -- broader than `graphical-session.target`
  since an HTPC session may be headless) runs `host-control-daemon.sh`,
  which execs `dualdeck-host-service --adapter-ipc` with **no**
  `--adapter-socket` passed, so it binds the shared default socket and
  becomes the one persistent thing every emulator launch can find.
  `install-host-control-daemon.sh`/`uninstall-host-control-daemon.sh`
  install/remove the unit, refusing cleanly (not erroring) on
  immutable/Distrobox systems or where `systemctl --user` isn't reachable
  at all, matching `install-host-distrobox.sh`'s existing rejection style.
- **A GUI toggle**, not just raw `systemctl` commands: `dualdeck-host.sh`'s
  menu gained a new entry whose label reflects live daemon state ("(not
  running)" / "(RUNNING)"), offering Start/Stop/Status, built so a future
  Decky plugin can trivially shell out to the same two `systemctl --user`
  calls. Uninstalling now also tears the daemon down.
- **Auto mode-switching for every launcher, not just the AppImage path.**
  New `scripts/lib/adapter_socket_probe.sh` factors the AppImage path's
  proven probe/fallback logic into a shared `probe_or_spawn_adapter_socket()`
  helper: try the persistent daemon's shared socket first, only spawn a
  private ephemeral Host Service if nothing answers. `run-host-azahar.sh`
  and `run-host-cemu.sh` now use it. melonDS's plain (non-Host-Control)
  launch tail in `run-host.sh` also probes the shared socket before its
  final exec, exporting `MELONDS_REMOTE_OUT_OF_PROCESS=1` to connect to a
  running daemon when one exists -- probe-only, no spawn fallback, so
  melonDS's proven in-process default is unchanged when no daemon is
  running. The AppImage AppRun template's own embedded copy of this logic
  is unchanged in behavior, just cross-referenced in comments against the
  new shared library (an AppImage's `AppRun` can't `source` a file outside
  the `.AppImage`, so it stays a hand-synced duplicate, same precedent
  as `adapter_ipc_client.cpp`/`adapter_ipc_server.cpp`'s `recvMessage()`).
- **Reset on update.** `apply-update.sh` now checks whether the daemon
  was active before installing an update and, if so, runs
  `systemctl --user restart` after -- literally "toggle off and back on,"
  as requested, logged as a warning (not a failed update) if the restart
  itself fails.
- **A Steam Controller touchpad HID experiment**, opt-in via
  `DUALDECK_HOSTCONTROL_STEAM_TOUCHPAD=1` (default off, zero behavior
  change otherwise): `HostControlAdapter`'s existing virtual gamepad
  device gains `ABS_MT_POSITION_X/Y`, `ABS_MT_TRACKING_ID`, `ABS_MT_SLOT`
  (single slot -- the wire protocol carries one aggregate delta stream,
  not per-finger identity), `BTN_TOUCH`, and `INPUT_PROP_BUTTONPAD`, and
  identifies with Valve's own wired Steam Controller USB IDs
  (`0x28de`/`0x1102`) instead of the Xbox 360 IDs it normally reuses.
  Wire-level `mouseDeltaX/Y` is dead-reckoned into an absolute touchpad
  position (clamped, not wrapped); a touchpad click (`MouseButton_Left`
  held) is used as a "finger present" proxy for `ABS_MT_TRACKING_ID`,
  since the wire protocol has no real finger-down/up signal. New setup
  calls are deliberately isolated from the existing gamepad/stick/mouse
  setup's own failure path (`isTouchpadReady()`, independent of
  `isDeviceReady()`/`isMouseDeviceReady()`) -- a failure here only
  disables this one opt-in feature.

  **Said plainly, because this was explicitly flagged to the user before
  implementing it anyway at their request:** real Steam Controller/Steam
  Deck touchpad recognition goes through Steam's own
  `SDL_JOYSTICK_HIDAPI_STEAM` driver, which reads `/dev/hidraw*` in
  Valve's proprietary HID report format. A `uinput`-created device only
  ever produces a plain `/dev/input/eventN` (evdev) node -- there is no
  corresponding hidraw node, and no vendor/product ID or capability bits
  can make one appear. This is a hard architectural ceiling `uinput`
  cannot cross, not a tuning problem. The realistic outcome is Steam
  treating this device as an unrecognized/generic gamepad with unused
  extra axes, not native touchpad-cursor behavior in Big Picture. This
  does **not** replace the still-separately-needed client-side fix for
  whatever is preventing `SDL_EVENT_GAMEPAD_TOUCHPAD_*` from reaching the
  wire in the first place (see the previous entry's diagnostics) -- the
  wire-level data this reads has to originate from the client either way.

**A collision deliberately avoided:** `AdapterIpcServer::start()`
unconditionally `unlink()`s any existing socket file before binding, with
no "is a live process already here" check. `run-host.sh`'s manual
Host-Control-only branch therefore still uses its own private socket path,
not the shared default -- pointing it at the shared path would silently
steal it from a running daemon. It now prints a non-blocking informational
note if the daemon already looks active, nothing more.

**A real, still-open IPC risk, found while reading `adapter_ipc_server.cpp`
rather than assumed:** `AdapterHelloAckReason::AlreadyRegistered` is
defined and serialized but never actually sent -- `acceptLoop()` is
strictly sequential with a `listen()` backlog of 1. A second adapter
connecting while one is already active sits queued at the OS level rather
than getting an explicit rejection; the second client only notices via its
own 5s receive timeout. A persistent, always-reachable daemon is exactly
the scenario that first makes two-emulators-at-once a real, not
hypothetical, case. Not fixed in this change (flagged as optional
hardening for `acceptLoop()` to non-blockingly reject immediately instead)
-- needs its own dedicated test before being relied on as safe.

**Verified:** every script-level change (private-socket fallback spawn,
shared-socket direct-connect with no private spawn attempted, melonDS's
probe-only plain-launch branch, the manual Host-Control-only informational
note, the update-reset restart-only-if-was-active logic, and the "no
`systemd --user` available" degrade-gracefully path) via real functional
sandbox tests: stub `dualdeck-host-service`/emulator binaries, a fake
`systemctl`, and real Python `AF_UNIX` listeners simulating a live daemon.
`host_control_adapter.cpp`'s touchpad changes build clean with
`-Wall -Wextra -Wpedantic -Wconversion -Wshadow`, and all existing host
unit tests pass unchanged, plus a new test confirming
`DUALDECK_HOSTCONTROL_STEAM_TOUCHPAD=1` is read without crashing and that
`isTouchpadReady()` correctly reports false in this sandbox (no
`/dev/uinput` here either, same limitation as every other uinput-dependent
test in this file).

**Not yet verified:** real Steam recognition (or lack thereof) of the
Steam-Controller-identified device in an actual Big Picture session on
real hardware; real `systemd --user` activation timing from a cold
Gaming-Mode/Bazzite-HTPC boot; the two-simultaneous-adapter-connections
IPC race under a real persistent daemon.

## 2026-08-02: Found it -- the touchpad only works while holding the STEAM button, because raw touch reporting is gated behind an active Trackpad-type Steam Input binding

Real hardware report, immediately after the previous entry shipped: Host
Control's touchpad-as-mouse genuinely works now (moves the host's virtual
mouse cursor), but only for as long as the STEAM button is held down at
the same time -- confirming touch capture itself is fine, but something
gates *when* it fires.

**Root cause:** this is the same class of problem as an earlier, already-
documented finding for the older mouse-motion path (see the 2026-08-01
"SDL_EVENT_MOUSE_MOTION never fires without a specific Steam Input
binding" entry), just showing up for `SDL_EVENT_GAMEPAD_TOUCHPAD_*`
instead. Steam Input only forwards a trackpad's raw touch data through
that SDL channel while the pad has a **Trackpad-type action bound to it
in the currently active action set/layer** -- it isn't unconditional.
Without a dedicated Controller Layout for the DualDeck Client Steam
shortcut, Steam falls back to whatever generic template it auto-picked
for a freshly-added non-Steam shortcut, and the only place a Trackpad
binding exists in that default is Valve's own built-in "hold STEAM"
layer (normally reserved for the system cursor/on-screen keyboard) --
so touch only ever reports while that chord is held, exactly matching
the report.

**Fix, documented in `docs/troubleshooting.md`'s new touchpad entry
(not a code change -- Steam Input controller-layout assignments live in
a separate, undocumented-format config store this project's own
`scripts/lib/steam_shortcut.py` deliberately doesn't try to script, same
reasoning that file's own module docstring already gives for punting
Controller Layout to a manual step):** give the shortcut its own
Controller Layout, bind both trackpads to the **Trackpad** action type
directly on the base action set (not a hold-modifier layer), matching
`docs/steam-deck-setup.md`'s existing "configured as a mouse in Steam
Input (the default 'Trackpad' binding)" guidance for the unrelated
touchscreen-substitute feature -- same underlying Steam Input mechanism,
different DualDeck feature consuming it.

**Not yet verified:** whether this documented fix actually resolves it
on real hardware -- no physical Steam Deck in this sandbox to confirm
against; the next real-hardware test is the thing to watch for.

## 2026-08-02: Host Control never worked on Bazzite at all -- "Unit dualdeck-host-control.service could not be found"

Real hardware report from a Bazzite HTPC host: starting the new
persistent Host Control daemon from `dualdeck-host.sh`'s menu failed with
`Could not start the Host Control daemon -- see
~/.config/dualdeck/install.log for details, or check whether systemd
--user is available on this system`, and the actual systemd error behind
it was `Unit dualdeck-host-control.service could not be found.`

**Root cause, confirmed by direct code reading, not assumed:**
`host/internal/dualdeck-host-service` (the standalone binary Host
Control mode runs) was packaged as a bare, unbundled binary -- a comment
at its copy step in `build-release.sh` claimed it was "statically
linked... no extra runtime library dependencies," which was simply
wrong: `host/remote-server/CMakeLists.txt` links `TurboJPEG::TurboJPEG`,
found via `find_library()` -- a real dynamic dependency, confirmed with
`ldd` against a freshly built copy (`libturbojpeg.so.0`, `libstdc++.so.6`,
`libgcc_s.so.1`, beyond glibc/the dynamic linker). Because of that
undocumented dependency, both `install-host-distrobox.sh` (the manual
one-off Host Control launch, on immutable systems) and this session's new
`install-host-control-daemon.sh` (the persistent daemon) had each grown
their own outright refusal to run Host Control mode at all on
immutable/rpm-ostree systems like Bazzite -- reasoning that it would need
a Distrobox container to guarantee `libturbojpeg` was present, the same
way melonDS/Azahar/Cemu's own much heavier Qt6/SDL2 dependencies do.
`install-host-control-daemon.sh`'s refusal exits 0 (an intentional,
logged-as-informational "not supported here" early-out, not an ERR-trap
failure) *before* ever writing the systemd unit file -- but
`dualdeck-host.sh`'s calling code didn't check for that, so it silently
fell through to `systemctl --user enable --now
dualdeck-host-control.service` anyway, which failed with "Unit ... could
not be found" because the unit file the daemon refusal had skipped
writing genuinely didn't exist. The generic error message the user saw
never mentioned any of this -- the real, already-known reason
(`install-host-control-daemon.sh` had printed it to stderr) was simply
discarded.

**The actual fix, not just a better error message:** `dualdeck-host-service`
links no Qt6/SDL2 at all -- its only dependency beyond glibc is
`libturbojpeg` (plus `libstdc++`/`libgcc_s`), a dramatically lighter
footprint than any of the three emulators. Bundling those three
libraries alongside it via the exact same `bundle_library_dependencies()`
helper `pack_appimage()` already uses for the prebuilt Azahar/Cemu
AppImages (`scripts/lib/appimage_pack.sh`, already sourced by
`build-release.sh`) makes it fully self-contained, independent of
whether the launching system's package manager can install
`libturbojpeg` at all. With that in place, Host Control mode needs no
Distrobox container on any system:

- `build-release.sh` now bundles `dualdeck-host-service`'s shared library
  dependencies into `host/internal/lib/` right after building it.
- Every place that launches it (`run-host.sh`'s manual Host-Control
  branch, the persistent daemon's `host-control-daemon.sh` wrapper,
  `scripts/lib/adapter_socket_probe.sh`'s private-spawn fallback shared
  by `run-host-azahar.sh`/`run-host-cemu.sh`, and
  `launch-custom-emulator.sh`'s n3ds/wiiu spawns) now sets
  `LD_LIBRARY_PATH` to include that directory.
- `launch-host.sh` (the Steam shortcut's actual entry point) now routes
  `DUALDECK_HOST_CONTROL=1` straight to `run-host.sh` unconditionally,
  even on an immutable system -- Distrobox is reserved for an actual
  melonDS GUI launch, which still genuinely needs it.
- `install-host-distrobox.sh`'s own `DUALDECK_HOST_CONTROL` check now
  delegates to `run-host.sh` instead of erroring, in case anything still
  invokes it directly.
- `install-host-control-daemon.sh` no longer refuses on immutable
  systems at all -- its systemd unit lives under `$HOME` (always
  writable, immutable-OS or not) and now execs a self-contained binary,
  so the only real remaining prerequisite is a working `systemd --user`
  manager, which was already checked separately.
- `dualdeck-host.sh`'s daemon-start menu action now captures and shows
  the actual combined output of the install script and `systemctl`
  instead of a generic message, so any *future* genuine failure is
  self-explanatory instead of repeating this exact confusion.

**Verified:** `ldd` against a freshly built `dualdeck-host-service`
confirms the exact three libraries bundled; `bundle_library_dependencies()`
run against that real binary bundles exactly those three (glibc/the
dynamic linker correctly excluded, matching its existing AppImage-bundling
behavior); every touched script's heredoc body extracted and syntax-checked
clean (`bash -n`); functional sandbox tests with stub binaries confirm
`LD_LIBRARY_PATH` is actually set at each of the four invocation sites,
`launch-host.sh` routes Host Control to `run-host.sh` on both a simulated
immutable and regular system while leaving a plain melonDS launch on the
Distrobox path, and the new daemon-start error-capture logic correctly
surfaces a real refusal's actual text instead of the old generic message
(confirmed under `set -e`, including the classic "command substitution in
a plain assignment defeats set -e" pitfall this fix's `if
var="$(...)"; then` form deliberately avoids).

**Not yet verified:** against real Bazzite hardware -- this is the next
real-world test to confirm; the CI-built binary's glibc/ld.so compatibility
with a real Bazzite host is the same already-accepted risk this project's
prebuilt-AppImage strategy already carries (see the "no longer builds
anything locally" 2026-08-01 entry), not a new one.

## 2026-08-02: Trackpad-as-native-input experiment -- disabling Steam Input for the DualDeck Client shortcut, not a custom Controller Layout

Follow-up to the previous entry's fix (giving the DualDeck Client shortcut
its own Controller Layout with the trackpads bound to "Trackpad"). The
user asked for this to be automated for new users, as frictionless as
possible, and specifically: "the controller layout should function
exactly like a steam controller/steam deck." Before writing a custom
Steam Input Controller Layout -- a reverse-engineered, undocumented
binary/text VDF binding schema this project would have had to guess at,
same caveat as any other undocumented-format experiment in this file --
real research (SDL's own Steam Controller touchpad support PR discussion,
and a real, shipped implementation in RPCS3, a major emulator project)
turned up a simpler, better-precedented fix instead.

**What the research actually found:** Steam Deck's own controller
reports raw touchpad data to SDL's gamepad-touchpad API
(`SDL_EVENT_GAMEPAD_TOUCHPAD_*`, what the client already reads)
*unconditionally* at the HIDAPI/hardware level, independent of any Steam
Input binding -- the earlier entry's "needs a Trackpad-type binding"
diagnosis was aimed at the wrong mechanism. The real gate is whether
Steam Input is intercepting the controller *at all*: while it's active
(the default for any Steam shortcut), it owns the device and only
routes trackpad data through in specific circumstances (matching "only
while STEAM is held"); fully disabling Steam Input for one shortcut
(Properties -> Controller -> "Disable Steam Input") lets SDL read the
controller natively instead, where touch has always been available.
RPCS3 (github.com/RPCS3/rpcs3, PR #18427, "steam: disable steam input
for shortcuts") already automates exactly this for its own generated
non-Steam shortcut -- real, shipped code, not a guess.

**What shipped**, opt-in (a new "trackpad-experiment" entry in
`dualdeck-client.sh`'s menu with a dynamic Enable/Disable label, not
applied automatically during install, per the user's own explicit
choice to ship this as an opt-in experiment rather than a silent
default):

- New `scripts/lib/steam_input_config.py` (client-only -- this is about
  the Deck's own controller, not anything host-side), with its own
  minimal text-VDF (KeyValues) parser/serializer -- `localconfig.vdf` is
  Steam's plain-text format, a different grammar from `shortcuts.vdf`'s
  binary one that `steam_shortcut.py` already parses elsewhere in this
  project. Sets/clears exactly one key,
  `"UserLocalConfigStore"->"Software"->"Valve"->"Steam"->"apps"->
  "<appid>"->"UseSteamControllerConfig"` = `"0"`, computing `<appid>`
  with the byte-for-byte same legacy CRC32 algorithm
  `steam_shortcut.py`'s own `legacy_shortcut_appid()` already uses (so
  it targets the exact same shortcut, confirmed by direct comparison of
  both functions' output for the same input) -- every other key
  anywhere in `localconfig.vdf` is parsed, kept, and re-serialized
  completely untouched.
- Same defensive discipline as `steam_shortcut.py`: refuses to write
  while Steam is running unless `--force` (plugs into the existing
  `run_steam_shortcut_with_restart` auto-restart helper via the same
  "Steam appears to be running" refusal string), backs up the file
  first, and parses its own freshly-written output back as a sanity
  check before trusting it.
- A `--status` mode (read-only, safe regardless of whether Steam is
  running) drives the menu's dynamic label.
- Removing the DualDeck Client from Steam now also best-effort reverts
  this if it was ever turned on, so a stale override doesn't silently
  carry over to whatever gets installed at the same shortcut identity
  next.

**A second, related real user report, same root cause:** the host-control
mouse cursor was also observed capped to "wherever the mouse can move on
the Steam Deck's own screen." Direct code reading
(`client/src/main.cpp`) found two separate code paths both feeding the
same `hostControlMouseDeltaX/Y` accumulator: the raw-touchpad path
(unbounded by design -- finger position on the pad, not tied to any
on-screen cursor) and an older `SDL_EVENT_MOUSE_MOTION` fallback that
reads a real, window/screen-bounded OS cursor's relative motion --
inherently capped once that cursor hits an edge, matching the report
exactly. That fallback only fires today via the same Steam Input "hold
STEAM" mouse-binding mechanism this whole entry is about, so disabling
Steam Input is expected to make the unbounded touchpad path the sole
active source with no code change needed -- flagged here rather than
silently assumed; if it's still capped after testing this fix, the
`SDL_EVENT_MOUSE_MOTION` path warrants a closer look on its own.

**Verified:** `legacy_shortcut_appid()` output compared directly against
`steam_shortcut.py`'s and confirmed identical; the text-VDF parser/
serializer round-trips a realistic multi-app `localconfig.vdf` (including
an escaped-quote value) byte-for-byte when nothing changes, and leaves
every unrelated key untouched when something does; disable/enable/status
tested through their full cycle including idempotency (a repeated
disable or a redundant remove correctly report "nothing to change"); the
Steam-running refusal fires correctly and matches the auto-restart
helper's expected string; the actual `dualdeck-client.sh` menu entry
tested end-to-end against stub scripts and a fake Steam userdata
directory -- selecting it, confirming, writing the real file, the label
updating live, toggling back off restoring the original state, and a
full uninstall cleaning up the override.

**Not yet verified:** against a real Steam Deck -- whether Steam
actually respects this exact key/path the way RPCS3's precedent implies,
and whether the touchpad (and host-control mouse cap) genuinely both
resolve once Steam Input is disabled for the shortcut, is the next real
hardware test to confirm.

### Follow-up, same day: the toggle wasn't reachable from where it actually needed to be

Real user report immediately after shipping the above: "I don't see the
trackpad experiment in the client settings menu." The toggle only
existed in `dualdeck-client.sh`'s outer shell menu -- but
`install-steam-shortcut.sh` points the Steam shortcut's `Exe` straight
at `run-client.sh`, never at that menu script, so Gaming Mode (the only
way most people ever launch this) never shows it at all; the only way
to reach it was double-clicking `dualdeck-client.sh` manually in Desktop
Mode. The user reasonably expected it in the client's own in-app
Settings screen (`client/src/main.cpp`, the one already shipped in the
"Add client Settings menu" PR) instead, which the Steam shortcut always
reaches.

**Fixed** by moving the actual logic behind one new shared script,
`client/internal/configure-trackpad-experiment.sh` (sources
`steam_restart_helper.sh`, wraps `steam_input_config.py --status`/
default-enable/`--remove`), and having BOTH `dualdeck-client.sh`'s menu
and a new Settings-screen item ("TRACKPAD AS NATIVE INPUT
(EXPERIMENTAL): ON/OFF") call that one script rather than duplicating
the logic. The Settings-screen status is deliberately cached, not
queried live inside `settingsMenuItems()` (that lambda runs every frame
while Settings is open, for rendering -- shelling out to a script every
frame would be wasteful/janky); it's refreshed only when Settings is
opened and right after toggling.

**Verified:** the client (including `main.cpp`'s new
`runCaptureStdout()` helper and the Settings-screen wiring) builds
clean with a from-source SDL3 (`-Wall -Wextra -Wpedantic -Wconversion
-Wshadow`, zero warnings) and runs; the shared wrapper script re-tested
end-to-end the same way as before, now going through
`configure-trackpad-experiment.sh` instead of calling
`steam_input_config.py` directly.

### Second follow-up, same day: the Settings-screen toggle silently did nothing at all

Real user report: "it doesn't actually let me toggle it on, and screen
resolution is still limiting mouse movement on the host from the
client" -- the second half is a direct consequence of the first, not a
separate bug: if Steam Input was never actually disabled, the touchpad
path never took over, so the older screen-bound `SDL_EVENT_MOUSE_MOTION`
fallback (see this file's earlier entry on that) kept being the only
thing moving the host cursor.

**Root cause, a real off-by-one-directory path bug:** `main.cpp` shelled
out to `"./internal/configure-trackpad-experiment.sh"`, but
`run-client.sh` already `cd`s into `internal/` before `exec`ing the
binary and never `cd`s back out (confirmed by re-reading `run-client.sh`
directly) -- so the running client's CWD *is* `.../internal/` already,
and that extra `internal/` prefix pointed at a nonexistent
`.../internal/internal/...` path. `popen()` still succeeds (the shell
itself starts fine), but the script it tries to run isn't there, so the
whole toggle silently did nothing -- no error surfaced anywhere, which
is exactly why it looked like a dead button rather than an obvious
failure.

**Fixed** by dropping the incorrect prefix (`"./configure-trackpad-
experiment.sh"`, matching the binary's real CWD) in all three call
sites (`refreshTrackpadExperimentStatus()`, both branches of
`toggleTrackpadExperiment()`).

**Verified**, this time by directly reproducing the bug before fixing
it: a stub script placed at the real path, called via both the old
(`./internal/...`) and new (`./...`) strings from a CWD matching
`run-client.sh`'s actual `internal/` -- confirmed the old string fails
with "No such file or directory" and the new one succeeds. Client
rebuilt clean afterward (same from-source SDL3, zero warnings).

**Not yet verified:** whether the touchpad and host-control mouse cap
now actually resolve on real hardware once the toggle itself works --
still the next real test now that the toggle isn't silently a no-op.

### Third follow-up, same day: enabling it from in-app "seemingly crashe[d] Steam and restart[ed] it"

Real user report, right after the path fix above landed: enabling the
experiment from the Settings screen "seemingly crashes Steam and
restarts it." Not a misread -- `configure-trackpad-experiment.sh`
routes writes through `run_steam_shortcut_with_restart`
(`steam_restart_helper.sh`), which runs `steam -shutdown`, waits for it
to quit, retries the write, then relaunches Steam whenever the target
file is in use. That's the right, deliberate behavior for
`dualdeck-client.sh`'s own standalone menu -- run in Desktop Mode,
before anything is launched, on a controller-only setup with no other
way to reopen a closed Steam (see that helper's own original 2026-07-31
entry). It is *not* the right behavior triggered from inside the
Settings screen: the client is normally itself a Steam-launched process
in Gaming Mode, so killing Steam out from under the game it's actively
running is jarring at best -- and since Steam effectively *is* the
Gaming Mode session on a real Deck, risks tearing down more than just
Steam, which plausibly is what actually looked like a "crash."

**Fixed** with a new `--no-restart` mode on
`configure-trackpad-experiment.sh`: writes with `--force` instead (the
same accepted "Steam's in-memory cache could overwrite this on its next
save" tradeoff `steam_shortcut.py`'s own `--force` already carries, not
a new risk category) and tells the user to restart Steam themselves
whenever's convenient, rather than doing it automatically. `main.cpp`'s
Settings-screen calls now always pass `--no-restart`;
`dualdeck-client.sh`'s own menu doesn't, so its existing auto-restart
convenience is unchanged there.

**Verified:** a fake `steam` binary confirmed it's genuinely never
invoked when `--no-restart` is passed (the file still gets written
correctly even with a simulated "Steam is running" state), and that
omitting the flag still triggers the exact same auto-restart sequence
as before, unchanged, for `dualdeck-client.sh`'s own call. Client
rebuilt clean again afterward.

**Not yet verified:** on real hardware -- whether a manual Steam restart
after this reliably picks up the change, and whether the touchpad and
host-control mouse cap finally resolve once it does.

### Fourth follow-up, same day: the touchpad diagnostics themselves never actually ran

Real user report: "ok it enables, but ... screen resolution is still
limiting mouse movement on the host." A full `client.log` from an
actual connected session on real hardware was requested to check the
2026-08-01 touchpad diagnostic entry's logging (gamepad name,
`SDL_GetNumGamepadTouchpads()` count, per-touchpad finger-slot counts) --
but the log showed only `[input] opened gamepad: Steam Deck Controller`
and nothing else input-related, even though the session fully connected
and ran.

**Root cause, found by re-reading `main.cpp`:** the Deck's own built-in
controller is already present the instant this app starts (unlike a
hot-plugged USB pad), so it's opened by a separate startup path near the
top of `main()` (`SDL_GetGamepads()` + `SDL_OpenGamepad()` directly,
before the event loop even begins). The touchpad diagnostic logging
only ever lived inside the `SDL_EVENT_GAMEPAD_ADDED` event handler
further down, guarded by `if (!gamepad)` -- which never runs, since the
startup path has already opened one by the time that handler could ever
fire. **This diagnostic has never actually collected real data from a
Steam Deck at all** -- the touchpad investigation has been proceeding
on an empty log the whole time, not on "touchpads=0" or any other real
finding.

**Fixed** by factoring the logging into its own function
(`logGamepadTouchpadDiagnostics()`) and calling it from both places a
gamepad can become the active one: the startup path (new) and the
`SDL_EVENT_GAMEPAD_ADDED` handler (unchanged, now just calls the shared
function instead of duplicating the same lines).

**Verified:** client rebuilds clean from source SDL3 again
(`-Wall -Wextra -Wpedantic -Wconversion -Wshadow`, zero warnings).

**Not yet verified:** what the diagnostic actually says on real
hardware -- this is now the real next step; a fresh `client.log` after
this build will, for the first time, actually show whether SDL sees any
touchpads on the Deck's controller at all.

## 2026-08-03: Found the real root cause -- SDL 3.2.x has zero Steam Deck touchpad support at all

The fixed diagnostic above delivered, twice, on real hardware: `[input]
gamepad connected: name=Steam Deck Controller touchpads=0`. Zero, both
times -- once from a Desktop Mode session and once from a proper Gaming
Mode session with client and host on matching versions, ruling out a
Desktop-Mode-specific quirk. This means `SDL_GetNumGamepadTouchpads()`
has genuinely always returned 0 for this exact controller, independent
of every fix attempted so far in this file's touchpad saga (SDL
gamepad-touchpad capture, Steam Input disable, the CWD path bug, the
Steam auto-restart-on-toggle fix) -- none of them could ever have
worked, because SDL itself was never exposing any touchpad data for
this controller to begin with.

**Confirmed by reading SDL's own git history directly**
(github.com/libsdl-org/SDL, full clone, not guessed from memory):
`src/joystick/hidapi/SDL_hidapi_steamdeck.c` -- the file responsible
for translating the Deck's own HID reports into SDL events -- has
**zero** touchpad-related code in `release-3.2.16` (this project's
previously-pinned version) or any other 3.2.x release checked
(3.2.16 through 3.2.30, the last one in that series, all zero). Steam
Controller/Deck touchpad support (PR #15528, "Add Steam Controller
touchpads, capacitive touch for sticks, and grip sense", merged into
SDL's `main` branch, plus several touchpad-specific bugfixes landed
afterward -- "Fix touchpad finger detection on Steam Deck", "Fix Steam
Controller 2 touchpad finger detection", etc.) only reached a tagged
release starting with `release-3.4.0`; `release-3.4.12` (the latest
`3.4.x` release checked) has the complete set of those fixes. This
isn't a config issue, a binding issue, or a client bug -- it's a
version gap in a bundled dependency, and every earlier touchpad
diagnosis in this file, however reasonable given what was knowable at
the time, was chasing symptoms of the same underlying gap.

**Fixed** by bumping `scripts/build-release.sh`'s pinned
`SDL3_TAG` from `release-3.2.16` to `release-3.4.12`.

- **A real new build-time dependency surfaced**: SDL's X11 backend
  configure step now hard-requires the X11 XTEST extension headers
  ("Couldn't find dependency package for XTEST"), which 3.2.16 didn't
  need. Added `libxtst-dev`/`libXtst-devel`/`libxtst` to the `"build"`
  `ensure_packages()` call (apt/dnf/pacman respectively).
- **No new runtime dependency, though**: `ldd` against the freshly
  built `libSDL3.so` for both 3.2.16 and 3.4.12 shows the identical
  dependency set (`libc`, `libm`, the dynamic linker -- nothing else)
  -- SDL `dlopen()`s its actual X11/Wayland/etc. backends at runtime
  rather than linking them directly, so XTEST support being compiled in
  doesn't turn into a hard `libxtst.so.6` requirement on whatever
  machine actually runs the packaged client. No change needed to the
  `"client runtime"` `ensure_packages()` list.

**Verified:** SDL 3.4.12 built from source cleanly in this sandbox
(after adding the new XTEST dev dependency); the full client rebuilt
against it with zero warnings
(`-Wall -Wextra -Wpedantic -Wconversion -Wshadow`) and no source changes
needed anywhere in this project -- SDL3's public API used here is
unchanged between 3.2.16 and 3.4.12; `ldd`-diffed both `libSDL3.so`
builds side by side and confirmed an identical runtime dependency set.

**Not yet verified:** the actual, real thing this whole investigation
has been chasing -- whether the Deck's touchpad now reports through
`SDL_GetNumGamepadTouchpads()`/`SDL_EVENT_GAMEPAD_TOUCHPAD_*` at all
once built against 3.4.12, on real hardware. This is now finally a
question with a concrete, checkable answer via the fixed diagnostic
logging above, rather than another guess.

### Follow-up: 3.4.12 shipped and confirmed via CI (v0.1.116), but touchpads=0 persists

Real hardware, `client.log` from v0.1.116 (confirmed to be the exact
build from this SDL bump -- `release.yml` run #116, commit `74433f5`,
completed successfully in CI): `[input] gamepad connected:
name=Steam Deck Controller touchpads=0`. Still zero. The SDL version
gap was real and worth fixing regardless, but it wasn't the whole
story, or wasn't the story at all -- this needed to be said plainly
rather than declared fixed on the strength of the source-code reasoning
alone once real hardware disagreed.

**Re-reading SDL's actual driver code narrows it further.** SDL
3.4.12's `HIDAPI_DriverSteamDeck_OpenJoystick()` (the function that
would populate touchpad data) calls `SDL_PrivateJoystickAddTouchpad()`
**unconditionally**, twice, with no gating check of any kind -- so if
that function ever actually runs for a given connection, touchpads
cannot be 0. The only way `touchpads=0` is possible with this exact SDL
version is if that function is never reached in the first place, i.e.
SDL is not opening this device through its native Steam Deck HIDAPI
driver at all.

**Leading suspect:** Steam Input's own synthetic "Xbox 360-compatible"
virtual gamepad. When Steam Input is intercepting a controller (the
default for any app, Steam-launched or not, that doesn't request raw
access), it exposes a `uinput`-created virtual device using Microsoft's
well-known Xbox 360 vendor/product ID (`0x045e`/`0x028e`) -- the exact
same convention this project's own `host_control_adapter.cpp` reuses
for its own host-side virtual gamepad, chosen for the identical reason
(broad compatibility with anything expecting a standard gamepad). A
`uinput` device carries no touchpad HID reports by construction (same
architectural ceiling already documented for this project's own Steam
Controller touchpad HID experiment on the host side). If SDL is reading
*that* virtual device instead of the real hardware, `touchpads=0` is
guaranteed regardless of SDL version -- and disabling Steam Input for
just the one DualDeck Client shortcut (this file's earlier entries)
may not be the same thing as SteamOS's system-wide virtual-gamepad
generation being off for the Deck's own built-in controller.

**Diagnostic added, not a fix yet:** `logGamepadTouchpadDiagnostics()`
now also logs the connected gamepad's vendor ID, product ID, and
`SDL_GetGamepadStringForType()` result. Valve's own vendor ID
(`0x28de`) would mean the real hardware driver genuinely opened it (and
`touchpads=0` despite the unconditional-registration code above would
then be a real, different mystery worth its own investigation).
Microsoft's Xbox 360 ID (`0x045e`/`0x028e`) would confirm it's Steam's
synthetic virtual gamepad, redirecting the fix toward SteamOS's
system-wide controller/gamepad-emulation settings rather than anything
in this project's own code.

**Verified:** client rebuilds clean against SDL 3.4.12 with the new
logging (`-Wall -Wextra -Wpedantic -Wconversion -Wshadow`, zero
warnings).

**Not yet verified:** which of the two it actually is -- the next
`client.log` will show real vendor/product IDs for the first time,
turning this from a reasoned guess into a checkable fact.

### Resolved: real hardware confirms the touchpads now work

Real user report, 2026-08-03: "the touchpads work perfectly now." The
SDL 3.2.16 -> 3.4.12 bump above was the actual, complete fix -- once a
real client.log was captured against a build containing both the
version bump and the vendor/product diagnostic together (rather than the
intermediate v0.1.116 build above, which only had the version bump), the
touchpads worked. The vendor/product diagnostic added in the follow-up
above was never needed to explain a persisting failure, since there
turned out not to be one. This closes out the whole touchpad
investigation chain starting from the original "touchpads don't function
like a Steam Controller on the host" report.

## 2026-08-03: Host Control's mouse cursor screen-cap -- the actual, direct fix

Real user demand, separate from the touchpad-vs-SDL-version investigation
above: "it is still limited by resolution, we need a real answer and
real solution." Right to push back on more diagnosis -- this one has a
direct, well-understood fix that doesn't depend on resolving the
touchpad mystery at all.

**The real cause, stated plainly:** `SDL_EVENT_MOUSE_MOTION`'s
`xrel`/`yrel` (what `hostControlMouseDeltaX/Y` accumulates) come from a
*real* OS cursor's position delta. That cursor is confined to this
client's own window/the Deck's screen like any normal cursor -- once it
physically reaches an edge, it cannot move further, so no amount of
continued physical input past that point produces any more motion. This
is true regardless of what device is driving the cursor (the Deck's own
trackpad already does this today, independent of the whole
`SDL_EVENT_GAMEPAD_TOUCHPAD_*`/Steam Input investigation above -- a
touchpad configured as a mouse always drives a real, boundable OS
cursor) and independent of SDL version.

**The fix:** `SDL_SetWindowRelativeMouseMode()`, an SDL feature built
for exactly this case -- its own header comment: "an FPS wouldn't want
the player's motion to stop as the mouse hits the edge of the window."
Enabling it hides the OS cursor, grabs mouse input to the window, and
keeps reporting relative motion deltas indefinitely regardless of screen
edges. `main.cpp` now toggles this on whenever `net.hostMode() ==
HostMode::HostControl` and connected, off otherwise -- Emulation mode's
DS-touchscreen-via-mouse-drag feature (`mapPointToDSCoords`) still needs
the real absolute cursor position, so relative mode would break that if
left on outside Host Control.

**A real correctness detail caught before shipping:** `SDL_mouse.h`
documents that `SDL_SetWindowRelativeMouseMode()` "will flush any
pending mouse motion for this window" *every time it's called* -- an
early version of this fix called it unconditionally every frame based
on current state, which would have flushed real in-flight motion
constantly and made input feel jittery. Fixed by tracking the
previously-applied state (`wasRelativeMouseMode`) and only calling the
SDL function on an actual transition, matching this loop's existing
`lastHostMode` transition-tracking pattern.

**Verified:** client rebuilds clean (`-Wall -Wextra -Wpedantic
-Wconversion -Wshadow`, zero warnings) against SDL 3.4.12.

**Not yet verified:** on real hardware -- whether host-control mouse
movement is now genuinely unbounded is the next thing to confirm; this
fix does not depend on or wait for the separate touchpad-vs-synthetic-
gamepad question above, so it's expected to work regardless of how that
one resolves.

## 2026-08-03: client-triggers-host-update on a version mismatch

Real user request, planned ahead of any specific failure: "if the client
connects to the host and the host is on an older version, it should try
to trigger an update if possible." Before this, `HelloRejectReason::
AppVersionMismatch` just told the user to update one side manually --
verified via direct codebase grep that no auto-update trigger existed
anywhere before implementing this (the user later reported "the client
telling the host to update is not working," which was correct: it had
never been built, not a bug).

**Trust model, the user's own explicit choice** (via `AskUserQuestion`,
not assumed): auto-trigger only for a device identity already in the
host's approved-devices set (`DeviceApprovalManager`), never for any
connecting client. The version-mismatch check happens *before*
authentication/approval in `net_server.cpp`'s Hello handling (on
purpose -- a stale client should never learn whether its stale
credentials would have worked), so this needed its own read-only lookup
rather than reusing the existing `check()`: `DeviceApprovalManager::
isApproved()` (device_approval_manager.h/.cpp) looks up the approved set
without the side effect `check()` has of registering/refreshing a
pending-approval entry for an unrecognized id -- a client whose version
doesn't even match yet has no business cluttering the pending-approval
queue with an entry a human can't act on until versions match anyway.

**Design:**
- New `HelloRejectReason::AppVersionMismatchUpdateTriggered = 6` (purely
  additive to the wire format -- `HelloRejectReason` is a plain
  `uint8_t`, see `protocol.cpp`'s serialize/parse -- no
  `kProtocolVersion` bump needed). Client shows a distinct "HOST IS
  UPDATING ITSELF - RETRYING..." message instead of the plain
  AppVersionMismatch "update one side to match the other" message.
- New `NetServerConfig::selfUpdateCommand` (net_server.h): empty by
  default (disables the feature entirely). Only makes sense for a
  standalone `dualdeck-host-service` process that can cleanly restart
  itself afterward (the persistent Host Control daemon under
  `systemd --user`, `Restart=on-failure`) -- **never** for melonDS's
  in-process integration, which has no way to restart itself mid-
  emulation without losing the user's game. `main.cpp` only wires this
  (to `<host_root>/internal/apply-update.sh`, which already restarts the
  persistent daemon if it was active) when a new `--self-update` CLI
  flag is passed; `scripts/build-release.sh`'s generated
  `host-control-daemon.sh` passes it, nothing else does.
- Gated additionally on `config_.authToken.empty()` -- static-token
  deployments never populate `DeviceApprovalManager`'s approved set at
  all (there, `hello->authToken` means "shared secret", not "persistent
  device identity"), so the feature is a structural no-op in that mode
  even if `selfUpdateCommand` were set by mistake.
- Fire-and-forget via `runSelfUpdateCommand()` (`std::system("nohup " +
  command + " >/dev/null 2>&1 &")`) so a potentially ~180-second update
  download never blocks the Hello/HelloAck response.
- `selfUpdateTriggered_` (atomic, deliberately never reset) ensures the
  update command runs at most once per host process lifetime -- a
  client retrying every few seconds while the update is already
  downloading must not spawn a second concurrent update. A side effect
  worth knowing: once triggered, *subsequent* Hello attempts from the
  same approved device during the update window see plain
  `AppVersionMismatch`, not another `AppVersionMismatchUpdateTriggered`
  -- slightly less precise messaging on retries 2+, not a bug (the
  client's auto-reconnect loop keeps retrying regardless of which of the
  two reasons it sees, and the daemon comes back on its own once the
  update finishes).

**A real bug the new tests caught before shipping:** the live
`protocol.cpp`'s `parseHelloAckPayload()` had its own separate
validation cap -- `if (reason > static_cast<uint8_t>(HelloRejectReason::
AppVersionMismatch)) return std::nullopt;` -- left over from before this
change and never updated when the new enum value was added. Any client
parsing a real `AppVersionMismatchUpdateTriggered` HelloAck would have
seen it as a malformed packet and dropped the entire connection instead
of showing the new message. Caught by
`test_self_update_trigger.cpp`'s real end-to-end test (a real `NetServer`
on real loopback sockets, a real raw-socket Hello, asserting on the
parsed `HelloAck`) failing with the client-side handshake helper itself
returning false, not a wrong-value assertion -- exactly the class of bug
a "does the field decode to the right enum value" unit test alone would
have missed, since the packet never parsed as anything at all. Fixed by
raising the cap to `AppVersionMismatchUpdateTriggered`.

**Frozen/vendored patch copies checked, no regeneration needed:**
`host/melonds-patches/0001-remote-server-integration.patch` vendors a
full standalone copy of `net_server.cpp`/`net_server.h` (melonDS's
in-process integration builds its own disconnected snapshot, confirmed
via direct grep) -- but `selfUpdateCommand` is designed to always stay
empty for melonDS's in-process path (see above), so the vendored copy
never needing the new trigger logic is by design, not an oversight.
`host/azahar-patches/` and `host/cemu-patches/` only vendor `protocol.h`/
`protocol.cpp` (for adapter-IPC contract types) plus `ipc_protocol.h` --
confirmed via grep that their vendored `parseHelloAckPayload` is dead
code in both (only `serializeHelloAckPayload` is ever called, by the
adapter-contract layer; the actual client-host Hello/HelloAck exchange
for Azahar/Cemu happens in the separately-built, always-current
`dualdeck-host-service` binary they connect to over adapter IPC, not in
anything vendored into the patch). `scripts/check-patch-protocol-sync.sh`
only compares `kProtocolVersion` numbers between the live header and
each patch's embedded copy, which this purely-additive enum change
doesn't affect either way -- it would not have caught (and did not need
to catch) any of the above.

**Verified:** new `test_self_update_trigger.cpp` (host_tests) -- real
`NetServer`/`DeviceApprovalManager` end-to-end, no mocks: an
already-approved device with a mismatched `appVersion` gets
`AppVersionMismatchUpdateTriggered` and the configured
`selfUpdateCommand` actually runs (observed via a marker file, since the
trigger is intentionally fire-and-forget with no other completion
signal); an *unapproved* device with the same mismatch gets plain
`AppVersionMismatch` and never triggers the command (the security
boundary the user explicitly chose); the update fires at most once per
process lifetime; static-auth-token mode never triggers it at all. Full
host (`dualdeck_host`, `dualdeck-host-service`,
`melonds_remote_host_tests` -- 88 cases, 0 failures) and protocol
(`dualdeck_protocol_tests` -- 104 cases, 0 failures) test suites rebuilt
and passing; client (`dualdeck-client`, including both new
`HelloRejectReason::AppVersionMismatchUpdateTriggered` switch cases)
rebuilds clean against SDL 3.4.12 with `-Wall -Wextra -Wpedantic
-Wconversion -Wshadow`, zero warnings.

**Not yet verified:** on real hardware -- an actual host running an
older version, an approved Deck client connecting, and confirming the
persistent daemon actually re-downloads, restarts, and comes back
reachable at the new version. This is the next thing to test once
another release is available to update *to*.

## 2026-08-03: Cemu Vulkan renderer init failure on Bazzite, works fine on a Fedora laptop

Real user report: "Error when initializing Vulkan Renderer on Cemu on
bazzite, works fine on Fedora Laptop." Same Cemu binary, same wire
protocol -- the only variable that changes between the two reports is
*how* Cemu gets launched: on the Bazzite HTPC, DualDeck's whole design
is Steam-shortcut-first (`install-steam-shortcut.sh --exe
dualdeck-host.sh`), so Cemu is a grandchild of a process Steam itself
launched; a Fedora laptop test is far more likely to have run Cemu (or
this whole flow) directly, outside Steam.

**Root cause, same bug class already found and fixed once in this file
(2026-08-01, melonDS's Distrobox launch path):** `dualdeck-host.sh` is
the literal `--exe` target of the host's Steam shortcut, so every
process it goes on to `exec` -- `launch-host.sh` (melonDS/Host Control),
`run-host-azahar.sh` (3DS), `run-host-cemu.sh` (Wii U/Cemu),
`launch-custom-emulator.sh` -- inherits Steam's own environment
unmodified, including `LD_PRELOAD` pointing at Steam's overlay-injection
libraries (`gameoverlayrenderer.so`). That library hooks Vulkan's
`vkCreateInstance`/`vkCreateDevice` to draw Steam's own in-game overlay
-- a well-known source of Vulkan initialization failures in native Linux
Vulkan apps launched through Steam. `install-host-distrobox.sh` already
had its own `unset LD_PRELOAD LD_LIBRARY_PATH` for exactly this reason
(it broke melonDS's `libGL.so.1` loading outright, same LD_PRELOAD
contamination, different graphics API), but that fix only covered
melonDS's immutable-system Distrobox path -- Cemu's native (no
Distrobox) launch path never got it, and neither did any of the other
launch paths reachable from `dualdeck-host.sh`.

**Fixed** by moving the `unset LD_PRELOAD LD_LIBRARY_PATH` to the true
root of the process tree: the top of `dualdeck-host.sh` itself, right
after its `cd`, before anything else runs. Every launch path below it
(`ds`/`n3ds`/`wiiu`/`hostcontrol`/`custom` in `choose_emulator()`)
inherits the cleaned environment automatically, with no per-script
duplication needed. `install-host-distrobox.sh` keeps its own copy of
the same `unset` too -- redundant when reached through
`dualdeck-host.sh` now, but still a correct, self-contained safety net
for anyone invoking it directly.

**Verified:** `bash -n` on the generated script content confirms no
syntax breakage from the added heredoc lines (the heredoc delimiter is
quoted, so no shell expansion of the explanatory comment's `$()` text
happens inside it).

**Not yet verified:** on real Bazzite hardware -- whether Cemu's Vulkan
renderer actually initializes successfully now that Steam's LD_PRELOAD
no longer reaches it. This is a strong, precedented root cause (the
identical failure mode already confirmed and fixed once for melonDS in
this exact codebase), not a guess, but real-hardware confirmation is the
next step once a build with this fix is available to test.

## 2026-08-03: Host Control screen-mirror experiment (X11 only, opt-in)

Real user request, while diagnosing the Cemu Vulkan issue above without
access to the TV the Bazzite host is normally connected to: "I want to
add an option to the client's host control to mirror the screen... if we
can bake that in as well." Host Control mode has never sent any video
(`HostControlAdapter::getLatestFrame()` was hard-coded to `return
false`, with a comment stating "there is no emulated screen while no
emulator is running") -- true when the mode only meant "navigate the
host's own Big Picture/desktop via a virtual gamepad," but not the whole
story once the actual ask became "let me see the host's screen." Scoped
per the user's own explicit choice (`AskUserQuestion`): X11 only, not
Wayland, and a "quick and good enough" (~5fps) capture rather than
investing in a smoother pipeline right away.

**Design**, entirely additive, zero protocol changes:
- **Host side** (`host/remote-server/include/host/x11_screen_capture.h`
  + `.cpp`, new files): a small, PIMPL'd `X11ScreenCapture` class using
  Xlib + the XShm extension to grab the root window's pixels. PIMPL'd
  specifically so the header stays includable, and the class safely
  inert, even in a build with no X11 dev headers at all --
  `host/remote-server/CMakeLists.txt` does `find_package(X11)` (not
  `REQUIRED` -- unlike TurboJPEG, this is a narrow opt-in feature, not
  something every host build needs) and only defines
  `DUALDECK_HAVE_X11_SCREEN_CAPTURE` when both X11 and Xext are found;
  the .cpp file compiles a real implementation or an always-not-ready
  stub depending on that macro, and it's unconditionally added to
  `dualdeck_host`'s sources either way so `host_control_adapter.cpp`
  never needs its own separate guard.
- **`HostControlAdapter`** (host_control_adapter.h/.cpp): new opt-in env
  var `DUALDECK_HOSTCONTROL_MIRROR_SCREEN` (same read-once-in-
  constructor style as the existing `DUALDECK_HOSTCONTROL_STEAM_TOUCHPAD`
  experiment), default off -- `X11ScreenCapture` is only ever
  constructed (and only then attempts an X11 connection at all) when
  this is set. `getLatestFrame()` now actually captures, internally
  rate-limited to ~5fps (`DUALDECK_HOSTCONTROL_MIRROR_FPS`, optional,
  1-30, same clamp-and-fall-back-to-default style as other capture-fps
  env vars in this project) rather than on every call -- `NetServer::
  videoLoop()` polls this at up to `videoSendFps` (default 60), and a
  full-resolution desktop capture is real work not worth repeating 60
  times a second for a feature whose whole point is periodic menu/setup
  visibility, not smooth gameplay video. `frameDimensions()` is now
  overridden to report the real captured resolution (essentially never
  DS's fixed 256x192 default).
- **No new wire protocol message needed at all**: the host already sends
  real captured frames as ordinary `VideoFrame` packets over the exact
  same video socket/JPEG-compression path Emulation mode always has
  (`net_server.cpp`'s `videoLoop()`/`compressFrameBgraToJpeg()` needed
  zero changes) -- Host Control mode was never "video-incapable" at the
  wire level, just never had anything to send.
- **Client side** (`client/src/client_settings.h/.cpp`, `main.cpp`): new
  persisted `ClientSettings::mirrorHostScreen` (default off, same
  opt-in-experimental convention as the trackpad experiment), a new
  "MIRROR HOST SCREEN (EXPERIMENTAL)" Settings-menu toggle (both
  keyboard and gamepad handlers, matching every other toggle there). The
  render loop's existing `if (nowConnected && nowHostMode ==
  HostMode::HostControl) { renderHostControlScreen(...); continue; }`
  branch now only takes that early-exit when the toggle is off --
  turning it on just lets the loop fall through to the exact same
  video-decode/render path Emulation mode already uses, no new
  client-side rendering logic needed. If the host isn't actually
  mirroring (env var unset there, or no usable X11 display), this
  degrades to showing the built-in test-pattern texture instead of real
  video -- harmless, if not especially informative; a known rough edge
  of this first cut, not a crash or hang.

**How to actually turn this on** (no GUI toggle on the host side yet,
same as the touchpad experiment): export `DUALDECK_HOSTCONTROL_MIRROR_SCREEN=1`
in whatever environment starts the host process -- before running
`./dualdeck-host.sh`/`./internal/run-host.sh` directly, or via
`systemctl --user edit dualdeck-host-control.service` to add an
`Environment=DUALDECK_HOSTCONTROL_MIRROR_SCREEN=1` line if using the
persistent daemon.

**Explicit, deliberate scope limits, not oversights:**
- **X11 only.** Capturing an arbitrary Wayland compositor's output needs
  either the wlr-screencopy protocol or the xdg-desktop-portal
  ScreenCast/PipeWire API -- both substantially more code, and neither
  verifiable without real Wayland hardware in hand (this session's
  sandbox has no Wayland compositor to test against, only Xvfb). On a
  Wayland-only session (no XWayland reachable either), `isMirrorReady()`
  stays false and the feature is a clean no-op with a logged reason, not
  a crash or a silent wrong-looking capture.
- **Standard 24/32-bit TrueColor visual only.** `X11ScreenCapture`
  checks the actual `red_mask`/`green_mask`/`blue_mask`/`bits_per_pixel`
  values from the real XShm image rather than assuming them, and
  disables itself (logged) rather than emitting corrupted color if some
  genuinely unusual visual is in use.
- **No resolution capping/downscaling.** A 4K host screen captures and
  streams at full native resolution -- likely slow/bandwidth-heavy
  (`defaultVideoQualityForFrameSize()` already lowers JPEG quality for
  large surfaces, tuned against Cemu's 854x480 GamePad output, not
  necessarily against a full desktop's resolution) -- a real
  future-improvement candidate, not attempted in this first cut.

**Verified:**
- Real end-to-end capture against an actual X server (Xvfb, not just a
  compile check): correct captured dimensions, correct raw byte count
  (`width * height * 4`), and -- critically, not merely assumed --
  correct BGRA8888 channel ordering confirmed by drawing a known pure-red
  rectangle onto the root window and checking the captured bytes exactly
  match (`B=0x00 G=0x00 R=0xFF`) via a small standalone smoke-test
  program built directly against the real `host_control_adapter.cpp`/
  `x11_screen_capture.cpp` sources.
- Graceful degradation verified two ways: the default (env var unset)
  path, and the opt-in-but-no-display path (`DISPLAY` explicitly
  unset), both confirmed to return `false`/stay inert rather than crash
  -- new `host_control_adapter_mirror_disabled_by_default_returns_no_frame`
  and `host_control_adapter_mirror_opt_in_degrades_gracefully_without_x11_display`
  tests (host_tests).
- Full host (`dualdeck_host`, `dualdeck-host-service`,
  `melonds_remote_host_tests` -- 90 cases, 0 failures, including the two
  new mirror tests) and protocol (`dualdeck_protocol_tests` -- 104
  cases, 0 failures) suites rebuilt and passing, with `find_package(X11)`
  actually succeeding and the real (non-stub) capture code compiling in
  this sandbox's own environment. Client (`dualdeck-client`,
  `melonds_remote_client_settings_tests` -- including two new
  `mirrorHostScreen` round-trip tests) rebuilds clean against SDL 3.4.12
  with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`, zero warnings.

**Not yet verified:** on real Bazzite hardware -- whether the actual
desktop session there is X11 or Wayland (unknown as of this entry; the
user had not yet confirmed `$XDG_SESSION_TYPE` when this was
implemented), and if X11, whether the mirrored image is visibly usable
end-to-end (client Settings toggle on, host env var set) for the
original motivating purpose: seeing Cemu's own graphics-settings screen
without physical access to the connected TV.

## 2026-08-03: Cemu "nightly build" report -- real bug found, but not the one guessed

Real user report: "EmuDeck Installs the latest stable version of Cemu
(2.6 in this case). DualDeck Compiles a nightly build (a6fb0a4 in this
case)." A concrete, checkable hypothesis, investigated against Cemu's
real upstream git history rather than assumed either way (cloned
github.com/cemu-project/Cemu directly, fetched all tags):
`a6fb0a48eb437a8a41c13b782ac8ae0433bf8f98` (`scripts/lib/pinned_commits.sh`'s
`CEMU_COMMIT`, what `scripts/build-release.sh`'s real release pipeline
actually builds) **is** exactly and only tag `v2.6` -- `git rev-parse
v2.6` resolves to that identical hash. The "DualDeck builds a nightly"
hypothesis, taken literally (wrong commit), is false.

**What's actually true, and a real bug regardless:** Cemu's own
`src/Common/version.h` only shows a real version string ("2.6") when
the build passes `-DEMULATOR_VERSION_MAJOR`/`-DEMULATOR_VERSION_MINOR`
as CMake cache variables -- its own comment states plainly: "no version
provided. Only show commit hash." `scripts/lib/build_emulator.sh`'s
`build_cemu()` never passed these at all, for either
`scripts/build-release.sh`'s official release build or
`scripts/patch-existing-emulator.sh`'s local-iteration build. The
result: a build of the genuine v2.6 tag still self-identifies in
Cemu's own UI as its bare commit hash (`a6fb0a4`) -- exactly what made
a real stable release look, quite reasonably, like a nightly build to
the user. Confirmed by reading Cemu's own official release CI
(`.github/workflows/build.yml`/`determine_release_version.yml`), which
does pass these flags for every build including the real tagged
releases.

**Fixed:** `pinned_commits.sh` gained `CEMU_VERSION_MAJOR="2"`/
`CEMU_VERSION_MINOR="6"` (kept in sync with `CEMU_COMMIT` by hand each
time it's bumped -- there's no way to derive them automatically without
also depending on Cemu's own release-tag-naming convention staying
`vMAJOR.MINOR` forever, which felt like the wrong thing to build
against). `build_cemu()` gained optional `version_major`/
`version_minor` parameters, passed through as the CMake flags Cemu
needs. `build-release.sh`'s call site now passes them.

**A second, independent, real bug found while investigating:**
`scripts/patch-existing-emulator.sh` used to keep its own separate copy
of `MELONDS_COMMIT`/`AZAHAR_COMMIT`/`CEMU_COMMIT` rather than sourcing
`pinned_commits.sh` (whose own header comment already warned this exact
scenario was the reason it was factored out in the first place -- "a
second script... can't silently drift onto a different commit"). Its
melonDS/Azahar copies had stayed in sync by luck; its Cemu copy had
drifted onto `50b9e4ba1d4d7cf9821a9cd416378bb94e1ba0ca`, an untagged
commit dated months after v2.6 -- a **genuine** nightly/dev commit,
confirmed via the same upstream-clone check (no tag points at it, and
it postdates v2.6's commit by over five months). Anyone using "patch my
own existing emulator" for Cemu really was getting a nightly build,
just not through the path the user actually hit.

**Fixed, and directly implementing the user's own stated policy**
("make sure DualDeck only uses the latest stable version, unless
specified by the user during the patching"): `patch-existing-emulator.sh`
now `source`s `pinned_commits.sh` instead of keeping its own copy
(eliminating this whole class of drift structurally, not just this one
instance of it), and gained a new `--commit <sha>` flag -- the explicit,
opt-in way to patch against something other than the pinned stable
default on purpose. When `--commit` is used for Cemu, the
`EMULATOR_VERSION_MAJOR`/`MINOR` passthrough is deliberately skipped
(an arbitrary user-chosen commit doesn't necessarily correspond to any
real released version number -- Cemu's own honest commit-hash fallback
is more correct than a guessed version string would be).

**Still an open, undecided question -- flagged, not resolved:** whether
`VkApplicationInfo::applicationVersion` being `VK_MAKE_VERSION(0,0,0)`
(what the unfixed build sent to the GPU driver) versus `(2,6,0)` (what
it sends now) is actually related to the user's separately-reported
Vulkan renderer/graphics-device issue. Some GPU vendor drivers key
app-specific compatibility profiles off exactly this field, which makes
it a real, plausible mechanism -- but nothing here proves it's *the*
cause, and it doesn't explain "no graphics device recognized" or OpenGL
crashing outright by itself. Worth fixing regardless of whether it
turns out to be the answer; not claimed as a confirmed fix for that
issue.

**Verified:** live-tested against a real clone of the pinned Cemu
commit (not just read the diff) -- `patch-existing-emulator.sh --system
wiiu --source <real Cemu v2.6 checkout>` applies cleanly and prints a
build command containing `-DEMULATOR_VERSION_MAJOR=2
-DEMULATOR_VERSION_MINOR=6`; the same command with `--commit
<arbitrary-sha>` correctly omits those flags and prints the expected
commit-mismatch warning. `bash -n` clean on every modified script.

**Not yet verified:** whether a Cemu build with the correct version
flags actually changes anything observable about the Vulkan/graphics-
device issue on real Bazzite/Fedora hardware -- the next release build
will carry this fix, but it should not be assumed to be the whole
answer to that separate, still-open report.

## 2026-08-03: Host Control screen mirror showed solid grey on real hardware -- confirmed Wayland-vs-X11, not a new bug

Real user report, testing the X11-only screen-mirror experiment above
on both machines: "the Screen mirroring is just grey, nothing on either
fedora or bazzite," followed by confirmation both sessions are Wayland.
This matches -- exactly, not approximately -- the gap that feature's
own header comment already named as a known, deliberate first-cut
limitation, not a new mystery: `X11ScreenCapture` opens a display via
plain `XOpenDisplay(nullptr)`. On a Wayland session where XWayland
happens to be running (common -- many Wayland compositors auto-start it
for X11-app compatibility and set `DISPLAY` accordingly), this call
*succeeds* -- so `isMirrorReady()` comes back true and capture proceeds
-- but it's connecting to XWayland's own compositing root window, which
carries none of the real Wayland desktop's actual content. A uniform
grey (XWayland's typical empty-root color) is exactly what capturing
"nothing" through that path looks like -- not a crash, not the
test-pattern fallback (which would look different and would mean
`isMirrorReady()` was false), but a technically-successful capture of
an essentially blank compositing layer.

**Why this isn't a quick fix:** the only broadly-compatible way to
capture a real Wayland compositor's actual output across both of the
user's actual environments (Fedora -- almost certainly GNOME/Mutter;
Bazzite -- KDE Plasma/KWin, or gamescope specifically in Gaming Mode)
is the `xdg-desktop-portal` `ScreenCast` interface over PipeWire --
DBus session negotiation, a one-time interactive permission grant, and
real PipeWire stream/buffer-format negotiation code. The simpler
`wlr-screencopy-unstable-v1` protocol (no permission dialog, much less
code) only exists on wlroots-based compositors (Sway, Hyprland,
gamescope) -- it does **not** work on KWin or Mutter, i.e. not on either
of the user's actual desktop sessions as tested (though it would work
for Bazzite's Gaming-Mode/gamescope session specifically, arguably
Host Control's real primary use case). Neither PipeWire, a portal
backend, nor any wlroots compositor is available in this sandbox to
build and verify against the way the X11 path was verified end-to-end
against a real Xvfb server -- shipping a from-scratch PipeWire
integration unverified would repeat the exact mistake this file's
touchpad-investigation entries spent several rounds correcting
(declaring something fixed on reasoning alone, without real-environment
confirmation).

**Not yet fixed as of this entry.** Real Wayland capture (portal +
PipeWire) is substantial, unbudgeted engineering -- this project's own
earlier planning notes already flagged Wayland screen capture as likely
the single largest piece of work in the whole Host-Control-video
effort, before this feature request even existed. Decision on how to
proceed deferred to the user (asked directly, not assumed) given the
real verification-risk tradeoff of building it blind versus
deprioritizing it in favor of a more direct, mirror-independent path to
the actual blocking issue (getting Cemu's own terminal/stderr output,
which needs no video at all). User chose to build it now and iterate on
real hardware -- see the follow-up entry immediately below.

### Follow-up: Wayland portal + PipeWire capture implemented, verified as far as this sandbox allows

New `WaylandScreenCapture` (host/remote-server/{include/host,src}/wayland_screen_capture.h/.cpp),
same PIMPL/degrade-gracefully shape as `X11ScreenCapture`. `HostControlAdapter`
now tries X11 first (cheap, and genuinely correct on a real X11 desktop
session), falling back to this only if X11 didn't work -- avoiding an
unconditional portal session (and its one-time interactive permission
prompt, see below) on hosts where X11 already works fine.

**Design**, following the xdg-desktop-portal spec directly: `CreateSession`
-> `SelectSources` (monitor sources only) -> `Start` (the call that
shows the interactive one-time permission dialog *on the host's own
screen* -- a real GNOME/KDE system dialog, not anything this project
draws; this is the portal's actual security boundary working as
designed) -> `OpenPipeWireRemote` (returns a session-scoped, pre-
authorized file descriptor -- connecting to the ambient/default local
PipeWire socket instead would not be authorized to see this stream at
all) -> `pw_context_connect_fd()` with that fd -> a `pw_stream`
negotiating a raw video format, running on its own persistent
background thread (PipeWire delivers frames via callback, unlike
X11ScreenCapture's synchronous per-call `XShmGetImage`) that keeps a
mutex-protected "latest frame" slot `capture()` just reads out.
Persisted consent (`persist_mode`/`restore_token`) was deliberately
**not** implemented in this first cut -- every process launch re-prompts
for permission; a real, worthwhile future improvement, scoped out
rather than half-built.

**Real verification undertaken, more than the usual "compiles clean"
given this class's real-world stakes** -- this sandbox has no
compositor/portal/PipeWire service by default, so packages were
installed and a real environment assembled specifically to test against
(`pipewire`, `wireplumber`, `xdg-desktop-portal`, `xdg-desktop-portal-wlr`,
`sway`, `gstreamer1.0-pipewire`, all genuinely available via apt, not
assumed):
- **PipeWire consumption -- fully verified end to end, not just
  compiled.** A real PipeWire daemon + real session bus were started,
  and a real video-producing PipeWire node created via `gst-launch-1.0
  videotestsrc pattern=red ! pipewiresink mode=provide` (confirmed via
  `pw-dump` as a genuine `Stream/Output/Video` node). A minimal test
  program built directly against `WaylandScreenCapture`'s exact
  `pw_stream`/SPA-format-negotiation code connected to that real node,
  negotiated a real format (`64x48`, `SPA_VIDEO_FORMAT_BGRx`), received
  three real frames, and read back the pixel data: `B=0x00 G=0x00
  R=0xff` -- exactly matching the drawn red test pattern, in the
  correct byte order this project's BGRA8888 convention expects. This
  is the same category of proof the X11 path got (a known color drawn,
  then confirmed byte-for-byte in the captured output), just via a
  PipeWire producer instead of an X11 root-window fill.
- **Portal D-Bus session flow -- implemented per the documented spec,
  compiles clean under this project's full `-Wall -Wextra -Wpedantic
  -Wconversion -Wshadow` warning set (verified directly, including
  against the real `dbus-1`/`libpipewire-0.3` headers with
  `SYSTEM`-include treatment so their own GNU-extension macros don't
  drown out real warnings from this project's own code), but **not**
  driven through a complete live `CreateSession` -> `Start` round trip
  against a running `xdg-desktop-portal` + backend in this sandbox --
  attempts to run compositor/portal daemons as genuinely persistent
  background processes hit this sandbox's own job-control/backgrounding
  behavior (commands meant to run indefinitely were treated as timed-out
  failures), not a problem with the portal software itself or this
  project's D-Bus code. Real, honest gap, not glossed over: the
  `CreateSession`/`SelectSources`/`Start`/`OpenPipeWireRemote` message
  construction and Response-signal handling is implemented correctly
  per the spec's documented method signatures and object-path/signal
  conventions, but has not been observed actually completing against a
  live portal backend.
- Full host (`dualdeck_host`, `dualdeck-host-service`,
  `melonds_remote_host_tests` -- 90 cases, 0 failures) rebuilds clean
  with both `DUALDECK_HAVE_X11_SCREEN_CAPTURE` and
  `DUALDECK_HAVE_WAYLAND_SCREEN_CAPTURE` active (both `find_package`/
  `pkg_check_modules` calls succeeded in this sandbox once the new dev
  packages -- `libdbus-1-dev`/`libpipewire-0.3-dev` (apt), `dbus-devel`/
  `pipewire-devel` (dnf), `dbus`/`libpipewire` (pacman), added to
  `scripts/build-release.sh`'s `"build"` `ensure_packages()` list -- were
  installed); `ldd` on the built `dualdeck-host-service` confirms it now
  links `libdbus-1.so.3`/`libpipewire-0.3.so.0` alongside the X11 libs,
  which the existing generic `bundle_library_dependencies()` call
  (already fixing the Bazzite Host Control libturbojpeg bug, see its own
  2026-08-02 entry) picks up and bundles automatically -- no separate
  packaging-script changes needed for runtime availability.
- The new degrade-gracefully test
  (`host_control_adapter_mirror_opt_in_degrades_gracefully_without_any_backend`)
  is deliberately more defensive than the original X11-only version:
  it clears `DISPLAY`, `DBUS_SESSION_BUS_ADDRESS`, *and* redirects
  `XDG_RUNTIME_DIR` to a fresh empty directory, since libdbus's
  systemd-style `$XDG_RUNTIME_DIR/bus` autodetection runs independently
  of `DBUS_SESSION_BUS_ADDRESS` -- a real fallback path on any
  systemd-based desktop, i.e. plausibly whatever machine actually runs
  this test suite. Without that, this unit test could pop a genuine
  interactive system permission dialog on a developer's real desktop
  just from running `ctest` -- caught and fixed before it could ever do
  that to anyone, not discovered the hard way.

**Not yet verified:** on real Bazzite/Fedora hardware -- whether the
full portal permission flow actually completes and produces a real
mirrored image end to end. Given both machines are confirmed Wayland,
this is the actual, sole remaining gap before the original motivating
use case (seeing Cemu's own graphics-settings screen without physical
access to the connected TV) can work.

## 2026-08-03: "still using Cemu a6fb0a4, not 2.6" after the version-flag fix -- a stale CI cache, not the fix being wrong

Real user report, after updating to the release containing the
`EMULATOR_VERSION_MAJOR`/`MINOR` fix above: "after uninstalling and
reinstalling, it is still using Cemu a6fb0a4, not 2.6." The fix itself
was correct (verified directly against a real Cemu checkout earlier),
but never actually shipped -- root cause is a CI caching bug, not
anything a reinstall on the user's end could have worked around.

**Root cause:** `.github/workflows/release.yml`'s Cemu build cache key
was `cemu-<pinned-commit>-${{ hashFiles('...patch') }}-${{ runner.os
}}-v1` -- keyed on the pinned commit and the patch file's content hash,
neither of which the version-flag fix touched (it only changed
`scripts/lib/build_emulator.sh`'s own CMake invocation). The cache key
stayed byte-for-byte identical before and after the fix, so
`actions/cache` kept restoring the *pre-fix* `.release-work/cemu-src`
directory on every run since. Worse, `build_cemu()`'s own local
cache-hit check (`git apply --reverse --check` against the patch file)
then *also* saw "patch still applies cleanly in reverse" -- true, since
only the CMake flags changed, not the patched source -- and skipped the
rebuild that would have actually applied the new flags. Two independent
"is this still valid" checks, and neither one happened to be sensitive
to the one thing that had actually changed. This is the exact same bug
class this file's own Azahar cache-key comment already documents having
shipped once before (v0.1.39/v0.1.40, identical binaries despite a
patch change) -- that fix (hashing the patch file into the key) covers
a *patch* changing, but not a *build-invocation-only* change like this
one.

**A second, latent instance of the same bug found while investigating,
fixed proactively (not yet reported broken, but a live risk):** the
SDL3 cache key was still the literal string `sdl3-release-3.2.16-...`,
unchanged since before `SDL3_TAG` was bumped to `release-3.4.12`
earlier this session (see the touchpad-fix entry above) -- and
`build-release.sh`'s own local check was just "does
`SDL3Config.cmake` exist," with no verification of *which tag* was
actually installed there. That the touchpad fix demonstrably works on
real hardware is strong evidence this particular branch's cache
happened to be populated fresh (first write under this key on this
branch, not a stale restore) -- but the bug was real and live: any
future SDL3_TAG bump that forgot to also bump this string, or any
scenario where an old cache entry under this exact key got inherited,
would have silently kept serving 3.2.16 again with zero visible error.

**Fixed, both instances, two layers each (matching the project's
existing Azahar-cache-key precedent for the CI-side half):**
- CI cache keys bumped (`v1` -> `v2` for Cemu; the SDL3 key's literal
  tag string corrected to match `SDL3_TAG`'s actual current value) to
  force a genuine cache miss and real rebuild on the next release.
- **Root-cause, local-script-side fix, so this can't recur silently the
  same way again:** both `build_cemu()` (scripts/lib/build_emulator.sh)
  and the SDL3 build step (scripts/build-release.sh) now write a small
  marker file recording exactly what was actually built (Cemu:
  `version_major:version_minor`; SDL3: the tag string) immediately
  after a successful build, and their own cache-hit checks now also
  verify that marker matches what's currently being requested --
  independent of whatever the GitHub Actions cache key does or doesn't
  capture. A future change to either build's invocation that forgets to
  bump the corresponding CI cache key will still be caught locally and
  force a real rebuild, rather than silently reusing a stale binary
  indefinitely the way this one did.

**Verified:** the marker-file cache-hit/miss logic itself, directly --
simulated the three real scenarios (no marker present, matching marker,
mismatched marker) against a stand-in directory structure and confirmed
each one reports the correct hit/miss decision, without needing to run
an actual multi-hour Cemu rebuild to prove the *logic* is right.
`bash -n` clean on both modified scripts; `.github/workflows/release.yml`
parses as valid YAML.

**Not yet verified:** that the next real CI release build actually
produces a Cemu binary self-reporting as "2.6" rather than a commit
hash -- the previous entry's own verification (a live checkout,
patched, with the version flags visibly present in the printed build
command) proved the *fix* is correct; this entry's fix addresses why
that correct fix never reached a real release. The next release build
triggered after this entry is the one that should finally confirm the
whole chain end to end.

## 2026-08-03: Cemu detects no GPU at all, even after the version fix -- real root cause found in the AppImage's own LD_LIBRARY_PATH

Real user report, with the actual `log.txt` this time (not just a
symptom description): "it seems despite Cemu being on 2.6 instead of
the nightly build, it still does not detect my GPU, not iGPU. this
seems to be an issue with just the DualDeck integrated patches." The
log's own error is exact and specific:

```
------- Init Vulkan graphics backend -------
The following required Vulkan instance extensions are not supported:
VK_KHR_surface
VK_KHR_wayland_surface
```

**Checked the "is it the patch" hypothesis directly against the real
diff, not assumed:** `host/cemu-patches/0001-remote-server-integration.patch`
touches `VulkanRenderer.cpp` in exactly one place -- adding a new
`CaptureSurfaceBGRA()` function (used for this project's own video-
streaming capture, added well after Cemu's own `HandleScreenshotRequest()`)
plus its destructor cleanup. It never touches `vkCreateInstance`,
`vkEnumerateInstanceExtensionProperties`, device/instance-extension
enumeration, or anything within reach of the actual failing code path
-- confirmed by reading Cemu's real, unmodified upstream source at the
exact pinned commit (`GetInstanceExtensions()`-equivalent logic around
`VulkanRenderer.cpp:1255-1310`) and finding the failing check is
genuine, unmodified stock Cemu code. The patch's own C++ changes are
not the cause.

**The real mechanism, traced end to end through real source, not
guessed:**
1. Cemu's own `src/CMakeLists.txt` directly links
   `target_link_libraries(CemuCafe PUBLIC Wayland::Client)` when Wayland
   support is enabled -- confirmed in the real Cemu checkout, not this
   project's patch.
2. The user's actual setup is `emudeck-replace-in-place.sh`'s
   downloaded, patched AppImage (`/home/bazzite/Applications/Cemu.AppImage`,
   confirmed directly in their own terminal output) -- built by
   `pack_appimage()`/`bundle_library_dependencies()` (scripts/lib/
   appimage_pack.sh), which bundles the *full recursive `ldd` closure*
   of the Cemu binary built on GitHub's own CI runner (a headless
   machine with no real GPU or Vulkan ICD ever discoverable there).
3. `scripts/lib/apprun_templates.sh`'s generated AppRun script for the
   out-of-process AppImages (`generate_apprun_out_of_process()`)
   explicitly sets `LD_LIBRARY_PATH="${bundled_lib_path}:..."` on the
   exact line that launches the real emulator binary.
4. Cemu's own (again, unmodified) startup code calls
   `dlopen("libvulkan.so")` / `dlopen("libvulkan.so.1")` with a bare
   soname, not an absolute path -- which resolves through that same
   `LD_LIBRARY_PATH`-first search order as any other library. Whatever
   Vulkan-loader-adjacent library rode along in step 2's bundled
   closure gets found *before* the host's own real, correctly-
   functioning Vulkan loader -- explaining both symptoms together
   (`VK_KHR_surface`, the single most universal, always-present Vulkan
   extension on any real desktop, missing *and* `VK_KHR_wayland_surface`
   missing) on a machine confirmed to have a real, working GPU (an
   AMD Ryzen 7 9800X3D system, per the log's own hardware line -- note
   this CPU model itself has no integrated graphics at all, so a
   discrete GPU is a certainty here, not a maybe).

This is the exact same bug *class* this file already documents twice
over for this same AppImage-bundling mechanism -- a build-environment
copy of something that must always be the host's own shadowing the
correct one via `LD_LIBRARY_PATH` (glibc's `GLIBC_PRIVATE` symbol
mismatch crashing `mkdir -p`, and Qt's platform plugin discovery
failing outright) -- just a new instance of it, this time hitting
graphics-driver libraries instead.

**Fixed** by extending `bundle_library_dependencies()`'s existing
`_is_never_bundle_library()` exclusion list (already covering glibc's
own tightly-coupled components, with its own header comment explaining
exactly why) to also exclude the whole Mesa/Vulkan/GL driver-library
family: `libvulkan.so.*`, `libGL.so.*`, `libGLX.so.*`,
`libGLdispatch.so.*`, `libEGL.so.*`, `libgbm.so.*`, `libdrm.so.*`,
`libdrm_*.so.*`. This is standard, well-established practice for
portable Linux app packaging generally (AppImage/Flatpak/Snap
tooling -- including linuxdeploy, which this project's own bundling
logic is conceptually modeled after -- universally exclude Mesa/Vulkan/
GL libraries from bundles for exactly this reason: they are the
userspace half of the kernel's own GPU driver, not a portable
application dependency, and must always match whatever kernel driver
is actually running on the host).

**Scope, checked directly:** only the AppImage packaging path
(`emudeck-replace-in-place.sh`'s downloaded, patched AppImages) is
affected. The plain, non-AppImage `host/cemu` binary
(`run-host-cemu.sh`'s own launch path, used by DualDeck's own "Launch..."
menu when not using EmuDeck replace-in-place) execs Cemu directly with
no `LD_LIBRARY_PATH` override at all -- confirmed by reading that
script directly -- so it was never exposed to this specific mechanism.

**Refined further, same day:** a second, independent analysis of the
same log (the user's own follow-up, from another AI) converged on the
same root cause via a more specific mechanism worth incorporating
defensively even without a second log confirming the exact symbol:
Mesa's own Vulkan ICD -- now correctly loaded from the host once
`libvulkan.so.1` itself was excluded above -- can itself dlopen/link
`libwayland-client` at the WSI layer to implement
`VK_KHR_wayland_surface`. Since Cemu's own `CMakeLists.txt` links
`Wayland::Client` directly, `libwayland-client`/`libwayland-egl`/
`libwayland-cursor` were still riding along in the bundled closure even
after the first fix -- if the host's Mesa ICD resolved its own
Wayland-protocol symbol needs against *that* bundled (CI-environment,
plausibly version-mismatched) copy instead of the host's own, an
ABI-mismatched symbol lookup there is just as capable of silently
breaking Vulkan WSI support as a bundled `libvulkan.so.1` would have
been. Extended `_is_never_bundle_library()` to cover these three too,
for the identical reason as the rest of the list: they must always
match the host's live Wayland compositor/client ecosystem, never a
build machine's.

**Verified:** the exclusion logic itself, directly -- confirmed
`libvulkan.so.1`/`libGL.so.1`/`libEGL.so.1`/`libdrm.so.2`/
`libdrm_amdgpu.so.1`/`libgbm.so.1`/`libwayland-client.so.0`/
`libwayland-egl.so.1`/`libwayland-cursor.so.0` are all now correctly
excluded while ordinary libraries (X11, dbus, turbojpeg, etc.) still
bundle normally, both via direct unit-level calls to
`_is_never_bundle_library()` and by running the real
`bundle_library_dependencies()` function against this project's own
real, freshly-built `dualdeck-host-service` binary (whose own
dependency closure is unaffected by any of these exclusions, confirmed
identical before and after) and confirming none of the newly-excluded
names appear in its output. `bash -n` clean.

**Not yet verified:** on real Bazzite hardware -- whether Cemu's Vulkan
(and OpenGL, which crashed outright per the user's earlier report)
backends actually initialize successfully once the AppImage no longer
bundles a shadowing graphics-driver library. This is a strong,
mechanistically-complete root cause (not a guess -- traced through
real source on both the Cemu and DualDeck sides), but the next release
build is what will actually confirm it end to end.

## 2026-08-03: Cemu hangs on exit, has to be force-killed -- GUI thread blocked on a background-thread join

**Real user report:** "Ok, it seems to be working now, but odd thing, cemu
seems to hang when trying to exit. and I usually have to terminate it" --
reported immediately after the AppImage GPU-detection fix above got Cemu's
Vulkan renderer working again, so this is a distinct, newly-visible bug,
not a re-report of the graphics issue.

**Root cause:** traced directly through `host/cemu-patches/`'s own diff
content (`src/remote_server/RemoteServerBridge.cpp`, `src/gui/
MainWindow.cpp`, `src/Cafe/CafeSystem.cpp`), not guessed. Ending a title
(File > End Emulation, or closing Cemu with one running) calls
`MainWindow::EndEmulation()` on Cemu's own wx **GUI thread**, which calls
`CafeSystem::ShutdownTitle()` **synchronously**, which does
`s_remoteServerBridge.reset()` at the very top of teardown -- destroying
the `RemoteServerBridge` and running its destructor, `stop()`, all still
on that same GUI thread. `RemoteServerBridge::stop()` set
`stopRequested_ = true` and then immediately called
`reconnectThread_.join()` -- blocking the GUI thread until the background
reconnect thread (a loop that sleeps in 100ms increments, checking
`stopRequested_` each time, but also calls a *blocking*
`AdapterIpcClient::connect()` when not yet connected) actually wakes up
and exits. `adapter_sdk/ipc/src/adapter_ipc_client.cpp`'s own
`kRecvTimeoutSeconds = 5` bounds that blocking `connect()` attempt's
Hello/HelloAck handshake, so in the worst case (`stop()` called at the
exact moment the reconnect thread just started a `connect()` attempt --
e.g. no Host Service running, or a slow/stalled one) the GUI thread could
freeze for close to 5+ seconds, easily read by an impatient user as "it
hangs, so I kill it" well before it would have actually resolved on its
own.

Azahar's own `RemoteServerBridge::stop()` (`host/azahar-patches/`) turned
out to have the exact same pattern, copy-derived from the same design
(its own header comment literally says "same ordering melonDS's own
RemoteServerBridge teardown already uses, for the same reason") and
called just as synchronously from Qt's UI thread via
`GMainWindow::ShutdownGame()`'s `remote_server_bridge.reset()` -- a latent
version of the identical bug, fixed at the same time even though it
hadn't been separately reported, since it's the same root cause under a
different GUI toolkit.

**Fix:** both `stop()` implementations now hand the actual
`reconnectThread_.join()` + `ipcClient_->disconnect()` work off to a
**detached background thread**, so the GUI-thread caller (`ShutdownTitle()`
/ `ShutdownGame()`) returns immediately instead of blocking on it.
`adapter_`/`ipcClient_` are `std::move()`'d into that detached thread's
closure rather than left to be destroyed synchronously back on the GUI
thread the instant `stop()` returns: `ipcClient_`'s read/write threads
hold a reference to `*adapter_` and must finish shutting down (inside
`ipcClient->disconnect()`, which joins them) before `adapter_`'s own
destructor can safely run, so both must be destroyed together, in that
order, inside the same closure. The one piece of state still cleared
synchronously is Cemu's `s_currentAdapter` pointer (read every frame by
`LatteRenderTarget.cpp`'s render hook) -- set to `nullptr` at the very top
of `stop()`, before anything is hung off to the background thread, so any
code reading `currentAdapter()` after `stop()` begins immediately sees "no
active bridge," matching the function's previous synchronous behavior for
that one piece of state.

Also noticed in passing, not yet fixed (separate, minor, doesn't affect
this bug): `RemoteServerBridge`'s constructor `Bind()`s a wx event handler
to `g_mainFrame` on every construction (every title launch) with no
corresponding `Unbind()` in the destructor, so launching multiple titles
within one Cemu session accumulates duplicate event-handler bindings.
Left alone for now -- doesn't contribute to the hang (each stale binding
just calls the idempotent `EndEmulation()` an extra time) and fixing it
properly needs switching from a lambda to a bindable member-function
pointer, a slightly larger change than this fix's scope.

**Verified:** both patched `stop()` bodies apply cleanly via `git apply
--check`/`git apply` in an isolated scratch repo (confirming the diff
hunks are well-formed and self-consistent after hand-editing the patch
files' line counts); the core move-capture-and-detach pattern compiles
and runs correctly as a standalone `g++ -std=c++20 -Wall -Wextra -pthread`
reproduction, including the early-return guard for a `stop()` call with
nothing to hand off.

**Not yet verified:** on real hardware -- whether ending a title/closing
Cemu is now instant instead of hanging. The next release build is what
will actually confirm this end to end.

## 2026-08-03: Host Control mode gamepad bugs found via Steam's Controller Tester -- X/Y swapped, left stick inverted, no triggers/stick-clicks

**Real user report** (testing Host Control mode's virtual gamepad directly
in Steam's own Controller Tester): "Joystick controls are reversed, L
joystick reads down when going up, left if right, etc... X and Y are
reversed during host control mode, that should only happen in the
emulators, or we can map the emulator controls accordingly, as those are
Nintendo mappings... bumpers register, but Triggers do not work. same
with stick clicking."

**Root cause 1 (X/Y swapped):** `<linux/input-event-codes.h>` defines
`BTN_X` as a plain alias for `BTN_NORTH` (0x133) and `BTN_Y` as a plain
alias for `BTN_WEST` (0x134) -- confirmed by reading
`/usr/include/linux/input-event-codes.h` directly. A Linux input device's
button *index* (what SDL -- and Steam's Controller Tester, which reads
through SDL -- keys `gamecontrollerdb.txt`'s `x:bN`/`y:bN` entries on, for
a device identified as an Xbox 360 pad) is assigned in ascending raw-code
order, not `UI_SET_KEYBIT` call order: `BTN_NORTH` (0x133) sorts before
`BTN_WEST` (0x134). `host_control_adapter.cpp`'s `kButtonMappings` sent
`DSButton_X`'s presses as code `BTN_WEST` (the higher index) and
`DSButton_Y`'s as `BTN_NORTH` (the lower index) -- backwards from
gamecontrollerdb's expectation that the lower index is "x", so Steam
displayed X presses as Y and vice versa.

**Root cause 2 (left stick inverted):** `client/src/main.cpp` negates
`leftStickY`/`rightStickY` before they ever hit the wire, specifically to
match the 3DS circle pad's `GetStickDirectionState()` convention
(positive = up, confirmed by reading Azahar's `hid.cpp` directly) --
correct for an emulated analog stick, but `host_control_adapter.cpp`'s
`translateControllerState()` passed that already-inverted wire value
straight through into a uinput `ABS_Y` axis, where the universal
joystick/evdev/SDL convention is the opposite (positive = down). This
silently double-applied the flip for Host Control mode specifically,
inverting every up/down motion Steam's tester displayed.

**Root cause 3 (no triggers/stick-clicks):** Host Control mode's virtual
gamepad never had a right stick, analog triggers, or thumbstick-click
support at all -- `HostControlGamepadState` only ever had `leftStickX/Y`
and eight digital buttons. `rightStickX/Y` were already on the wire
(`ControllerState`) but never read here; there was no wire representation
at all for analog triggers or thumbstick clicks, since the DS/3DS/Wii U
devices this protocol was originally built for have neither.

**Fix:**
- `host/remote-server/src/host_control_adapter.cpp`: `kButtonMappings` now
  uses the `BTN_X`/`BTN_Y` aliases directly (same numeric codes, assigned
  to the fields whose wire meaning they actually match) instead of
  `BTN_WEST`/`BTN_NORTH`. `translateControllerState()` re-negates
  `leftStickY`/`rightStickY` (a local `negateStickAxis()`, mirroring
  `client/src/main.cpp`'s own) so the double-flip cancels out.
- **Protocol v12** (`protocol/include/melonds_remote/protocol.h`):
  `ControllerState` gained `leftTrigger`/`rightTrigger` (0..255) and
  `hostControlButtons` (a new `HostControlButton` bitmask:
  `ThumbLeft`/`ThumbRight`) -- host-control-mode-only fields, same
  contract as v11's `mouseDeltaX/Y`/`mouseButtons` (no DS/3DS/Wii U game
  reads them). Wire size grows by 3 bytes.
- `host_control_adapter.h/.cpp`: `HostControlGamepadState` gained
  `rightStickX/Y`, `leftTrigger`/`rightTrigger`, `thumbL`/`thumbR`; the
  uinput device now advertises `ABS_RX`/`ABS_RY` (right stick, same
  range as the left), `ABS_Z`/`ABS_RZ` (triggers, 0..255, matching a real
  Xbox 360 pad's convention), and `BTN_THUMBL`/`BTN_THUMBR`.
- `client/src/main.cpp`: reads `SDL_GAMEPAD_AXIS_LEFT_TRIGGER`/
  `RIGHT_TRIGGER` (SDL's 0..32767 range, scaled to the wire's 0..255) and
  individual `SDL_GAMEPAD_BUTTON_LEFT_STICK`/`RIGHT_STICK` clicks (not the
  L3+R3 *combo*, which stays reserved for the menu-open chord -- see
  `kMenuChordHoldUs`'s comment; a lone click of either stick is never part
  of that chord and is safe to forward every tick).
- **melonDS's frozen `protocol.h`/`protocol.cpp` copy** (`host/melonds-
  patches/`) regenerated to exactly match the live files -- melonDS runs
  its own in-process `NetServer` implementing this wire protocol directly
  (unlike Azahar/Cemu, which only ever see the separate, unaffected
  `GenericInputState` over adapter IPC), so a version mismatch here would
  have broken every melonDS connection the moment a v12 client talked to
  a v11-frozen melonDS host. This regeneration also happened to pick up
  `AppVersionMismatchUpdateTriggered` (added earlier this session for the
  client-triggers-host-update feature), which had been missed when that
  feature shipped -- purely additive/non-wire-breaking on its own, but
  now correctly in sync going forward too.

**Verified:** all three fixes via new/updated unit tests in
`test_host_control_adapter.cpp` (button-code assignment is exercised
indirectly through `translateControllerState()`'s existing
`west`/`north` boolean tests, which are unaffected -- the actual raw
uinput code change isn't unit-testable without a real `/dev/uinput`, see
below; the Y-renegation and new trigger/thumb-click fields are directly
tested) and `test_controller_state.cpp`'s protocol round-trip test,
extended for the three new fields; a full local build of
`dualdeck-host-service`, `dualdeck-client`, and every existing test suite
(host: 94 cases, protocol: 104 cases, client net/settings: 20 cases, all
passing); melonDS's regenerated `protocol.h`/`protocol.cpp` confirmed
byte-identical to the live repo copies and compiling cleanly standalone;
the melonDS patch as a whole re-confirmed to `git apply` cleanly against
its pinned commit.

**Not yet verified:** on real hardware -- whether Steam's Controller
Tester now shows correct X/Y labels, an upright left stick, and working
triggers/stick-clicks for Host Control mode. The uinput device-creation
and raw ioctl/event-write paths (`UI_SET_ABSBIT`/`UI_ABS_SETUP` for the
new axes, the actual `BTN_X`/`BTN_Y`/`BTN_THUMBL`/`BTN_THUMBR` events)
cannot be exercised in this sandbox (no `/dev/uinput`) -- only compile-
clean and the pure `translateControllerState()` logic are verified here.

## 2026-08-03: Real-hardware multi-emulator bug sweep -- Cemu/Wii U triggers+L3/R3 never wired, Azahar/Cemu dual-screen race, mirror never actually reachable, Cemu touch confirmed not implemented

**Real user report** (single message, after testing all three emulators plus
Host Control mode on real hardware): "Cemu Touch screen does not work,
Keybinds are also not automatically applied... Launching Azahar (possibly
for the first time) shows both screens on the host until the client
disconnects and reconnects... opening Configure on Azahar crashes it...
MelonDS integration seems to be completely broken... also, it seems the
toggle for dualdeck is not integrated into melonDS's GUI anymore... Host
Control Screen mirroring still does not work, as falls back to a grey
screen." (The gamepad-specific parts of this same report -- X/Y swap,
inverted stick, missing triggers/stick-clicks in Host Control mode --
have their own entry above.)

### Fixed: Cemu's real Wii U ZL/ZR triggers and stick-clicks never had data

**Root cause:** `host::AdapterBridge::applyControllerState()` -- the
translation every out-of-process emulator adapter's input goes through --
never populated `GenericInputState::leftTrigger`/`rightTrigger`, and never
set `GenericButton_L2`/`R2`/`L3`/`R3`. `host/cemu-patches/`'s
`RemoteController::raw_state()` already reads `m_status->leftTrigger`/
`rightTrigger` and already maps `GenericButton_L3`/`R3` to the Wii U
GamePad's real `kButton6`/`kButton7` stick-click bits (confirmed by
reading that file directly -- the Wii U GamePad genuinely does have
clickable analog sticks and analog ZL/ZR, unlike DS/3DS) -- but nothing
upstream of it ever supplied real values, so these were always zero
regardless of what the client sent.

**Fix:** `host/remote-server/src/adapter_bridge.cpp` now forwards
`state.leftTrigger`/`rightTrigger` straight through to
`GenericInputState`, and a new `extraButtonsToGenericButtons()` maps
protocol v12's `ExtraButton_ThumbLeft`/`ThumbRight` (see the gamepad
entry above -- renamed from the original `HostControlButton`/
`hostControlButtons` once it became clear Cemu needed these too, not
just `host::HostControlAdapter`) to `GenericButton_L3`/`R3`. The client
already sends real trigger/thumb-click data unconditionally (not gated
on session mode), so this alone closes the gap for Cemu without any
further client changes.

**Verified:** new unit tests (`adapter_bridge_translates_triggers`,
`adapter_bridge_translates_extra_buttons_to_l3_r3`) in
`test_adapter_bridge.cpp`; full local build of `dualdeck-host-service`
and the entire host test suite (96 cases, all passing).

**Not yet verified:** on real Cemu gameplay -- whether ZL/ZR and stick
clicks now register. This may also be some or all of what the user
described as "keybinds are also not automatically applied" if what they
actually meant was "triggers don't do anything even though I bound
them" -- worth confirming specifically once this ships, since the
*separate*, already-existing auto-mapping mechanism (`CemuAdapter`'s
constructor calling `add_controller()`/`set_default_mapping()` against
VPAD player 1, mirroring Azahar's `registerInputEngine()`) was traced in
full and found correct, gated only on a pre-existing, already-logged
prerequisite ("player 1 isn't currently configured as a Wii U GamePad
controller" -- Controller Settings -> Player 1). If keybinds still don't
auto-apply after this fix and that prerequisite is confirmed met, that's
a genuinely separate bug needing its own report.

### Fixed: Azahar/Cemu "both screens until reconnect" -- a real race in the adapter IPC layer

**Root cause**, confirmed via direct reading of both DualDeck's own
`adapter-sdk/ipc/src/adapter_ipc_server.cpp` and the real, unmodified
upstream Azahar source at the pinned commit: the remote client's
connection to `NetServer` (host/remote-server) can complete *before* the
out-of-process emulator adapter (Azahar/Cemu) finishes its own, entirely
separate IPC handshake with the Host Service -- both connect
concurrently on process startup, and there is no ordering guarantee
between them. `AdapterIpcServer::notifyClientConnectionChanged()` was a
pure pass-through with no memory: if it was called with no adapter
connected yet, the notification was silently dropped forever, and
nothing re-sent it once an adapter did connect moments later. Azahar's
own `GMainWindow::OnRemoteClientConnectionChanged()` (which forces
`layout_option = SingleScreen`) therefore never ran until the *next* real
transition -- a disconnect (correctly delivered, since the adapter is
connected by then), followed by a reconnect (also now correctly
delivered) -- exactly matching "both screens until disconnect, then
correct after reconnect." Two independent research agents traced this
end to end against real source (one ruled out a stock-Azahar-reapplies-
layout hypothesis and a callback-registered-too-late hypothesis first,
before finding the actual mechanism).

**Fix:** `AdapterIpcServer` now remembers the last known
client-connection state (`lastKnownClientConnected_`) regardless of
whether an adapter happens to be listening at the time.
`notifyClientConnectionChanged()` records it unconditionally before its
existing no-adapter-connected early return. `serveConnection()` sends
one catch-up `ClientConnectionChanged` message to a newly-handshaked
adapter immediately, so it learns the real current state instead of
defaulting to "not connected." Affects Cemu identically (same
`AdapterIpcServer`, same race window), even though only Azahar was
explicitly reported.

**Verified:** two new end-to-end tests
(`ipc_adapter_connecting_after_notify_still_learns_current_state`,
`ipc_adapter_connecting_with_no_prior_notify_learns_not_connected`)
using a real `AdapterIpcServer` + `AdapterIpcClient` over a real Unix
socket in the same test process (not mocked); the existing
`ipc_client_connection_changed_relayed_from_server_to_adapter` test
updated for the new catch-up message's extra callback firing. Full
adapter-sdk test suite (53 cases) and host test suite (96 cases) both
passing.

### Fixed: Host Control screen mirroring was structurally unreachable, not just failing

**Root cause:** `DUALDECK_HOSTCONTROL_MIRROR_SCREEN` -- the env var
`HostControlAdapter`'s constructor actually gates screen capture on --
was never exported by any real install or launch path in this repo (a
repo-wide grep found zero matches outside this very documentation file).
The X11 and Wayland portal+PipeWire capture backends built earlier this
session were fully implemented and unit-verified, but completely
unreachable in a real install without hand-editing a systemd unit
override yourself -- explaining "still does not work, as falls back to a
grey screen" independent of whatever the original X11-vs-Wayland
diagnosis found.

**Fix:** `scripts/build-release.sh`'s generated
`host/internal/host-control-daemon.sh` (the persistent
`dualdeck-host-control.service`'s actual `ExecStart` command, and the
real, always-running Host Control entry point today) now defaults
`DUALDECK_HOSTCONTROL_MIRROR_SCREEN=1` unless already set in the
process's own environment. `HostControlAdapter`'s own opt-in design is
unchanged for every other launch path (e.g. `run-host.sh`'s manual,
Steam-shortcut-based Host-Control branch) -- only the daemon that's
actually meant to be the persistent, real-world entry point now reaches
it by default, and the client already has its own Settings toggle
(`clientSettings.mirrorHostScreen`) to opt out of *displaying* whatever
the host sends.

**Verified:** `bash -n` clean on both `build-release.sh` itself and the
extracted, generated `host-control-daemon.sh` body.

**Not yet verified:** on real hardware -- whether mirroring now actually
shows something once the daemon is rebuilt/reinstalled. Also unverified:
whether the *original* grey-screen symptom (before this fix) was ever
actually reaching either capture backend at all, given the env var could
never have been set through any normal install -- the earlier
X11-vs-Wayland root-cause work may turn out to be moot for anyone using
the persistent daemon, or may still matter once capture is actually
reachable.

### Investigated, not fixed: Azahar Configure crash

A dedicated research pass cloned the real, pinned Azahar commit and
traced the entire Configure -> Input tab construction path
(`ConfigureDialog` -> `ConfigureInput::LoadConfiguration()` ->
`UpdateButtonLabels()` -> `ButtonToText`/`AnalogToText`) against the
leading hypothesis (DualDeck's patch overwrites
`Settings::values.current_input_profile` with a synthetic
`"melonds_remote"` input engine string -- see `AzaharAdapter.cpp`'s
`registerInputEngine()` -- which the Configure dialog's UI code might
not handle gracefully). Every site that reads an unrecognized engine
string in this path was confirmed, by reading the real source, to
degrade safely (falls through to `"[unknown]"`, uses
`ParamPackage::Get()`'s exception-safe defaulted lookups, or isn't even
reachable from dialog construction at all) -- **this specific hypothesis
is ruled out**, not just unconfirmed. The more likely remaining
candidates (DualDeck's own `RemoteButtonFactory`/`RemoteAnalogFactory`/
`RemoteTouchFactory::Create()`, none of which exist in upstream Azahar so
couldn't be checked against real behavior this way, or something
entirely unrelated to the remote-server patch) need an actual crash
backtrace to pin down -- this project's established discipline is not to
guess further without one. **Needed from the user next time this
happens:** Azahar's own crash log/backtrace (stderr output, or
`~/.local/share/azahar-emu/log/azahar_log.txt` if the crash is graceful
enough to flush it), and whether it happens with no game running, with a
game running but not streaming, or only while a DualDeck client is
actively connected.

### Investigated, not fixed: Cemu GamePad touch screen -- confirmed genuinely unimplemented, not a bug

`host/cemu-patches/`'s `CemuAdapter::capabilities()` explicitly sets
`gamePad.touchSupported = false`, with its own honest comment: "Cemu has
no GamePad touch input pipeline for any controller backend today." This
is a real, deliberate, already-documented scope decision from when this
patch was first built, not a regression or an overlooked bug --
confirmed by reading the patch's own source directly. Wii U GamePad
touch input is a genuinely separate, unbuilt feature (would need a new
path from wherever Cemu's VPAD touch state is read into
`RemoteController`, plus routing the wire protocol's touch fields to the
GamePad surface specifically), not something fixable as part of this bug
sweep. Tracked as a real roadmap item, not closed out here.

### Investigated, not fixed: melonDS "completely broken," GUI toggle "not there anymore"

Extensive static verification found no evidence of a patch-level
regression: `git apply --check` against the exact pinned melonDS commit
succeeds cleanly; the "Enable DualDeck (Steam Deck streaming)" checkbox
(`chkMelonDSRemoteEnable`) is present and correctly wired in both
`EmuSettingsDialog.ui` and `.cpp`; `EmuInstance::startRemoteServer()`'s
env-var-or-Config-key precedence, port-bind-failure logging, and
management-listener wiring all read correctly; the known Flatpak-vs-
AppImage EmuDeck install-shape gap (`find_emudeck_melonds_flatpak_
launcher()`) was already discovered and fixed on 2026-08-01, before this
report. The only concrete, confirmed regression risk found this session
-- melonDS's frozen `protocol.h`/`protocol.cpp` copy going out of sync
with today's v12 wire bump -- has already been closed (see the gamepad
entry above); melonDS was still correctly at v11 (matching the live repo)
*before* that fix, so it cannot explain the user's original report, which
predates today's protocol change entirely. No other static-analysis
avenue turned up a plausible mechanism. **Needed from the user next
time:** melonDS's own log output (stderr, or whatever
`docs/known-limitations.md`'s existing melonDS log-location guidance
points to) from a session where the checkbox was confirmed missing, and
confirmation of whether that melonDS install is the AppImage DualDeck's
replace-in-place installer produced (check the sidecar
`.dualdeck.json` manifest next to it) or something else entirely (a
stock EmuDeck download, a manually-built copy, etc.).

## 2026-08-03: Client-triggers-host-update notifier broke -- a self-inflicted regression from this same session's protocol v12 bump

**Real user report:** "Client -> Host update notifier seemingly broke in
this version."

**Root cause:** `net_server.cpp`'s Hello-handling code required
`header->protocolVersion == kProtocolVersion` just to attempt parsing the
Hello payload at all -- if it didn't match, the whole block was skipped
and the handshake fell through to the default `ProtocolVersionMismatch`
rejection, never reaching the appVersion-mismatch/self-update-trigger
logic beneath it (added earlier this same session, see the
client-triggers-host-update entries above). This was harmless as long as
`kProtocolVersion` stayed stable across releases (an app-version-only
difference, same wire format, was the only mismatch case that could ever
happen) -- but this same session's Host Control gamepad fix bumped
`kProtocolVersion` from 11 to 12 (`ControllerState` grew 3 bytes for
`leftTrigger`/`rightTrigger`/`extraButtons`). From that point on, an
already-approved device on the old (pre-v12) client build hitting a
freshly-updated (v12) host -- exactly the scenario this feature exists to
recover from -- got silently rejected with plain `ProtocolVersionMismatch`
instead of triggering a self-update, because the version mismatch was now
at the *wire* level, not just the app-version level, and that path never
even looked at the Hello payload.

**Fix:** the `header->protocolVersion == kProtocolVersion` check moved out
of the top-level gate (which now only requires `PacketType::Hello` and a
sane `payloadSize`) into a `protocolVersionMismatch` flag checked
alongside the existing appVersion-mismatch condition. `HelloPayload`'s own
wire layout is independent of `kProtocolVersion` (confirmed by reading
`parseHelloPayload()` -- purely length-prefixed strings read from
`data`/`size`, never consults the packet header), so parsing it is safe
regardless of a version mismatch; a genuinely incompatible future
`HelloPayload` layout still fails safely via that function's own strict
size/trailing-byte checks, landing in the existing "malformed Hello
payload" branch rather than misparsing. A real protocol-version mismatch
now takes the exact same already-approved-device-triggers-self-update path
an app-version-only mismatch always did, since it's if anything a
*stronger* signal that the host needs to update to match, not a weaker
one.

**Verified:** new end-to-end test
(`approved_device_with_protocol_version_mismatch_triggers_self_update` in
`test_self_update_trigger.cpp`) using a real `NetServer` over real
loopback sockets and a hand-crafted Hello packet with a mismatched
`protocolVersion` field in its header (since `buildHelloPacket()` always
stamps the live version) -- confirms both the `AppVersionMismatch
UpdateTriggered` reject reason and that `selfUpdateCommand` actually
fires. Full host test suite (97 cases, all passing) and a clean local
build of `dualdeck-host-service`.

## 2026-08-03: Azahar "opens as a background process, but does not render a window" on Bazzite -- the already-flagged missing Distrobox launch path

**Real user report:** "Azahar now does not open fully, it opens as a
background process, but does not render a window."

**Root cause:** `run-host-azahar.sh` already carried an explicit,
honest warning for exactly this situation -- "this looks like an
immutable (rpm-ostree) system, e.g. Bazzite -- Azahar's Distrobox launch
path isn't built yet... so this may fail if a required library isn't
already present on the base image" -- and then ran the Azahar binary
directly against Bazzite's own minimal base image regardless. Unlike
Cemu (GTK3/Vulkan/PulseAudio -- much more likely to already be present
on a gaming-focused base image), Azahar needs Qt6, which Bazzite doesn't
ship. When Qt6 can't find a real xcb/wayland platform plugin, it
silently falls back to a non-visible platform rather than crashing
loudly -- the exact same failure mode already fixed for the AppImage
path via bundled Qt platform plugins (see `apprun_templates.sh`), just
never fixed for this native launch path. "Process alive, no window" is
exactly what that produces.

**Fix:** `run-host-azahar.sh` now routes through the same
`"dualdeck-host"` Distrobox container `install-host-distrobox.sh`
already creates and provisions for melonDS on immutable systems --
confirmed to already carry `qt6-qtbase-devel`/`qt6-qtbase-private-devel`/
`qt6-qtmultimedia-devel`/`qt6-qtsvg-devel` and friends, so no new
container or package list was needed, just wiring Azahar's launch
through it. Distrobox forwards `DISPLAY`/`WAYLAND_DISPLAY`/
`XDG_RUNTIME_DIR` and mounts `$HOME` automatically; the DualDeck-specific
env vars (`AZAHAR_REMOTE_ENABLE`, etc.) are passed explicitly via
`env` inside the `distrobox enter` call, matching
`install-host-distrobox.sh`'s own identical pattern for melonDS, rather
than assuming Distrobox forwards arbitrary non-XDG env vars (it doesn't
promise to). Deliberately *not* `exec`'d -- same reason this script's
own `HOST_SERVICE_PID` cleanup trap comment already gives: the shell
needs to stay alive to run that trap once Azahar (now running inside the
container) actually exits. If the container doesn't exist yet (melonDS
has never been launched on this machine), this now fails with a clear,
actionable message instead of silently running against the bare host --
directing the user to launch melonDS once first, or run
`install-host-distrobox.sh --install-only` directly, rather than
duplicating that script's own container-creation/package-install logic
a second time here.

Cemu's own identical warning/direct-launch pattern is left unchanged for
now -- no user report suggests its window-rendering is actually broken
(the user could open and interact with its Controller Settings), and
wrapping it in the same container without first confirming its own
runtime deps (GTK3/Vulkan/PulseAudio/libsecret/bluez/libgcrypt/libusb --
none of which are in the container's current package list, only
melonDS's Qt6/SDL2/X11/Wayland set) are actually present there risks
introducing a *new* problem rather than fixing a confirmed one.

**Verified:** `bash -n` clean on both `build-release.sh` itself and the
extracted, generated `run-host-azahar.sh` body.

**Not yet verified:** on real Bazzite hardware -- whether Azahar now
actually renders its window once launched through the container, and
whether the container's existing package set is sufficient (it was
provisioned for melonDS, not audited specifically against Azahar's own
`ensure_packages()` list of `qt6-base-dev`/`libvulkan1`/`libsdl2-2.0-0`/
`libopenal1`/`libboost-iostreams`/`libboost-thread`).

## 2026-08-03: MelonDS and Host Control mirror -- both root-caused from real host diagnostics

**Real user reports followed up with actual host-side evidence** (run
directly on the Bazzite host, per this project's established "get real
logs before guessing further" discipline):

### MelonDS: missing `libturbojpeg.so.0` in the Distrobox container

Running `~/.config/dualdeck/install/melonDS` directly showed the real
cause immediately: `error while loading shared libraries:
libturbojpeg.so.0: cannot open shared object file`. melonDS runs its own
in-process `NetServer` (its patch vendors a full copy of
`net_server.cpp`/`.h`, unlike Azahar/Cemu, which delegate video encoding
to the separate `dualdeck-host-service` process -- already fixed to
bundle this same library for exactly this reason, see that binary's own
`internal/lib` bundling), so melonDS's own binary links TurboJPEG
directly and needs `libturbojpeg.so.0` itself at runtime.
`install-host-distrobox.sh`'s `dualdeck-host` container package list
simply never included `turbojpeg-devel` at all -- not a devel-vs-runtime
naming mismatch like some other packages in that list, a plain omission.
A binary that can't pass the dynamic linker before `main()` explains
both halves of the report at once: no server ever starts, and no window
ever opens (so no menu, no checkbox to see either).

**Fix:** added `turbojpeg-devel` to the `dnf install` list in
`install-host-distrobox.sh`, matching the exact package name already
verified correct elsewhere in this file's own `ensure_packages()` calls
for the same library. Takes effect automatically the next time this
script (re-)runs against the user's existing container -- including via
the normal self-update path, since `apply-update.sh` delegates to this
exact script on immutable systems.

### Host Control mirror: X11 "succeeding" against XWayland's own empty root, never actually reaching the Wayland fallback

`journalctl` showed `HostControlAdapter: screen-mirror capture ready
(X11)` -- on a confirmed Wayland session. This was the exact failure
mode already named (but not actually fixed) in this same file's
2026-08-03 mirror entry above: `X11ScreenCapture::isReady()` only checks
"can I connect to a display and does XShm work," which XWayland (the
X11-compatibility layer every real Wayland session still runs) happily
answers yes to, even though its root window has no real desktop content
composited into it -- real content lives entirely in the Wayland
compositor, invisible to X11. The previous fix built a genuine Wayland
portal + PipeWire fallback, but kept trying X11 *first* unconditionally
"since it's cheaper" -- and X11 kept falsely succeeding on this exact
kind of session, so the Wayland fallback was never actually reached in
practice. The client showing black instead of the old grey placeholder
makes sense in hindsight: previously mirroring was never enabled at all
(nothing to capture, client showed its own grey placeholder); once
defaulted on, X11 was now genuinely capturing XWayland's real, empty,
black root window and sending *that*.

**Fix:** `host_control_adapter.cpp`'s constructor now checks
`WAYLAND_DISPLAY` before choosing which backend to try first. If set (a
real Wayland session -- XWayland's mere presence doesn't matter), it
tries `WaylandScreenCapture` first, falling back to `X11ScreenCapture`
only if the portal path doesn't pan out. If unset (a real X11-only
desktop), the original X11-first behavior is unchanged (cheap,
synchronous, no portal permission prompt). `isMirrorReady()`/
`getLatestFrame()`/`frameDimensions()` needed no changes -- they already
null-check and `isReady()`-check both backends independently, agnostic
to construction order.

**Verified:** full local build of `dualdeck-host-service` and the full
host test suite (97 cases, all passing) after the
`host_control_adapter.cpp` change; `bash -n` clean on `build-release.sh`
and the extracted, generated `install-host-distrobox.sh` body for the
`turbojpeg-devel` addition.

**Not yet verified:** on real Bazzite hardware -- whether melonDS now
actually starts and shows its checkbox once the container gets the new
package, and whether the Wayland portal path (now actually reachable)
produces a real, non-black captured frame this time, including the
one-time interactive screen-share permission prompt actually appearing
and being answerable from a `systemd --user` daemon context.

## 2026-08-03: Cemu "controls work, but have to be manually bound" -- `set_default_mapping()`'s own guard was silently no-oping the auto-bind

Real user report, after the previous batch's Cemu trigger/L3-R3 wiring fix
landed: "Cemu controls *Work*, but they need to be binded by the user,
which I do not like, otherwise it works fine." Confirmed via reading real
Cemu source at the pinned commit (`a6fb0a4`, v2.6): `CemuAdapter`'s
constructor auto-binds the remote controller onto Player 1 by calling
`VPADController::set_default_mapping(m_remoteController)`
(`src/input/emulated/VPADController.cpp`) -- but that function's own final
step only fills a Wii U button slot when
`m_mappings.find(m.first) == m_mappings.cend()`, i.e. it silently does
nothing for any slot Player 1 already has *any* mapping for. This guard
exists to protect a user's own hand-configured controller from being
clobbered by, say, plugging in a second physical pad -- reasonable in
general, but it means `set_default_mapping()` only actually does anything
the *literal first time ever* Player 1 gets configured. The moment a user
has set up any real controller for Player 1 (true for nearly every
real-world Cemu profile), every slot is already "mapped," and
`set_default_mapping()` silently no-ops for DualDeck's remote controller
too -- explaining exactly the report: video/basic session works (that
path doesn't go through this mapping at all), but no input arrives until
the user manually rebinds Player 1 to the DualDeck-labeled controller
themselves.

**Fix:** `CemuAdapter.cpp` no longer calls `set_default_mapping()`. It
calls the new `forceRemoteControllerMapping()` instead, which replicates
`VPADController.cpp`'s own `InputAPI::XInput`/`InputAPI::DualDeckRemote`
button/axis table (the same table gated behind that guard) and calls the
lower-level `EmulatedController::set_mapping()` directly, once per entry
-- a public, unconditional `m_mappings[mapping] = { controller, button }`
with no "already mapped" check at all (confirmed by reading
`EmulatedController.cpp` directly). This makes DualDeck's remote
controller always take over Player 1's full button/axis mapping the
instant a client session starts, regardless of whatever was configured
before -- matching the precedence every other DualDeck emulator
integration already gives network input over local input for the
duration of a session.

**Verified:** `git apply --check` (and a real apply) of the patch against
a fresh clone of Cemu at the exact pinned commit (`a6fb0a48eb4...`)
succeeds cleanly; the replicated table, `VPADController::kButtonId_*`
enum values, and `EmulatedController::set_mapping()`'s signature were all
read directly from that same real checkout, not guessed. `CemuAdapter.cpp`
already includes `input/api/Controller.h`, `input/emulated/
EmulatedController.h`, and `input/emulated/VPADController.h` (needed for
the unscoped `Buttons2` enum and `VPADController::ButtonId` enum used by
the table) -- no new includes were required.

**Not yet verified:** a full Cemu build (infeasible in this sandbox --
Vulkan/OpenGL/Latte-renderer dependency chain) and real-hardware
confirmation that Player 1's controls now register immediately on
connect with zero manual rebinding, on a Cemu profile that already had a
real controller configured for Player 1 beforehand.

## 2026-08-03: Last frame persists on screen after exiting an emulator, until reconnect

Real user report: "when exiting an emulator, for example azahar, when
exiting the application, the last frame that was sent to the client
persists on screen until I change client and reinitialize the connection
or exit and reopen the client app." Root-caused via direct code reading
in `client/src/net_client.cpp`: `hasFrame_` is set `true` the first time
`videoReceiveLoop()` ever decodes a frame, but was never reset anywhere
in the file. `getLatestFrame()`'s only gate is `if (!hasFrame_) return
false;`, so once any frame has ever been decoded in a session, it keeps
handing back that same stale `latestFrame_` buffer to every caller
forever -- regardless of the host's mode changing, the adapter that
produced it disconnecting, or the control connection itself staying
otherwise healthy. `main.cpp`'s render loop only stops calling
`getLatestFrame()` once `hostMode()` reads `HostControl` *and*
`clientSettings.mirrorHostScreen` is off; with mirroring on (an
experiment this same session added and has been actively testing), or in
the brief window before a `ModeChanged` packet is processed, it kept
redrawing the last real frame indefinitely -- exactly matching the
report. Reconnecting via "exit and reopen the client app" already masked
this by accident: a fresh process means a fresh `NetClient` object, whose
`hasFrame_` starts `false` again -- not because reconnecting itself did
anything to clear stale state.

**Fix:** `controlReceiveLoop()`'s `ModeChanged` handling now resets
`hasFrame_` to `false` (under `frameMutex_`) whenever the reported mode
actually changes, before updating `hostMode_`. A mode change is exactly
the signal that whatever video state existed before is no longer valid,
so the render loop falls through to the test pattern / host-control
placeholder the moment the notification arrives, instead of only on a
full reconnect.

**Verified:** new end-to-end test
`net_client_clears_stale_frame_on_mode_change`
(`client/tests/test_net_client.cpp`) -- a real `NetClient` against a real
`NetServer` over loopback, confirms `getLatestFrame()` returns a real
frame while in `Emulation` mode, then confirms it starts returning
`false` once the host mode changes to `HostControl`, reproducing the
exact before/after this fix changes. Full local build + `ctest` run (6
suites: protocol, adapter-sdk, client-settings, net-client, config
migration, host -- all passing, 0 failures) confirms no regressions.
`net_client.cpp` also recompiles clean under the same strict
`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror` flags CI uses.

**Not yet verified:** on real hardware, whether the placeholder/test
pattern now actually appears immediately after an emulator exits with
`mirrorHostScreen` on (the previously-untested combination this fix
specifically addresses) versus the already-working case with mirroring
off.

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
