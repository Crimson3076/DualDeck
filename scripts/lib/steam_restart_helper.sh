#!/usr/bin/env bash
# Shared by host/internal/install-steam-shortcut.sh,
# host/internal/uninstall-steam-shortcut.sh, and their client/
# equivalents (copied in by scripts/build-release.sh alongside
# steam_shortcut.py itself, same pattern -- meant to be `source`d, not
# executed directly).
#
# steam_shortcut.py refuses to write shortcuts.vdf while Steam is
# running and a write is actually needed (it caches the file in memory
# and can silently clobber the change on its next save) -- previously
# this just told the user to close Steam manually and try again. On a
# Steam-Controller-only setup that's a real trap: once Steam is closed,
# there's no other way to interact with the desktop to reopen it (real
# user report, 2026-07-31, Bazzite HTPC). This restarts Steam
# automatically instead, with no confirmation prompt (explicit user
# choice, made aware of the tradeoff below).
#
# Known risk, accepted deliberately rather than discovered the hard
# way: Bazzite has documented upstream GitHub issues of Steam hanging
# on "Shutting down Steam" when asked to quit from Gaming Mode -- if
# that happens here, the 30s wait below times out, the shortcut write
# is skipped (not risked against a Steam that might still be alive),
# and Steam is relaunched anyway so a stuck shutdown doesn't also leave
# input broken. Set DUALDECK_NO_STEAM_AUTORESTART=1 in the environment
# to disable this behavior entirely and fall back to steam_shortcut.py's
# original plain refusal message, if this ever proves troublesome on a
# particular machine.

# run_steam_shortcut_with_restart <steam_shortcut_py> <error_log> <arg>...
#
# Runs steam_shortcut.py with the given args. On success, returns 0
# immediately, same as calling it directly. On the specific "Steam
# appears to be running" refusal, hands off to a detached background
# watcher (_steam_restart_and_retry below) and returns 0 anyway -- the
# caller should treat this as "in progress," not a hard failure. Any
# OTHER failure (corrupt shortcuts.vdf, permissions, etc.) is left
# completely alone: prints the same stderr steam_shortcut.py always
# has and returns its real nonzero exit code, so the caller's existing
# error handling (on_error/kdialog) still applies unchanged.
run_steam_shortcut_with_restart() {
    local steam_shortcut_py="$1" error_log="$2"
    shift 2

    local stderr_file
    stderr_file="$(mktemp)"
    # `cmd || exit_code=$?`, not `if cmd; then ... fi; exit_code=$?`:
    # an `if` statement whose condition is false and has no `else`
    # exits 0 itself (POSIX), which would silently discard python3's
    # real exit code here -- caught by testing an unrelated failure
    # (corrupt shortcuts.vdf) and finding it reported as success.
    local exit_code=0
    python3 "${steam_shortcut_py}" "$@" 2>"${stderr_file}" || exit_code=$?
    cat "${stderr_file}" >&2

    if [[ "${exit_code}" -eq 0 ]]; then
        rm -f "${stderr_file}"
        return 0
    fi

    if [[ "${DUALDECK_NO_STEAM_AUTORESTART:-}" == "1" ]] || ! grep -q "Steam appears to be running" "${stderr_file}"; then
        rm -f "${stderr_file}"
        return "${exit_code}"
    fi
    rm -f "${stderr_file}"

    echo "Steam is running -- restarting it automatically to finish this (only needed once per shortcut change, not on every launch)..." >&2
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "DualDeck" --passivepopup \
            "Steam needs to restart to finish this -- it will reopen automatically in a few seconds, please wait." 8 2>/dev/null || true
    fi

    _steam_restart_and_retry "${steam_shortcut_py}" "${error_log}" "$@"
    return 0
}

# _steam_restart_and_retry <steam_shortcut_py> <error_log> <arg>...
#
# Internal. Hands the actual quit/wait/retry/relaunch sequence off to a
# fully detached background process (setsid, its own session -- plain
# `&` alone is not enough) so it survives even if THIS process is
# itself a descendant of Steam (e.g. reached via a Steam shortcut in
# Gaming Mode) and gets torn down when Steam quits -- if it weren't
# detached, Steam's own exit could kill the very thing meant to bring
# Steam back.
_steam_restart_and_retry() {
    local steam_shortcut_py="$1" error_log="$2"
    shift 2

    setsid nohup bash -c '
        steam_shortcut_py="$1"; shift
        error_log="$1"; shift

        log() {
            echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") steam-restart-watcher: $1" >> "${error_log}" 2>/dev/null || true
        }

        log "asking Steam to quit (steam -shutdown)"
        steam -shutdown >/dev/null 2>&1 || true

        waited=0
        while pgrep -x steam >/dev/null 2>&1 && [ "${waited}" -lt 30 ]; do
            sleep 1
            waited=$((waited + 1))
        done

        if pgrep -x steam >/dev/null 2>&1; then
            log "Steam did not quit within 30s -- not retrying the shortcut write (the exact clobber this check exists to prevent). Restarting Steam anyway so input is not left stranded; re-run this action once Steam is fully closed."
        else
            log "Steam quit after ${waited}s -- retrying the shortcut write"
            if python3 "${steam_shortcut_py}" "$@" >>"${error_log}" 2>&1; then
                log "shortcut write succeeded on retry"
            else
                log "shortcut write FAILED on retry (exit $?) -- see the python3 output logged just above"
            fi
        fi

        log "relaunching Steam"
        nohup steam >/dev/null 2>&1 &
        disown
    ' _ "${steam_shortcut_py}" "${error_log}" "$@" \
        </dev/null >/dev/null 2>&1 &
    disown
}
