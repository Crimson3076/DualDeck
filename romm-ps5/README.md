# RomM-PS5

An open-source PS5 homebrew app to browse a user's own
[RomM](https://github.com/rommapp/romm) library and download legally-owned
PS5 backups directly onto a jailbroken console over RomM's REST API — not an
SFTP client, not a piracy tool. Controller-only UI, no Sony SDK/keys/firmware
bundled, no proprietary code.

## Status: Phase 1 (research + architecture) complete. No downloader exists yet.

This is the first milestone of a longer plan (see `docs/architecture.md`
Section 0 and the development sequence it follows). What exists right now:

- A feasibility/architecture writeup covering the SDK choice, a structural
  reference app, the RomM API surface this app needs, and the two hardest
  open questions going in — incremental ZIP extraction and resumable
  downloads — with a recommended design for both. **`docs/architecture.md`**
- A RomM API reference assembled from RomM's own source/docs (no live
  instance was reachable while writing it — see the doc for what still needs
  confirming against a real server). **`docs/romm-api.md`**
- A minimal scaffold that actually cross-compiles against the real
  ps5-payload-dev SDK (verified in this session, not just planned — see
  architecture doc Section 7 for the exact commands) and does nothing else
  yet. **`src/`, `CMakeLists.txt`, `toolchain/README.md`**
- A CI job that fetches the SDK and builds that scaffold on every push
  touching this directory. **`../.github/workflows/romm-ps5-ci.yml`**

Nothing here has run on a real PS5. Every claim in `docs/architecture.md` is
explicitly marked as either verified-in-this-session or planned-but-untested
— read that distinction before relying on anything.

## Why this lives inside the DualDeck repository for now

This directory was created inside the `DualDeck` repository because that's
where this development session was configured to work, not because RomM-PS5
is related to DualDeck (a Nintendo DS/3DS/Wii U remote-play project for Steam
Deck) — it isn't. Per the project's own deliverables list, RomM-PS5 is meant
to end up as its own public repository before any PS5 Homebrew Store
submission. Until that split happens, everything RomM-PS5-related is
self-contained under this one directory and doesn't touch DualDeck's own
code, docs, or CI.

## License

GPL-3.0-or-later, same as the rest of this repository (see `../LICENSE`) —
this also matches the license of the `ps5-payload-dev` SDK samples this
project's build setup is based on. When this directory becomes its own
repository, it should carry its own copy of the license text.

## Next steps

See `docs/architecture.md` Section 8 for the ranked list of what's still
unverified. In short: fetch and build the SDL2 port for controller input
(the biggest gap left by Phase 1), then check the assumed RomM JSON schema
against a real 5.1+ instance's `/openapi.json` before writing the RomM API
client.
