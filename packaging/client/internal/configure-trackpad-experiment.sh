#!/usr/bin/env bash
# Real user report, 2026-08-02: the trackpad-experiment toggle only
# existed in dualdeck-client.sh's outer shell menu, but that menu is
# unreachable from Gaming Mode -- install-steam-shortcut.sh points the
# Steam shortcut's Exe straight at run-client.sh (see its own comment
# just below), never at this menu script, so the only way to reach it
# was double-clicking dualdeck-client.sh manually in Desktop Mode. The
# user (reasonably) expected it in the client's own in-app Settings
# screen instead, which the Steam shortcut always reaches. This thin
# wrapper is the one place that knows how to check/toggle the
# experiment (sourcing steam_restart_helper.sh for the same
# Steam-caches-this-file-in-memory safety steam_shortcut.py's own writes
# already have), so both dualdeck-client.sh's menu AND main.cpp's
# Settings screen (which shells out to this, not to steam_input_config.py
# directly, since it's a plain script call away rather than needing
# main.cpp to know steam_restart_helper.sh's bash-specific machinery)
# stay in sync with exactly one implementation.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
# shellcheck source=scripts/lib/steam_restart_helper.sh
source ./steam_restart_helper.sh

error_log="${HOME}/.config/dualdeck-client/install.log"
# Keep in sync with the same constant in install-steam-shortcut.sh,
# uninstall-steam-shortcut.sh, apply-update.sh, and dualdeck-client.sh's
# own copy -- steam_input_config.py needs the exact same --exe/--name
# those already use so it computes the identical shortcut appid.
client_shortcut_exe="${HOME}/.config/dualdeck-client/install/internal/run-client.sh"
client_shortcut_name="DualDeck"

if [[ "${1:-}" == "--status" ]]; then
    exec python3 ./steam_input_config.py --exe "${client_shortcut_exe}" --name "${client_shortcut_name}" --status
fi

no_restart=0
extra_args=()
for arg in "$@"; do
    case "${arg}" in
        --remove) extra_args+=(--remove) ;;
        --no-restart) no_restart=1 ;;
    esac
done

# Real user report, 2026-08-02: toggling this from the client's own
# in-app Settings screen "seemingly crashes Steam and restarts it" --
# real, not a misread: run_steam_shortcut_with_restart's automatic
# `steam -shutdown` + relaunch (below) is the right call for
# dualdeck-client.sh's own standalone menu, run in Desktop Mode BEFORE
# anything is launched, but the client is normally itself a
# Steam-launched process in Gaming Mode -- killing Steam out from under
# a game it's actively running is jarring at best and, since Steam
# effectively *is* the Gaming Mode session on a real Deck, risks tearing
# down more than just Steam. main.cpp's Settings-screen calls pass
# --no-restart for exactly this reason; dualdeck-client.sh's own menu
# doesn't, so its existing auto-restart behavior (a real convenience on
# a controller-only Desktop Mode setup with no other way to reopen a
# closed Steam) is unchanged.
if [[ "${no_restart}" -eq 1 ]]; then
    # Writes anyway (--force, same accepted "Steam's in-memory cache
    # could overwrite this on its next save" tradeoff steam_shortcut.py's
    # own --force already carries) rather than just refusing outright --
    # the change still needs to land somewhere, and a manual Steam
    # restart later (the user's own choice, on their own schedule) will
    # pick it up correctly either way.
    python3 ./steam_input_config.py --exe "${client_shortcut_exe}" --name "${client_shortcut_name}" \
        --force "${extra_args[@]}"
    echo "Restart Steam yourself whenever's convenient for this to take effect -- not done automatically from here."
else
    run_steam_shortcut_with_restart ./steam_input_config.py "${error_log}" \
        --exe "${client_shortcut_exe}" --name "${client_shortcut_name}" "${extra_args[@]}"
fi
