#!/usr/bin/env bash
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
