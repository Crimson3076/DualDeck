# RetroDECK compatibility (Cemu only)

DualDeck must stay launcher-agnostic: it never detects or special-cases
EmuDeck, RetroDECK, Tender, Steam, or ES-DE. This document covers what
was actually verified about RetroDECK's own architecture, what DualDeck
already does that works under RetroDECK unmodified, what this pass adds,
and what's still genuinely unverified. Cemu (Nintendo Wii U) only, per
the current scope -- melonDS/Azahar are not covered here.

## RetroDECK's architecture, as verified against its real source

Investigated directly (not from RetroDECK's own docs -- readthedocs.io
was unreachable from this environment's network policy; every claim
below is instead sourced from `github.com/RetroDECK/RetroDECK` and
`github.com/RetroDECK/components` directly):

- RetroDECK (`net.retrodeck.retrodeck`) is one Flatpak. Its manifest
  never builds melonDS/Cemu/Azahar from source at all --
  `automation_tools/install_components.sh` extracts prebuilt "component"
  tarballs into `/app/retrodeck/components/<name>/` at Flatpak-build
  time. `RetroDECK/components/cemu/component_recipe.json` shows that
  tarball is itself produced by extracting the **stock, unpatched
  `info.cemu.Cemu` Flathub Flatpak** -- RetroDECK never compiles Cemu
  from `cemu-project/Cemu` either.
- `RetroDECK/components/cemu/component_launcher.sh` is a two-line
  `exec ".../bin/Cemu_relwithdebinfo" "$@"`, baked into RetroDECK's own
  read-only `/app` layer. **There is no Flatpak extension point for
  emulator components** (the manifest's only `add-extensions:` entry is
  `org.freedesktop.Platform.codecs_extra.i386`, for FFmpeg codecs) and
  `/app` is immutable at runtime -- so nothing a host file or a separate
  Flatpak installs can override that launcher or binary directly, with
  or without `--filesystem=host`.
- RetroDECK's actual game-launcher is its own ES-DE fork
  (`RetroDECK/ES-DE`). Its `es_find_rules.xml` checks
  `~/Applications/<Name>*.AppImage` **before** falling back to the
  bundled `component_launcher.sh`, for all three emulators -- the same
  convention EmuDeck's own (non-fork) ES-DE already relies on. `wiiu`
  and `n3ds` both default to "Cemu (Standalone)"/"Azahar (Standalone)"
  already; `nds` defaults to a libretro core, not standalone melonDS
  (out of scope here -- Cemu only).
- RetroDECK's Flatpak finish-args already grant, among others:
  `--filesystem=host`, `--device=all`, `--share=network`,
  `--socket=pulseaudio` + `--filesystem=xdg-run/pipewire-0`,
  `--socket=wayland`/`--socket=x11`, `--allow=bluetooth`,
  `--filesystem=/run/udev:ro`. Runtime: `org.kde.Platform//6.10`, SDK:
  `org.kde.Sdk//6.10`.

## What this means for DualDeck: no true "RetroDECK component" today

A genuine drop-in replacement of RetroDECK's own Cemu component isn't
possible without upstream RetroDECK changes -- there's no extension
point, and `/app` can't be overridden at runtime. Rebuilding all of
RetroDECK from a forked manifest was ruled out: it fights every upstream
RetroDECK update forever, for a change scoped to "Cemu only." So this
pass ships two things:

1. **The AppImage path (already implemented, commit `f48094c` on
   `main`)** -- `scripts/emudeck-replace-in-place.sh` installs the same
   patched Cemu AppImage this project already ships to
   `~/Applications/Cemu.AppImage` whenever no EmuDeck install exists to
   overwrite. Because RetroDECK's own ES-DE fork checks that exact path
   first, this already lands DualDeck's patched Cemu in front of
   RetroDECK's bundled, unpatched one, using the `wiiu` system's
   existing "Cemu (Standalone)" default -- **no RetroDECK-side
   reconfiguration needed.** This is the production path.
2. **A RetroDECK-component-shaped build (new, this pass)** --
   `scripts/build-retrodeck-cemu-component.sh` builds the identical
   pinned Cemu commit + DualDeck patch and packages it as a
   `component_launcher.sh` + `bin/`+`lib/` tarball, for (a) an advanced
   user doing their own local RetroDECK Flatpak rebuild, and (b) a
   concrete artifact to hand RetroDECK maintainers alongside the
   upstream proposal (see below) -- **not** something RetroDECK's
   existing automation consumes unmodified today, since RetroDECK's own
   pipeline extracts from Flathub's stock Cemu Flatpak rather than
   building from source at all.

Both packages are built from the exact same inputs
(`scripts/lib/pinned_commits.sh`'s `CEMU_COMMIT`, currently
`a6fb0a48eb437a8a41c13b782ac8ae0433bf8f98` = tag `v2.6`, and
`host/cemu-patches/0001-remote-server-integration.patch`), via the same
`build_cemu()` (`scripts/lib/build_emulator.sh`) and the same dependency
bundling (`scripts/lib/appimage_pack.sh`'s `_stage_bundle_payload()`),
so they can never silently drift onto different Cemu builds.

### Why a separate script, not a change to `build-release.sh`

`scripts/build-retrodeck-cemu-component.sh` is deliberately **not**
wired into `scripts/build-release.sh` or `.github/workflows/release.yml`.
The existing release pipeline is the working, verified path (AppImage +
EmuDeck) -- this is an experimental, opt-in artifact for a launcher this
project cannot yet test against real hardware. Keeping it a standalone
script means it can never regress the existing release build if it
breaks, and someone who wants the RetroDECK-shaped artifact runs it
explicitly.

## IPC: already package-neutral, no changes needed

The Cemu patch's `AdapterIpcClient` (see `host/cemu-patches/README.md`)
already connects to a plain Unix domain socket at
`$XDG_RUNTIME_DIR/dualdeck/adapter.sock` (falling back to
`$HOME/.cache/dualdeck/adapter.sock`), created mode `0700`
(`adapter_sdk/src/socket_path.cpp`). `scripts/lib/adapter_socket_probe.sh`
(shared by every launcher: EmuDeck AppImage, standalone, and now the
RetroDECK component launcher) probes that same well-known path first and
only spawns a private, ephemeral Host Service if nothing is listening
there yet. Nothing in this project greps for "EmuDeck"/"RetroDECK"/
"Steam"/"ES-DE"/"Tender" anywhere, and `flatpak-spawn` is never used.

Because RetroDECK's Flatpak already grants `--filesystem=host`, the
sandboxed Cemu component (were one to exist) would see the exact same
host path Cemu's out-of-process AppImage launcher already uses -- no
new permission should be required. **This specific claim is unverified
against a real RetroDECK sandbox** -- see the test plan below.

## Permissions actually needed (for the upstream proposal and for a
local Flatpak rebuild)

If a Cemu component build is ever integrated into RetroDECK's own
manifest, it needs nothing beyond what RetroDECK's Flatpak already
requests for other components:

| Purpose | Flatpak permission | Already in RetroDECK's manifest? |
|---|---|---|
| Vulkan rendering | `--device=all` | Yes |
| Display | `--socket=wayland`, `--socket=x11` | Yes |
| Audio (PipeWire) | `--socket=pulseaudio`, `--filesystem=xdg-run/pipewire-0` | Yes |
| Controller input | `--device=all`, `--filesystem=/run/udev:ro`, `--allow=bluetooth` | Yes |
| DualDeck IPC (local Unix socket under `$XDG_RUNTIME_DIR`) | `--filesystem=host` (broad) or a narrower `--filesystem=xdg-run/dualdeck:create` | `--filesystem=host` already covers it |
| Client<->host network protocol | `--share=network` | Yes |

No new Flatpak permission grant is needed for Cemu specifically. If
RetroDECK's own maintainers prefer not to rely on the broad
`--filesystem=host` grant already present for other reasons, the
narrower `--filesystem=xdg-run/dualdeck:create` is the one addition
DualDeck's IPC would need -- worth naming explicitly in the upstream
proposal so it isn't assumed silently.

## Building the component artifact

```
./scripts/build-retrodeck-cemu-component.sh
```

Produces, under `retrodeck-component-out/` (override with
`BUILD_RETRODECK_COMPONENT_OUTPUT_DIR`):

- `dualdeck-cemu-retrodeck-component-linux-x86_64.tar.gz` -- top-level
  `cemu/` directory: `component_launcher.sh` (executable, same
  probe-shared-socket-else-spawn-private-daemon logic as the AppImage's
  own AppRun), `usr/bin/cemu` (the patched binary, renamed from Cemu's
  own `Cemu_release`), `usr/bin/dualdeck-host-service`,
  `usr/bin/resources/`, `usr/bin/gameProfiles/`, `usr/lib/*.so` (bundled
  runtime dependencies, glibc excluded -- see
  `scripts/lib/appimage_pack.sh`'s `bundle_library_dependencies()`).
- `BUILD_MANIFEST.json` -- records the exact Cemu upstream commit/tag,
  the patch file's own sha256, the CMake build type and version flags,
  the vcpkg overlay ports applied, the Flatpak runtime/SDK this artifact
  targets (`org.kde.Platform//6.10` / `org.kde.Sdk//6.10`), the DualDeck
  repo commit it was built from, and the build host/timestamp.
- `SHA256SUMS` -- checksums for both files above.

## Install / rollback / removal

### Production path: patched AppImage (recommended, already implemented)

Install (same tool EmuDeck installs already use):

```
./scripts/emudeck-replace-in-place.sh
```

With no existing EmuDeck Cemu install found, this installs the patched
AppImage fresh to `~/Applications/Cemu.AppImage` -- exactly where
RetroDECK's ES-DE looks first for the `wiiu` system's "Cemu
(Standalone)" launcher. No RetroDECK reconfiguration needed for Cemu.

Rollback/removal:

```
./scripts/emudeck-replace-in-place.sh --restore
```

Removes the fresh install cleanly (there is no prior original to restore
in this case -- `--restore` detects that from the install manifest and
deletes rather than restoring a placeholder). RetroDECK's ES-DE then
falls back to its own bundled, unpatched Cemu component automatically,
with **no separate RetroDECK-side uninstall step**.

### Advanced path: local RetroDECK Flatpak rebuild with the component tarball

This is for someone building their own local RetroDECK Flatpak (e.g. to
test against the upstream proposal below), not a supported install path
for a stock RetroDECK Flatpak install -- there is nowhere writable in a
stock install to place this.

1. Build the tarball: `./scripts/build-retrodeck-cemu-component.sh`.
2. Verify: `sha256sum -c SHA256SUMS` inside `retrodeck-component-out/`.
3. In a local clone of `RetroDECK/RetroDECK`, replace the `cemu`
   component-tarball source URL/checksum in whatever step consumes
   `RetroDECK/components/cemu/component_recipe.json`'s output with this
   artifact's path and sha256 (exact mechanics depend on RetroDECK's own
   `install_components.sh` at the version being rebuilt -- read that
   script directly before doing this; do not guess).
4. Rebuild RetroDECK's Flatpak locally (`flatpak-builder`) and install
   it to a **separate, non-default** app ID or a throwaway user, never
   over a production RetroDECK install.

Rollback/removal: uninstall the locally-built Flatpak
(`flatpak uninstall --user <local-app-id>`); the stock, Flathub-published
RetroDECK install is completely untouched by any of this, since it never
shares a Flatpak installation prefix with a locally built one unless one
is deliberately made to.

### Isolated RetroDECK test environment

Do not install RetroDECK on the production HTPC that already runs
EmuDeck. Recommended: a separate user account, a spare/loaner machine, or
a VM with GPU passthrough. **These commands are for review, not yet
run** -- confirm with the user before executing any of them for real:

```
flatpak install --user flathub net.retrodeck.retrodeck
flatpak run net.retrodeck.retrodeck
```

A small test library (1-2 Wii U titles already owned, legally dumped) is
enough for the milestone verification below -- do not commit ROMs,
firmware, keys, or save files to this repository at any point.

## Milestones

1. **RetroDECK -> patched Cemu -> game launches normally, DualDeck
   disabled.** Confirms the AppImage lands where ES-DE actually looks
   and Cemu itself runs unmodified-looking (no `CEMU_REMOTE_ENABLE`).
2. **RetroDECK -> patched Cemu -> DualDeck connects and streams the
   GamePad screen.** Confirms the adapter socket is reachable from
   inside RetroDECK's Flatpak sandbox with its *default* permissions
   (no `flatpak override` needed) -- or identifies exactly which single
   permission is actually missing, if any.
3. **Tender discovers and launches the component through RetroDECK
   normally**, with no Tender-specific DualDeck code required.

## Verification plan (compare against the known-good AppImage/EmuDeck setup, same hardware, same game)

Not "does it launch" -- side-by-side comparison on:

- Vulkan rendering and FPS
- Frame pacing
- PipeWire and audio
- Controller input (including the face-button mapping fix and touch, see
  `host/cemu-patches/README.md`)
- Capture and encoding (JPEG/H.264 path, see `docs/known-limitations.md`)
- DualDeck connection and end-to-end latency
- Dropped frames
- CPU and GPU usage
- Shutdown and cleanup (confirm no orphaned `dualdeck-host-service` --
  see the `AppRun`/`component_launcher.sh` non-`exec` foreground-run
  reasoning in `scripts/lib/apprun_templates.sh`)
- Repeated launches (confirm `probe_or_spawn_adapter_socket()`'s
  reconnect-vs-fresh-spawn behavior is correct every time)
- Network interruption
- Paths containing spaces and parentheses (RetroDECK's own
  `~/.var/app/net.retrodeck.retrodeck/...` paths, and any user library
  path)

This requires real hardware and a real RetroDECK install this
development environment does not have -- the scripts/docs above are
ready to run, but every checkbox in this section is **currently
unverified**.

## What's verified vs. not, as of this pass

**Verified**: RetroDECK's real component/launcher architecture (read
directly from source, cross-referenced against a second independent
research pass); the existing AppImage/ES-DE-fallback path's logic
(commit `f48094c`, tested against fake `$HOME`s, not real RetroDECK);
`pack_retrodeck_component_tarball()`'s packaging shape
(`tests/retrodeck_component_pack_test.py`, real tarball structure,
executable bits, and content preservation checked -- no real Cemu binary
or appimagetool involved); `bash -n` on all new/changed scripts.

**Not verified**: this artifact has never been built end-to-end (Cemu's
own vcpkg-based dependency graph needs unrestricted network access this
project's own sandbox doesn't have -- same constraint documented
throughout `host/cemu-patches/README.md`); nothing here has run inside
an actual RetroDECK Flatpak sandbox; the `--filesystem=host`-implies-
IPC-reachability claim above is reasoned from the manifest, not
confirmed live; Tender compatibility is entirely unverified.

## Upstream proposal

A draft message for the RetroDECK maintainers (proposing either a
build-from-source option in `components/cemu/component_recipe.json`, or
a real component extension point) will be written up separately and
shown before anything is posted to `RetroDECK/RetroDECK` or
`RetroDECK/components` -- no issue or PR will be opened without explicit
approval first.
