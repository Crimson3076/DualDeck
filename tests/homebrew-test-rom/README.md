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
  graphics) and writes a distinctive color into each engine's BG palette
  entry 0 (the "backdrop" color shown when no BG layer covers a pixel),
  then loops forever. No tiles, no sprites, no font -- this is the
  simplest possible way to get the emulated screen showing something
  other than its power-on default, using only two register writes.
- `arm7.c`: an empty infinite loop. NDS direct boot jumps both CPUs to
  their entry points; ARM7 just needs to exist and not crash.
- `build_rom.py`: hand-packs a valid 4096-byte `NDSHeader` (matching
  melonDS's own compiled struct layout, verified with an `offsetof()`
  dumper against melonDS's actual `NDS_Header.h` rather than assumed) plus
  the two compiled binaries into a `.nds` file that satisfies melonDS's
  `ValidateROM()` checks (see the script's docstring for the exact rules).

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

## What this proved (and didn't)

Used to verify `host/melonds-patches`'s video-capture path end-to-end:
built and direct-booted successfully (confirmed via melonDS's own log
output: "Inserted cart with game code: ####", "Game is now booting"), and
the remote server delivered a **stable, non-black, non-test-pattern**
256x192 frame that was consistent across repeated reads and across
process restarts -- i.e. a real frame computed by actual `RunFrame()`
execution and delivered through `GPU::GetFramebuffers()` →
`pushBottomFrame()` → the network client, not a static placeholder.

**What it didn't conclusively establish**: the exact pixel channel order
of the delivered frame. Writing distinctly different R/G/B palette values
across two separate runs produced the same output color both times,
which doesn't fit a simple "it's just RGBA" or "it's just BGRA" story on
its own -- this needs further investigation (possibly a VRAM-bank-enable
prerequisite for backdrop rendering that this minimal program doesn't set
up, or some other interaction not yet understood) rather than being
declared solved. See `docs/known-limitations.md`.

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
