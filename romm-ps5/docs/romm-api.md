# RomM REST API — reference for RomM-PS5

Status: assembled from RomM's public source, PRs, issues, and release notes
(rommapp/romm on GitHub). **No live RomM instance was reachable from the
research environment**, so nothing here is a substitute for checking a real
instance's `/openapi.json` before implementing the `rommapi` module — treat
every JSON field name below as a best-effort placeholder, not a contract.
See `docs/architecture.md` §3 and §8 for how this fits the overall design and
what specifically needs confirming.

Target: RomM 5.1.0+ (per project requirements). RomM 5.1.0 added Range
support for multi-file ROM downloads, which this app depends on for the ZIP
fallback path (architecture doc §5).

## Discovering the real spec

Every RomM instance serves:
- Swagger UI: `GET /api/docs`
- Redoc: `GET /api/redoc`
- Raw OpenAPI JSON: `GET /openapi.json`

Before implementing `rommapi`, fetch `/openapi.json` from a real 5.1+
instance and diff it against this document.

## Authentication

RomM-PS5 uses **Client API Tokens only** — never the username/password
`/api/token` OAuth2 flow, never a session cookie. The user creates the token
themselves in the RomM web UI (Administration → Client API Tokens) and pastes
it into RomM-PS5's server-setup screen.

- Format: `rmm_` followed by 64 hex characters.
- Header: `Authorization: Bearer rmm_<64 hex chars>`.
- Up to 25 tokens per RomM user account; each carries a subset of that
  user's scopes; no forced expiry (the creating user can set one in RomM).
- RomM authorization is scope-based; a token that can't read ROMs will fail
  those calls with a 401/403 regardless of how RomM-PS5 calls it — surface
  the RomM-provided error rather than guessing at a cause.
- RomM has an admin option to disable auth entirely on the download
  endpoints, intended for clients (e.g. an emulator) that fetch a ROM by
  bare URL. **RomM-PS5 never relies on this.** Every request — including
  every byte of every download — carries the bearer token.

## Endpoints

All paths below are relative to the instance base URL configured by the
user (e.g. `https://romm.example.com` or `http://192.168.1.10:3000`), under
`/api`.

### `GET /api/platforms`

Lists all platforms RomM knows about. RomM-PS5 filters this client-side to
the platform whose slug is `ps5` (RomM's platform slugs track IGDB's; IGDB's
own PlayStation 5 slug is `ps5`) and uses its `id` for all subsequent
`platform_id` filtering. If no `ps5` slug is present, show a clear
"no PS5 platform configured on this server" empty state rather than an error.

### `GET /api/roms`

Query parameters (from RomM's own PRs/issues):

| Param | Type | Purpose |
|---|---|---|
| `platform_id` | int | Scope to the PS5 platform's id |
| `search_term` | string | Search-box text |
| `collection_id` | int | Not used by RomM-PS5's MVP |
| `limit` / `offset` | int | Pagination |
| `order_by` | string | `name` for alphabetical sort (default) |

Expected response shape (**unconfirmed** — verify against live schema):
a paginated list of ROM summary objects, each carrying at least an `id`,
display `name`, whether it's `multi` (folder/multi-file) or single-file, a
size in bytes, and a cover-art reference. Exact field names (`fs_size_bytes`
is confirmed to exist and be sortable per RomM's docs; cover-art field names
are not confirmed) need to come from a live spec.

### `GET /api/roms/{id}`

Full detail for one ROM. RomM-PS5 needs, at minimum:
- Title / display name.
- Title ID, if RomM surfaces one for this platform (task requirement: "Title
  ID when available" — treat as optional/nullable in the UI, not assumed
  present).
- Whether it's single-file or multi-file (`multi` or equivalent).
- For multi-file ROMs: a `files[]` array, each entry with at least a file
  name/relative path and a size in bytes — this drives the recommended
  per-file download strategy (architecture doc §4). **This array's exact
  shape is the single most important thing to confirm against a live
  instance before writing the download code.**
- Cover-art image reference(s), ideally both a small (list view) and large
  (detail view) variant.

### `GET /api/roms/{id}/files/content/{file_name}`

Serves the raw bytes of one file belonging to a multi-file ROM. Ordinary
static-file serving — standard `Range: bytes=...` / `If-Range` apply, so this
is resumable the normal HTTP way. This is the endpoint the recommended
folder-game download strategy calls once per file in `files[]`.

### `GET /api/roms/{id}/content/{file_name}`

- **Single-file ROM**: serves that one file directly (`.ffpkg`, `.exfat`,
  `.ffpfs`, `.ffpfsc`, or a plain ROM file). Standard `Range` resume applies.
- **Multi-file ROM**: streams a ZIP of every file via nginx `mod_zip`,
  generated on the fly, never fully materialized server-side. `Range`
  support here is conditional on RomM having every file's CRC-32 cached when
  it built the `mod_zip` manifest (architecture doc §4–§5) — detect a `200`
  response to a `Range` request as "not actually resumable this time" and
  restart cleanly rather than assuming success.
- Query parameters: `file_ids` (comma-separated, select a subset of a
  multi-file ROM's files instead of all of them) and `hidden_folder` (nest
  the ZIP's contents under a hidden folder). RomM-PS5's MVP doesn't need
  either — full-ROM download only — but they're available if a later version
  adds selective-file download.

### `GET /api/roms/download`

Bulk download: `rom_ids`, `collection_id`, `virtual_collection_id`, or
`smart_collection_id` (exactly one) → a single ZIP of everything matched.
Not used by RomM-PS5's MVP (single-game downloads only, per the out-of-scope
list), documented here only so it isn't confused with the per-ROM endpoint
above.

## Field names still needing live-instance confirmation

Everything in this list blocks writing (not designing) the `rommapi` module:

- Exact cover-art field name(s) on the rom/platform objects, and whether
  they're full URLs or paths needing the instance base URL prepended.
- Exact shape of `files[]` on a multi-file ROM detail response (field names
  for relative path, size, and — if present — a hash useful for post-download
  verification).
- Whether RomM exposes a title ID field for PS5 ROMs specifically, and under
  what key.
- Whether individual file entries carry a hash (md5/sha1/crc) usable for the
  single-file `.ffpkg`/`.exfat` download's post-transfer verification step
  (architecture doc §6's "expected size and optional checksum" requirement).
- RomM's reported API/instance version field (for the app's own logging
  requirement: "RomM API version when available").
