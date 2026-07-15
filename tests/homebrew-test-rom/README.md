# Homebrew Test ROM

A minimal, **fully original** (no copyrighted content, no libnds, no
external downloads) homebrew NDS ROM used to verify
`host/melonds-patches/0001-remote-server-integration.patch` against a
real, direct-booting, JIT-executing ROM rather than just an unmodified
melonDS build with no cartridge inserted. `SPEC.md` explicitly forbids
including commercial ROMs in this repository (section 19); this sidesteps
that entirely by being source code you compile yourself, not a binary
game -- similar in spirit to how the project's other tests don't check
in any third-party binaries.

## What it does

- `arm9.c`: sets both 2D engines' `DISPCNT` to display-mode 1 (regular 2D
  graphics), then loops forever reading the real `KEYINPUT` hardware
  register (0x04000130) and writing the currently-held buttons (A/B/Up)
  as a solid RGB backdrop color into engine B's BG palette entry 0 (the
  "backdrop" color shown when no BG layer covers a pixel). No tiles, no
  sprites, no font -- this is the simplest possible way to make the
  emulated screen visibly, immediately reflect real DS button input,
  using only a register read and a register write per frame.
- `arm7.c`: an empty infinite loop. NDS direct boot jumps both CPUs to
  their entry points; ARM7 just needs to exist and not crash.
- `build_rom.py`: hand-packs a valid 4096-byte `NDSHeader` (matching
  melonDS's own compiled struct layout, verified with an `offsetof()`
  dumper against melonDS's actual `NDS_Header.h` rather than assumed) plus
  the two compiled binaries into a `.nds` file that satisfies melonDS's
  `ValidateROM()` checks (see the script's docstring for the exact rules).
- `interactive_pipeline_test.py`: drives the real network pipeline (real
  UDP `ControllerState` packets, exactly as the SDL3 client sends) against
  a running patched melonDS with this ROM booted, and confirms specific
  button holds produce specific, stable, expected colors in the delivered
  video frames -- verifying SPEC.md section 20 criteria (4)-(8).
- `stability_test.py`: runs a sustained session (continuous input +
  continuous video draining) for a target duration, checking for
  disconnects, stalled video, and process RSS growth -- verifying SPEC.md
  section 20 criterion (12).

## Building

Requires a bare-metal ARM cross-compiler (Ubuntu/Debian:
`apt install gcc-arm-none-eabi binutils-arm-none-eabi`):

```sh
./build.sh
```

Produces `test.nds`. Run it against the patched melonDS (see
`host/melonds-patches/README.md` for how to build that):

```sh
MELONDS_REMOTE_ENABLE=1 MELONDS_REMOTE_AUTH_TOKEN=some-token \
  ./melonDS --boot always /path/to/test.nds
```

## What this proved

Used to verify `host/melonds-patches`'s video-capture and input-injection
paths end-to-end:

- Built and direct-booted successfully (confirmed via melonDS's own log
  output: "Inserted cart with game code: ####", "Game is now booting"),
  and the remote server delivered a **stable, non-black, non-test-pattern**
  256x192 frame -- i.e. a real frame computed by actual `RunFrame()`
  execution and delivered through `GPU::GetFramebuffers()` →
  `pushBottomFrame()` → the network client, not a static placeholder.
- With the interactive (`KEYINPUT`-reading) version of `arm9.c` and
  `interactive_pipeline_test.py` driving real UDP `ControllerState`
  packets through the actual network pipeline (`NetServer` →
  `RemoteServerBridge` → `EmuInstance::inputProcess()` → `NDS::SetKeyMask()`
  → CPU register read → visible backdrop-color change →
  `GPU::GetFramebuffers()` → `pushBottomFrame()` → client), each held
  button state produced **exactly one stable pixel value across 50
  consecutive samples**, with clean, immediate transitions between
  states and no noise -- a genuine, unambiguous confirmation that DS
  controls sent over the remote protocol affect a running program
  (SPEC.md section 20 criteria (4)-(8)).
- This also **conclusively resolved** two previously-open questions:
  - **Engine B is the "bottom" screen** delivered by
    `GPU::GetFramebuffers()` (the dynamic, input-reactive color was
    written to engine B's backdrop and that's what changed on screen;
    engine A's fixed color never appeared in the delivered frames).
  - **The delivered pixel format is BGRA8888**: holding "Up" (which the
    ROM maps to the blue component) changed byte offset 0; holding "B"
    (green component) changed byte offset 1; holding "A" (red component)
    changed byte offset 2; byte offset 3 stayed 255 (alpha). This
    resolves the earlier ambiguity from the static two-color ROM version,
    which used only a single sample per run and couldn't distinguish
    engine/order this cleanly.
- `stability_test.py` was run for a sustained period against the live
  patched melonDS process with continuous input and video traffic; see
  `docs/known-limitations.md` for the actual duration achieved and
  results (frame count, RSS growth, disconnects) in this sandboxed
  environment.

**Important caveat, stated plainly**: this is a fully original, minimal
homebrew program, not a commercial DS game -- SPEC.md section 19
explicitly forbids including commercial ROMs in this repository, and
sourcing/using real commercial software or firmware in this sandbox was
avoided for the same copyright reason. The *mechanism* verified here
(remote button state → `SetKeyMask()` → CPU-visible register → program
logic → framebuffer → network) is identical regardless of what program
is running; what hasn't been (and can't be, without real commercial
software or real Steam Deck/gamepad hardware) verified is a specific
commercial game's own input handling, or a human physically operating a
Steam Deck controller.

## Known environmental gotcha (not a bug in this project)

If `melonDS`'s `$HOME/.config` directory doesn't already exist (e.g. a
freshly created `$HOME` for a test run), melonDS's own `pathInit()` /
`Config::Load()` can fail non-obviously and pop a blocking
`QMessageBox::critical` dialog that never resolves under a headless X
server (Xvfb) with no way to click it. Pre-create the directory
(`mkdir -p "$HOME/.config"`) before launching melonDS in any headless/CI
context. This is a pre-existing melonDS behavior unrelated to the
remote-server patch and was deliberately left unpatched (out of scope,
see `host/melonds-patches/README.md`'s patch-boundary discussion).
