#!/usr/bin/env bash
# Real user report, 2026-08-27: "controller 1 does not have any controls
# mapped when opening [Cemu], and I need to manually add the controller
# and API." Root-caused by direct inspection of real Cemu v2.6 source
# (scripts/lib/pinned_commits.sh's pinned CEMU_COMMIT, cloned fresh to
# confirm rather than guessed) against host/cemu-patches/README.md's own
# "Silent VPAD-registration failure" entry: CemuAdapter's constructor
# (host/cemu-patches/0001-remote-server-integration.patch) only auto-
# wires DualDeck's remote controller onto VPAD player 1 if
# InputManager::instance().get_vpad_controller(0) returns non-null --
# which requires Cemu's own persisted controller profile for player 1
# (controllerProfiles/controller0.xml, src/input/InputManager.cpp) to
# already declare type "Wii U GamePad". Normally only Cemu's own Input
# Settings GUI ever creates that file; a fresh Cemu install (or
# DualDeck's bundled one, which has never been through that GUI at all)
# has none, so the auto-wiring silently has nothing to attach to.
#
# Writes that file directly, in exactly the minimal shape Cemu's own
# InputManager::migrate_config() produces for a "just declare the type,
# no physical controller" profile (confirmed against that real function's
# source, not guessed): a bare <type>Wii U GamePad</type>, no
# <controller> child at all. That's sufficient on its own -- CemuAdapter's
# existing runtime code does the actual button wiring the moment it finds
# this slot exists; this script's only job is making sure the slot
# exists in the first place.
#
# Deliberately never touches a profile that's already type "Wii U
# GamePad", even one with real <controller> mappings from an actual
# controller plugged into this host directly (e.g. local co-op) --
# CemuAdapter's add_controller() call only ever adds the remote
# controller alongside whatever's already mapped there, it never needs
# this script to have cleared anything out first.
#
# See build-release.sh's own comment just above this heredoc for the
# real bug this fixes and why this exact file/shape is correct. Normally
# launched via ../dualdeck-host.sh's "Reconfigure Controls" menu choice,
# not directly.
set -euo pipefail

# Mirrors real Cemu's own portable-vs-XDG config path resolution exactly
# (a fresh clone of Cemu's actual src/gui/CemuApp.cpp, DeterminePaths(),
# Linux branch): a "portable" directory next to the real cemu executable
# wins if present, otherwise Cemu uses $XDG_CONFIG_HOME/Cemu (default
# ~/.config/Cemu). DualDeck's own packaging never creates a portable/
# directory today, so this always resolves to the XDG path in practice --
# kept as a real check rather than hardcoded so this keeps working if
# that ever changes.
host_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -d "${host_root}/portable" ]]; then
    cemu_config_dir="${host_root}/portable"
else
    cemu_config_dir="${XDG_CONFIG_HOME:-${HOME}/.config}/Cemu"
fi

profile_dir="${cemu_config_dir}/controllerProfiles"
profile_path="${profile_dir}/controller0.xml"

if [[ -f "${profile_path}" ]] && grep -q "<type>Wii U GamePad</type>" "${profile_path}" 2>/dev/null; then
    echo "Controller 1 is already configured as a Wii U GamePad at ${profile_path} -- nothing to do."
    exit 0
fi

mkdir -p "${profile_dir}"

if [[ -f "${profile_path}" ]]; then
    backup_path="${profile_path}.bak-$(date +%s)"
    cp "${profile_path}" "${backup_path}"
    echo "Backed up existing (non-GamePad-type) profile to ${backup_path}"
fi

cat > "${profile_path}" <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<emulated_controller>
	<type>Wii U GamePad</type>
</emulated_controller>
XML

echo "Set Controller 1 to Wii U GamePad at ${profile_path}."
echo "Restart Cemu for this to take effect -- DualDeck's remote input then auto-wires onto it the same way it always has once that slot exists."
