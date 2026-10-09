# Packaging scripts

`packaging/` holds every script shipped in the release archive, laid out
exactly as it appears there: `packaging/host/internal/run-host.sh` ships as
`host/internal/run-host.sh`, and so on. `scripts/build-release.sh` copies the
whole tree into the archive in one step (`cp -a`, so executable bits carry
over); nothing in it is generated or rewritten at build time.

These were previously written out as 28 heredocs inside
`scripts/build-release.sh`. They moved here unchanged (verified byte-for-byte
against the heredoc output), so each script's own header comment is still
its main documentation. The notes below are the few that lived only in
`build-release.sh`, next to the heredoc that wrote the script.

## `client/internal/configure-trackpad-experiment.sh`

Real user report, 2026-08-02: the trackpad-experiment toggle only
existed in dualdeck-client.sh's outer shell menu, but that menu is
unreachable from Gaming Mode -- install-steam-shortcut.sh points the
Steam shortcut's Exe straight at run-client.sh (see its own comment
just below), never at this menu script, so the only way to reach it
was double-clicking dualdeck-client.sh manually in Desktop Mode. The
user (reasonably) expected it in the client's own in-app Settings
screen instead, which the Steam shortcut always reaches. This thin
wrapper is the one place that knows how to check/toggle the
experiment (sourcing steam_restart_helper.sh for the same
Steam-caches-this-file-in-memory safety steam_shortcut.py's own writes
already have), so both dualdeck-client.sh's menu AND main.cpp's
Settings screen (which shells out to this, not to steam_input_config.py
directly, since it's a plain script call away rather than needing
main.cpp to know steam_restart_helper.sh's bash-specific machinery)
stay in sync with exactly one implementation.

## `host/internal/host-control-daemon.sh`

Real user request, 2026-08-01: "Host control should be a constant
server from the host, being toggled via the eventual decky menu
plugin or the DualDeck Host GUI. Steam should not keep registering it
as a game running in the background." Today's "Host control only"
menu choice (run-host.sh's DUALDECK_HOST_CONTROL branch) execs
straight into dualdeck-host-service as the literal foreground process
of whatever launched it (a Steam shortcut, or this menu script) --
Steam's own "is this game still running" tracking watches that exact
process, with no separate PID/session layer to decouple from. A
systemd --user service is a completely independent process tree Steam
never launches and never tracks at all, closing that gap directly
rather than trying to make Steam stop noticing a process it's already
watching.

## `host/internal/reconfigure-cemu-controls.sh`

Real user report, 2026-08-27: "controller 1 does not have any controls
mapped when opening [Cemu], and I need to manually add the controller
and API." Root-caused by direct inspection of real Cemu v2.6 source
(scripts/lib/pinned_commits.sh's pinned CEMU_COMMIT, cloned fresh to
confirm rather than guessed) against host/cemu-patches/README.md's own
"Silent VPAD-registration failure" entry: CemuAdapter's constructor
(host/cemu-patches/0001-remote-server-integration.patch) only auto-
wires DualDeck's remote controller onto VPAD player 1 if
InputManager::instance().get_vpad_controller(0) returns non-null --
which requires Cemu's own persisted controller profile for player 1
(controllerProfiles/controller0.xml, src/input/InputManager.cpp) to
already declare type "Wii U GamePad". Normally only Cemu's own Input
Settings GUI ever creates that file; a fresh Cemu install (or
DualDeck's bundled one, which has never been through that GUI at all)
has none, so the auto-wiring silently has nothing to attach to.

Writes that file directly, in exactly the minimal shape Cemu's own
InputManager::migrate_config() produces for a "just declare the type,
no physical controller" profile (confirmed against that real function's
source, not guessed): a bare <type>Wii U GamePad</type>, no
<controller> child at all. That's sufficient on its own -- CemuAdapter's
existing runtime code does the actual button wiring the moment it finds
this slot exists; this script's only job is making sure the slot
exists in the first place.

Deliberately never touches a profile that's already type "Wii U
GamePad", even one with real <controller> mappings from an actual
controller plugged into this host directly (e.g. local co-op) --
CemuAdapter's add_controller() call only ever adds the remote
controller alongside whatever's already mapped there, it never needs
this script to have cleared anything out first.

## `host/install-steam-shortcut.sh`

Compatibility shim: every release before this host/internal/
restructuring had install-steam-shortcut.sh directly at host/, and
those releases' own host/apply-update.sh hardcodes exactly that path
when it downloads and invokes a newer release's copy (see
docs/known-limitations.md). That already-installed
old script can't be changed retroactively, so a real user on one of
those versions hit exactly this: "Check for updates" downloads this
new, restructured release fine, then fails with exit 127 trying to
run a file that no longer exists at the old flat path. This shim
forwards to the real (current) location so updating *from* one of
those older releases keeps working. New installs/updates never
reach this file directly -- dualdeck-host.sh and apply-update.sh
both already call internal/install-steam-shortcut.sh -- so this exists
purely for that one-time upgrade path and is safe to delete once no
supported release still depends on it.
