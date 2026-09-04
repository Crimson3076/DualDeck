# RomM-PS5 — Phase 1 Feasibility & Architecture

Status: **Phase 1 (research + architecture) only.** No downloader functionality
exists yet. Nothing in this document has been run on a real PS5. Where a claim
is backed by something actually compiled or tested during this phase, it says
so explicitly; everything else is a plan, not a result.

## 0. Why this document exists

RomM-PS5 is a from-scratch PS5 homebrew app: browse a user's own
[RomM](https://github.com/rommapp/romm) library over its REST API and download
legally-owned PS5 backups onto a jailbroken console. Before writing the app,
the brief called for confirming the SDK, the RomM API shape, and the two
hardest technical questions (incremental ZIP extraction, resumable downloads)
*before* committing to an implementation. This is that work.

Everything here was produced by actually downloading the candidate SDK and
cross-compiling real sample payloads in this session (see §7), and by reading
RomM's own source, release notes, and issue tracker (no live RomM instance was
reachable from this environment, so anything that needs a running instance's
`/openapi.json` is flagged as unverified in §8).

## 1. SDK and library survey

### Toolchain: `ps5-payload-dev/sdk`

[ps5-payload-dev](https://github.com/ps5-payload-dev) (John Törnblom et al.)
is the actively maintained, GPLv3-licensed open-source PS5 payload SDK. It was
last updated within days of this research (Aug 2026), and its `pacbrew-repo`
package index and sample tree are both current.

**Verified in this session** (see §7 for exact commands/output):
- Downloaded `ps5-payload-sdk.zip` directly from the SDK's GitHub Releases
  (no Docker image needed — the toolchain is a self-contained
  `clang-18`/`lld-18`-based cross toolchain called `prospero-clang`).
- Cross-compiled `samples/hello_world` to a PIE x86-64 ELF with a plain
  `make`.
- Cross-compiled `samples/http2_get` — this links against `libSceNet.so`,
  `libSceSsl.so`, and `libSceHttp2.so`, confirming the linker resolves
  Sony's own networking/TLS/HTTP2 system libraries as import stubs.
- Cross-compiled `samples/hello_cmake` (C and C++) using the SDK's bundled
  `toolchain/prospero.cmake`, confirming a CMake-based build (matching this
  monorepo's existing convention) works, not just raw `make`.

None of the resulting ELFs have been run on a PS5 — there is no console
available in this environment. "Compiles" is the only claim being made.

### What the SDK provides directly

The SDK ships a FreeBSD-derived libc/libc++/libpthread (the PS5's "Prospero"
OS kernel is FreeBSD-based) plus **stub import libraries for Sony's own
system libraries**, extracted from the console's own firmware. These are not
reimplementations — the real code runs from the PS5's own OS at runtime; the
`.so` stubs just let the cross-linker resolve the symbols. Confirmed present
in `target/lib`:

| Library | Relevance |
|---|---|
| `libSceNet.so`, `libSceNetCtl.so` | sockets, network interface/connectivity status |
| `libSceHttp.so`, `libSceHttp2.so` | native HTTP/1.1 and HTTP/2 client |
| `libSceSsl.so` | native TLS |
| `libScePad.so` | DualSense controller input (native, not SDL) |
| `libSceSystemService.so`, `libSceRegMgr.so` | system info — **candidate source for firmware version reporting** (§ logging requirement) |
| `libSceNotification.so` | native OS toast notifications — candidate for "download complete" |
| `libSceImeDialog.so`, `libSceKeyboard.so` | on-screen keyboard, for the one unavoidable text-entry case (server URL / token) without requiring a physical keyboard |

`sys/statvfs.h` is present in the bundled headers, so free-space reporting
(`statvfs()`) is standard POSIX, not something requiring a special API.

**Gap found:** no `scePad*` function prototypes are bundled in this base SDK
zip (only the linkable `.so` stub, no header). Community PS5 homebrew gets
DualSense input one of two ways: hand-declare the well-known `scePad*` ABI
directly, or use the `ps5-payload-dev/SDL` port, which already wraps it as
SDL2's `GameController` API. **This SDK port was not built or tested in this
session** (see §8, open risk #1) — it wasn't fetched, so "SDL2 gives us
controller input" is a documented plan based on the port's stated purpose and
its use by `Rufidj/Nativehbl` (§2), not a verified-in-this-repo fact.

### Everything else, via PacBrew

`ps5-payload-dev/pacbrew-repo` is a package repository for jailbroken PS5s
covering the remaining gaps. Relevant packages confirmed to exist in the repo
listing (not fetched/built in this session):

| Need | Packages available |
|---|---|
| TLS (userland alternative to SceSsl) | `openssl`, `libressl`, `mbedtls`, `bearssl` |
| HTTP client (userland alternative to SceHttp2) | `curl` |
| ZIP / archive | `libzip`, `libarchive`, `libminizip`, `bzip2`, `lz4` |
| JSON | `jansson`, `json-c` |
| PNG/JPEG (cover art) | `libpng`, `libjpeg-turbo`, `giflib`, `SDL2_image` |
| Fonts (UI text) | `freetype`, `fontconfig`, `harfbuzz` |
| UI/input/rendering | `SDL2`, `SDL2_gfx`, `SDL2_ttf` |

### Feature confirmation table (Phase 1 item 3)

| Requirement | Status | Basis |
|---|---|---|
| HTTP and HTTPS | **Confirmed compiles** | `http2_get` sample links `libSceNet`/`libSceHttp2`/`libSceSsl` cleanly (§7) |
| TLS certificate validation | Available, **behavior unverified** | SceSsl stub links; whether it validates against a usable trust store, and whether it accepts a custom CA, is unknown without a console. Userland `mbedtls`/`openssl` via PacBrew is the fallback if SceSsl proves inflexible (see §6 TLS abstraction) |
| JSON parsing | Available, not yet integrated | `jansson`/`json-c` via PacBrew; not in base SDK |
| ZIP extraction | Available, not yet integrated | `libzip`/`libarchive`/`minizip` via PacBrew — needed only for the fallback bulk-ZIP download path (§4) |
| Large-file writing | **Confirmed available** | Standard POSIX `open`/`pwrite`/`lseek` over FreeBSD libc; 64-bit `off_t` confirmed in bundled headers |
| Controller input | Available, **not built/tested this session** | `libScePad.so` stub present but headerless; `ps5-payload-dev/SDL` wraps it. Biggest open item — see §8 risk #1 |
| PNG/JPEG artwork | Available, not yet integrated | `libpng`/`libjpeg-turbo`/`SDL2_image` via PacBrew |
| Persistent configuration | **Confirmed available** | Plain file I/O to a writable `/data/...` path; no special API needed |
| Free-space reporting | **Confirmed available** | `sys/statvfs.h` present in bundled SDK headers |

## 2. Structural reference application

**[`Rufidj/Nativehbl`](https://github.com/Rufidj/Nativehbl)** (Native
Homebrew Launcher) is the closest existing open-source PS5 app to what
RomM-PS5 needs structurally:

- Built on SDL2 + `ps5-payload-sdk` (same toolchain chosen above).
- Two-stage ELF design: a small bootstrap ELF that launches a UI ELF as a
  "bigapp," with the UI writing a launch request the bootstrap consumes.
  RomM-PS5 doesn't need the two-stage split (it isn't launching other
  homebrew), but the pattern of "UI scans/acts, writes state, a small
  privileged step consumes it" is directly reusable for the
  download-then-hand-off-to-ShadowMountPlus flow.
- Controller mapping convention (D-pad navigate, Cross confirm, Square
  refresh/rescan, Circle back/exit) — adopting the same mapping keeps
  RomM-PS5 consistent with what PS5 homebrew users already expect.
- Software-rendered UI with JSON theme manifests, not PNG-asset-heavy —
  a reasonable starting point for RomM-PS5's own screens, though cover art
  *display* (not theming) is a hard requirement here that Nativehbl doesn't
  need, so `SDL2_image` (PacBrew) is still required on top of this pattern.

**Caveat:** Nativehbl launches local homebrew from disk; it does not talk to
a network API. No existing open-source PS5 app was found that both browses a
remote REST catalog *and* downloads to disk — that combination is the novel
part of this project. `ps5-payload-dev/websrv` (a payload that *serves* HTTP,
not consumes it) was reviewed as a secondary reference for how PS5 payloads
structure file I/O and directory scanning, but it's a server, not a client,
so its relevance is limited to filesystem-handling patterns.

## 3. RomM API (Phase 1 items 4–5)

RomM is FastAPI-based; every instance exposes Swagger UI at `/api/docs`,
Redoc at `/api/redoc`, and the raw spec at `/openapi.json`. **No running RomM
instance was reachable from this sandboxed environment**, so the table below
is assembled from RomM's own GitHub source, PRs, issues, and release notes
rather than a fetched spec — treat every row as "needs confirmation against
`https://<your-instance>/openapi.json` before implementation," which is
listed as the first concrete task of the next phase.

### Authentication

- RomM's authorization is scope-based; every API call maps to one or more
  scopes.
- **Client API Tokens** (Administration → Client API Tokens in the RomM web
  UI — not something this app creates) are the documented, correct
  mechanism for a long-lived companion app. Format: `rmm_` + 64 hex
  characters. Sent as `Authorization: Bearer rmm_<token>`. Up to 25 tokens
  per user; each carries a subset of that user's scopes; no forced
  expiration (the user can set one).
- RomM also supports session cookies, HTTP Basic, and OAuth2 short-lived
  JWTs (`/api/token`, 15-minute access / 2-week refresh) — none of these are
  appropriate for this app; **Client API Token is the only auth path
  RomM-PS5 implements.**
- RomM has an admin-level setting to skip auth entirely on the download
  endpoints (documented as existing for clients — e.g. emulators — that
  load a ROM by bare URL and can't carry a bearer header). **RomM-PS5 must
  never rely on or suggest enabling this.** Every request this app makes,
  downloads included, always carries the bearer token.

### Endpoints required

| Purpose | Endpoint (best available evidence) | Notes |
|---|---|---|
| List platforms | `GET /api/platforms` | Filter client-side to the `ps5` slug (RomM's platform slugs track IGDB's; IGDB's own slug for PlayStation 5 is `ps5`) |
| List/search ROMs | `GET /api/roms?platform_id=&search_term=&limit=&offset=&order_by=` | `platform_id` scopes to PS5; `search_term` for the search box; `order_by=name` for alphabetical sort |
| ROM detail (metadata + file list) | `GET /api/roms/{id}` | Expected to include the multi-file `files[]` array (id, file_name, size) needed to drive per-file downloads — **schema unconfirmed, verify against live `/openapi.json`** |
| Cover art | Field(s) on the rom/platform object (`path_cover_*`/similar) served as an image URL | Exact field names unconfirmed — verify against live schema |
| Single-file / per-file download | `GET /api/roms/{id}/files/content/{file_name}` | Serves raw bytes for one file inside a multi-file ROM; standard static-file serving, so standard HTTP `Range` applies |
| Whole-ROM download | `GET /api/roms/{id}/content/{file_name}` | For a single-file ROM, serves that file. For a multi-file ROM, streams a ZIP of all its files via nginx `mod_zip` (see §4). Accepts `file_ids` to select a subset and `hidden_folder` to nest the ZIP's contents |
| Bulk (multi-ROM/collection) download | `GET /api/roms/download?rom_ids=` / `?collection_id=` | Not needed for RomM-PS5's MVP (single-game downloads only), noted for completeness |

## 4. Can a streamed folder-ZIP be extracted incrementally? (Phase 1 item 6)

**Confirmed mechanism, confirmed conditional answer: yes, but only when RomM
already knows each file's CRC-32 — and that's also exactly the condition
that determines whether the ZIP is resumable (§5).**

RomM's multi-file ROM ZIP is generated by nginx's `mod_zip` module — "the
bundled nginx is built with mod_zip, which streams a zip archive over HTTP
without ever materialising it on disk." `mod_zip` works from a manifest RomM
supplies internally: for each file, a CRC-32, a size, and a location. Two
cases:

1. **CRC-32 known** (the normal case — RomM computes/caches file hashes
   during scanning): `mod_zip` writes each entry's local file header with
   the *real* CRC-32 and *real* size already filled in — no zip
   "data descriptor" trailer is needed, because nothing has to be
   discovered after the fact. That means a client reading the HTTP response
   as a byte stream can: read the 30-byte local file header, read the
   filename, then read exactly `compressed_size` bytes of file data straight
   to a destination file, verify the CRC-32 as it streams, and move to the
   next local file header — never needing to seek back for the ZIP central
   directory (which arrives last, and isn't needed for stored/uncompressed
   entries with valid local headers). This is the **incremental extraction
   RomM-PS5 needs**, and `mod_zip` is documented to support it.
2. **CRC-32 unknown** (`mod_zip`'s manifest can substitute `-` for the
   checksum when the CRC isn't known ahead of time): the local header uses a
   deferred/streaming form (bit-3 general-purpose flag, data written before
   its own checksum is known) — still streamable entry-by-entry, but without
   a CRC to verify against until the trailing descriptor, and — per
   `mod_zip`'s own docs — **`Range` support is disabled entirely** in this
   case.

This has not been confirmed against an actual byte-for-byte response from a
live RomM 5.1+ instance — RomM's exact `mod_zip` configuration (whether it
always has CRCs cached, whether compression is ever enabled instead of
stored) isn't documented and needs a real capture before the extractor is
trusted. That capture is listed as the first implementation task in the next
phase (§8).

### Recommended primary strategy: skip ZIP parsing entirely

Given the `/api/roms/{id}/files/content/{file_name}` per-file endpoint
exists (§3), the simplest and most robust path for a folder-based game is:

1. Fetch the ROM's metadata, read its `files[]` list (relative path + size
   per file).
2. Download each file individually to
   `<destination>/<Game Name>.download-partial/<relative path>`, using plain
   HTTP `Range` resume per file (ordinary static-file serving, not gated on
   RomM having a cached CRC).
3. On success, verify `sce_sys/param.json` exists, then atomically rename
   the directory to its final name.

This sidesteps ZIP parsing altogether for the common case — no custom
zip-stream parser, no ZIP-specific path-traversal/symlink surface (§6), and
resumability that doesn't depend on RomM's CRC-caching behavior. The
`mod_zip` incremental-extraction path above is kept as a **documented
fallback** (e.g., if a future RomM version removes the per-file endpoint, or
a specific deployment restricts it), not the primary implementation.

## 5. Resumable downloads (Phase 1 item 7)

| Download type | Resumable? | Basis |
|---|---|---|
| Regular single-file download (`.ffpkg`, `.exfat`, `.ffpfs`, `.ffpfsc`, or one file via `/files/content/`) | **Yes** | Ordinary static file serving through nginx; standard `Range`/`If-Range` support |
| Multi-file folder game via the **per-file** strategy (§4 recommended) | **Yes, per file** | Same as above, applied file-by-file |
| Multi-file folder game via the **whole-ZIP** fallback strategy | **Conditional** | RomM 5.1.0's release notes explicitly list "Support Range requests for multi-file ROM downloads" as a new feature — but `mod_zip` itself only supports `Range` when every entry's CRC-32 was known when the manifest was built (§4). When it isn't, `mod_zip` disables `Range` for that response entirely |

Per the task's own instruction: if a given dynamically-generated ZIP response
doesn't honor a `Range` request (server returns `200` with the full body
instead of `206 Partial Content`), RomM-PS5 must detect that from the status
code and **restart the transfer cleanly**, and say so in the UI, rather than
silently treating it as resumed. This is a hard requirement of the download
manager's state machine, not an edge case to skip.

## 6. Proposed architecture

Modules, matching the required "clear separation between networking, RomM
API, storage, extraction and UI":

```text
romm-ps5/
  src/
    net/          Thin wrapper over SceHttp2/SceNet/SceSsl (or a curl/mbedtls
                   fallback behind the same interface — TLS backend is an
                   interface, not a hard dependency on one library, because
                   SceSsl's custom-CA support is unverified, §1).
    rommapi/       RomM REST client: auth header injection, platform/rom
                   listing, search, per-file and whole-ROM download requests.
                   No knowledge of PS5 filesystem paths.
    storage/       Destination enumeration (which of the fixed candidate
                   paths exist + are writable), free-space queries (statvfs),
                   atomic rename-on-completion, `.download-partial`/`.part`
                   staging.
    extract/       Only the ZIP fallback path (§4) lives here: streaming
                   local-file-header parser, CRC verification, path
                   sanitization.
    download/      The download-state machine: progress, speed, ETA, cancel,
                   retry, Range-resume-or-restart decision, calls into
                   rommapi + storage + extract.
    ui/            SDL2 + controller input + cover art rendering. Talks only
                   to rommapi (read) and download (act) — never touches
                   sockets or the filesystem directly.
  toolchain/       SDK fetch/setup instructions (this phase's deliverable).
  docs/            This document + the RomM API reference.
```

### Security posture carried into the design (not yet implemented)

These map directly to the task's threat list and are structural decisions,
not yet code:

- **Path traversal / absolute paths / `../` / symlinks**: every filename
  used to build a destination path — whether from a `files[]` API response
  or (fallback path) a ZIP local file header — is normalized and rejected if
  it resolves outside the destination directory. This applies identically to
  both download strategies in §4.
- **Partial-vs-complete confusion**: enforced structurally by the
  `.download-partial`/`.part` staging directories/files in §4/§5 — nothing
  is renamed to its final name until validation (`sce_sys/param.json`
  presence for folders; expected size, and hash if RomM exposes one, for
  single-file images) passes.
- **Integer overflow on large files**: 64-bit `off_t` throughout (confirmed
  available, §1); no size or offset ever stored in a 32-bit type.
- **Disconnects / space exhaustion mid-transfer**: the download state
  machine (§ `download/`) treats "wrote fewer bytes than expected" and
  "write failed (ENOSPC)" as first-class transitions to a Failed state with
  the partial directory left in place for Retry, never silently discarded.

## 7. What was actually verified in this session

Exact evidence, so this phase's claims are checkable:

```text
$ curl -sSL -o ps5-payload-sdk.zip \
    https://github.com/ps5-payload-dev/sdk/releases/latest/download/ps5-payload-sdk.zip
HTTP 200, 8,814,217 bytes, valid zip archive

$ export PS5_PAYLOAD_SDK=.../sdk/ps5-payload-sdk
$ make -C samples/hello_world
prospero-clang -Wall -Werror -g -o hello_world.elf main.c
$ file hello_world.elf
hello_world.elf: ELF 64-bit LSB pie executable, x86-64, ... dynamically linked

$ make -C samples/http2_get
prospero-clang -Wall -Werror -g -lSceNet -lSceSsl -lSceHttp2 -o http2_get.elf main.c
$ file http2_get.elf
http2_get.elf: ELF 64-bit LSB pie executable, x86-64, ... dynamically linked

$ cmake -DCMAKE_TOOLCHAIN_FILE=.../toolchain/prospero.cmake ..
$ make   # in samples/hello_cmake/build
[100%] Built target hello_c
[100%] Built target hello_cpp
```

This confirms: the SDK is fetchable without Docker, `prospero-clang`/`lld`
cross-compile working PIE ELFs, `libSceNet`/`libSceSsl`/`libSceHttp2` link
without extra setup, and the CMake toolchain file works for both C and C++.
It does **not** confirm anything runs correctly on a PS5 — there is no
console in this environment. It also does not confirm SDL2, ScePad, or any
PacBrew package builds — those weren't fetched in this session.

## 8. Open risks / unverified items going into implementation

Ranked by how much they could change the design:

1. **Controller input is unbuilt.** The `ps5-payload-dev/SDL` port (or a
   hand-rolled `scePad*` binding) needs to actually be fetched and compiled
   next — this is step 3 of the development sequence and the single biggest
   remaining unknown from Phase 1.
2. **RomM's actual JSON schema is unconfirmed.** Field names for the `files[]`
   array, cover-art URLs, and file hashes in §3's table are inferred from
   RomM's PRs/issues/release notes, not a fetched `/openapi.json`. The first
   task of the next phase should be pointing this app (or even just `curl`)
   at a real RomM 5.1+ instance and diffing the assumed schema against
   reality before writing the `rommapi` module.
3. **`mod_zip`'s exact behavior on RomM specifically** (does it always have
   CRCs cached? is compression ever non-stored?) is inferred from `mod_zip`'s
   general documentation, not a captured RomM response. Since the
   recommended primary strategy (§4) avoids ZIP parsing for the common case,
   this only blocks the fallback path, not the MVP.
4. **SceSsl's TLS validation behavior** (system trust store contents,
   whether a custom CA can be injected) is unverified. If it can't do what
   the MVP's "optional custom CA certificate support" requirement needs, the
   `net/` module's TLS backend swaps to a PacBrew `mbedtls`/`openssl` build
   — which is exactly why §6 designed that as a swappable interface up
   front rather than a hard dependency on SceSsl.
5. **ShadowMountPlus's exact scan-path list matches the task's requested
   destination list** (`/data/homebrew`, `/data/etaHEN/games`,
   `/mnt/usb0`–`/mnt/usb7` with both subfolders) — confirmed by reading
   ShadowMountPlus's own README, which also documents `/mnt/ext0`/`/mnt/ext1`
   as additional scan roots RomM-PS5's MVP doesn't need to expose. No
   documented HTTP/IPC rescan API exists; the only documented, safe trigger
   is appending a line to `/data/shadowmount/manual.lst`, which is what the
   MVP's "Rescan" button should do — not anything reverse-engineered.
