# DualDeck's vcpkg overlay ports for Cemu

Real Fedora build failures, 2026-08-26 (see `docs/known-limitations.md`'s
matching entry for the full writeup): building Cemu's own pinned vcpkg
dependency graph (`ports/` at Cemu's `builtin-baseline` commit,
`a4275b7eee79fb24ec2e135481ef5fce8b41c339`) failed on a real Fedora
machine in two unrelated ways neither this project's own CI runner
(`ubuntu-latest`, `release.yml`) nor a from-scratch Ubuntu build had ever
hit:

- **`glslang` 14.2.0** uses `uint32_t` (and similar fixed-width types) in
  code that doesn't itself `#include <cstdint>`, relying on it arriving
  transitively through some other standard header -- not guaranteed by
  the C++ standard, and Fedora's newer GCC/libstdc++ no longer provides
  it that way.
- **`sdl2` 2.30.3**'s PipeWire audio backend
  (`src/audio/pipewire/SDL_pipewire.c`) passes a `struct pw_proxy*`
  directly to `pw_node_enum_params()`, which current PipeWire headers
  declare as taking `struct pw_node*` -- a strict-typing tightening in
  PipeWire itself since this SDL release was cut, not a logic bug (the
  two types share `pw_proxy`'s `spa_interface` layout by design; an
  explicit cast is the documented way to call a typed method through a
  bound proxy).

Both are real upstream-adjacent issues triggered by build-machine header
versions vcpkg has no control over (it builds every dependency from
source against whatever system headers are present) -- not something
either project's own next release is guaranteed to have fixed by the
time anyone reading this hits it again.

## Why an overlay port, not a global compiler flag

Cemu's own `CMakeLists.txt` already points
`VCPKG_OVERLAY_PORTS` at `dependencies/vcpkg_overlay_ports_linux/` on
Linux (it already ships a handful of its own overlays there -- `cairo`,
`glm`, `gtk3`, `libpng`, as of this writing) -- vcpkg's documented,
supported way to substitute one port's build recipe without forking the
whole `vcpkg` submodule or weakening compiler warnings/flags for
anything else in the build. `scripts/lib/build_emulator.sh`'s
`build_cemu()` copies this directory's two subdirectories (`sdl2/`,
`glslang/`) into that same location on every fresh clone (`cp -r`, never
a directory replace -- so Cemu's own existing entries there are left
alone). Each is a full, working copy of the real vcpkg port at Cemu's
pinned baseline, plus one small, targeted fix:

- **`ports/sdl2/`** -- adds a third patch,
  `pipewire-node-proxy-cast.patch`, to the port's existing `PATCHES` list
  (alongside vcpkg's own `deps.patch`/`alsa-dep-fix.patch`, both carried
  over unmodified). One line changed in SDL's own source: an explicit
  `(struct pw_node *)` cast at the one call site that needs it.
- **`ports/glslang/`** -- adds `-include cstdint` to `VCPKG_CXX_FLAGS`
  before configuring, scoped to this one port's own compilation only
  (not Cemu's ~500 source files, not any of the ~107 other vcpkg
  dependencies) -- deliberately narrower than a build-wide `CXXFLAGS`
  override would be, since the actual missing include only affects this
  one dependency.

## Keeping these in sync with a future Cemu/vcpkg bump

If `scripts/lib/pinned_commits.sh`'s Cemu commit, or Cemu's own
`vcpkg.json` `builtin-baseline`, ever moves, re-verify these two ports
against the new baseline (`vcpkg.json`/`portfile.cmake`/patch files at
`https://github.com/microsoft/vcpkg/tree/<baseline>/ports/<port>`) rather
than assuming they still apply unchanged -- vcpkg's own port content at a
newer baseline may already have shifted underneath these copies, or may
have picked up an equivalent fix upstream, making this overlay
unnecessary for that port.

## Verification

Neither fix has been built end-to-end against a real Fedora machine by
whoever wrote this (this project's own development sandbox cannot
compile Cemu at all -- see `docs/known-limitations.md`). The `sdl2` patch
was generated as a real, syntactically valid unified diff against the
exact pinned SDL `release-2.30.3` source and is a minimal, well-understood
class of fix (an explicit pointer cast PipeWire's own client code
commonly needs for exactly this reason). If either one turns out to be
insufficient on a real build, the next debugging step is the actual
compiler error from that specific port's build log
(`buildtrees/<port>/*-out.log` under vcpkg's build tree), not a
broader flag or an assumption that the whole approach is wrong.
