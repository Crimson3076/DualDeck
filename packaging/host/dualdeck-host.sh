#!/usr/bin/env bash
# The one thing to double-click to set up or run the DualDeck
# host -- shows a simple menu and delegates to whichever of the
# internal/ scripts actually applies, so a user never has to figure out
# which one they need (GitHub issue #10: "the normal path requires no
# terminal commands"). Those scripts still exist and still work
# standalone (e.g. for scripting or troubleshooting) -- this is just
# the single entry point a human actually needs to know about.
#
# Shows the full-screen DualDeck Host window (internal/dualdeck-host-ui,
# driven over a pipe -- see host/ui/src/protocol.h) when it can open,
# which works with a controller, mouse or keyboard, on the desktop and in
# Gaming Mode. Falls back to kdialog popups (SteamOS Desktop Mode and
# Bazzite are both KDE Plasma, where kdialog is standard), then to a
# plain numbered prompt in a terminal. Set DUALDECK_HOST_UI=0 to skip the
# window.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

# Real user report, 2026-08-03 (Bazzite HTPC): "Error when initializing
# Vulkan Renderer on Cemu on bazzite, works fine on Fedora Laptop." This
# script is install-steam-shortcut.sh's --exe target, i.e. the actual
# process Steam launches -- every emulator this menu execs into (melonDS
# via launch-host.sh, Azahar via run-host-azahar.sh, Cemu via
# run-host-cemu.sh) inherits Steam's own LD_PRELOAD (overlay-injection
# libs, e.g. gameoverlayrenderer.so) unless it's stripped here first.
# This is the identical root cause already found and fixed for melonDS's
# Distrobox launch path (see install-host-distrobox.sh's own
# 2026-08-01 comment: it broke libGL.so.1 loading outright there) --
# Cemu's native (non-Distrobox) launch path never got the same fix.
# Steam's overlay hooks Vulkan's vkCreateInstance/vkCreateDevice via
# that same LD_PRELOAD, a known cause of "Vulkan Renderer" init
# failures specifically when launched as a Steam shortcut -- explaining
# why a plain (non-Steam-launched) Cemu run on a Fedora laptop is
# unaffected. Stripped once, here at the true entry point, so every
# launch path below inherits the clean environment instead of needing
# its own copy of this fix (install-host-distrobox.sh keeps its own
# unset too -- harmless/redundant now, not worth removing since it's
# also a correct, self-contained safety net for anyone invoking it
# directly rather than through this menu).
unset LD_PRELOAD LD_LIBRARY_PATH

error_log="${HOME}/.config/dualdeck/install.log"
# shellcheck source=scripts/lib/release_install.sh
source ./internal/release_install.sh
dualdeck_trap_errors "DualDeck Host" "Something went wrong"

# Advanced -> Installation branch. Own config dir, own cache, own
# selection -- see internal/dualdeck_branch.sh's own header comment for
# why this is the *same logic* as the client's copy without being the
# same physical file/state (host and client are always separate
# machines).
export DUALDECK_BRANCH_CONFIG_DIR="${HOME}/.config/dualdeck"
# shellcheck source=scripts/lib/dualdeck_branch.sh
source ./internal/dualdeck_branch.sh

have_kdialog() { command -v kdialog >/dev/null 2>&1; }

# --- The DualDeck Host window -------------------------------------------
# Started once as a coprocess; each ui_* call writes one request line and,
# where the window answers, reads one reply line. The window only draws:
# every decision below stays in this script.
ui_active=0
ui_to=""
ui_from=""

ui_send() {
    [[ -n "${ui_to}" ]] || return 1
    local line="" field first=1
    for field in "$@"; do
        field="${field//\\/\\\\}"
        field="${field//$'\t'/\\t}"
        field="${field//$'\n'/\\n}"
        if [[ "${first}" -eq 1 ]]; then
            line="${field}"
            first=0
        else
            line+=$'\t'"${field}"
        fi
    done
    # In a subshell so a window that just closed (SIGPIPE) only ends that.
    ( printf '%s\n' "${line}" 1>&"${ui_to}" ) 2>/dev/null
}

# The window's answer. If it's gone (closed or crashed), there's nothing
# to answer with: an empty reply reads as Cancel everywhere, and the main
# menu's Cancel ends this script.
ui_read() {
    local reply=""
    read -r -u "${ui_from}" reply 2>/dev/null || reply=""
    printf '%s' "${reply}"
}

ui_start() {
    [[ "${DUALDECK_HOST_UI:-1}" != "0" ]] || return 1
    [[ -x ./internal/dualdeck-host-ui ]] || return 1
    [[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]] || return 1
    mkdir -p "$(dirname "${error_log}")"
    # Its own bundled SDL3 (internal/ui-lib/, see build-release.sh), set
    # for the window only: nothing launched from this menu inherits it.
    coproc DUALDECK_UI {
        LD_LIBRARY_PATH="${PWD}/internal/ui-lib" exec ./internal/dualdeck-host-ui 2>>"${error_log}"
    }
    # Plain duplicates: usable from the $(...) subshells the menus run in.
    exec {ui_to}>&"${DUALDECK_UI[1]}" {ui_from}<&"${DUALDECK_UI[0]}"
    local reply=""
    if ui_send hello; then
        read -r -t 15 -u "${ui_from}" reply 2>/dev/null || reply=""
    fi
    if [[ "${reply}" != ready* ]]; then
        ui_stop
        return 1
    fi
    ui_active=1
    ui_send header "${dualdeck_version}"
}

# Closes the window, e.g. right before exec'ing into an emulator.
ui_stop() {
    if [[ -n "${ui_to}" ]]; then
        ui_send quit || true
        exec {ui_to}>&- {ui_from}<&-
        ui_to=""
        ui_from=""
    fi
    if [[ -n "${DUALDECK_UI_PID:-}" ]]; then
        wait "${DUALDECK_UI_PID}" 2>/dev/null || true
    fi
    ui_active=0
}

# A spinner over the current screen while something slow runs.
ui_busy() {
    if [[ "${ui_active}" -eq 1 ]]; then
        ui_send busy "$1" || true
    fi
}

# The release's VERSION file sits one level up (see build-release.sh);
# shown in the main menu so it's obvious which build is installed.
dualdeck_version="$(cat ../VERSION 2>/dev/null || true)"

info() {
    if [[ "${ui_active}" -eq 1 ]]; then
        # An empty title: the window uses the first sentence.
        ui_send message "" "$1" && ui_read >/dev/null
    elif have_kdialog; then
        kdialog --title "DualDeck Host" --msgbox "$1" 2>/dev/null
    else
        echo
        echo "$1"
        echo
    fi
}

# confirm QUESTION [TITLE YES-LABEL NO-LABEL DANGER]
# The optional fields only change how the DualDeck window shows it.
confirm() {
    if [[ "${ui_active}" -eq 1 ]]; then
        ui_send confirm "${2:-Continue?}" "$1" "${3:-Yes}" "${4:-No}" "${5:-0}" || return 1
        [[ "$(ui_read)" == "yes" ]]
    elif have_kdialog; then
        kdialog --title "DualDeck Host" --yesno "$1" 2>/dev/null
    else
        read -rp "$1 [y/N] " reply
        [[ "${reply}" =~ ^[Yy]$ ]]
    fi
}

# Real requirement: "Discover branches through GitHub with pagination.
# ... Never silently fall back to main." choose_branch_from_list() builds
# its menu straight from dualdeck_branch_list() (cached, paginated
# already inside that function) -- an empty/failed list here means "no
# branches to show," not "assume main."
choose_branch_from_list() {
    local branches stale_note=""
    branches="$(dualdeck_branch_list 2>/dev/null || true)"
    if [[ -z "${branches}" ]]; then
        info "Couldn't load the branch list from GitHub (offline, rate-limited, or unreachable) and there's no cached list yet. Try Refresh again once you're back online."
        return 0
    fi
    if dualdeck_branch_cache_is_stale; then
        stale_note=" (cached list, may be out of date -- use Refresh to update)"
    fi
    if [[ "${ui_active}" -eq 1 ]]; then
        ui_send menu list "Installation branch" "Pick the branch to install from${stale_note}"
        while IFS= read -r b; do
            [[ -z "${b}" ]] && continue
            ui_send item "${b}" "${b}" "" branch "" "" ""
        done <<< "${branches}"
        ui_send end
        local picked
        picked="$(ui_read)"
        [[ "${picked}" == "cancel" ]] || printf '%s\n' "${picked}"
    elif have_kdialog; then
        local -a kd_args=()
        while IFS= read -r b; do
            [[ -z "${b}" ]] && continue
            kd_args+=("${b}" "${b}")
        done <<< "${branches}"
        kdialog --title "DualDeck Host" --menu "Choose an installation branch${stale_note}" \
            "${kd_args[@]}" 2>/dev/null || true
    else
        echo "Choose an installation branch${stale_note}:" >&2
        local -a arr=()
        while IFS= read -r b; do
            [[ -z "${b}" ]] && continue
            arr+=("${b}")
        done <<< "${branches}"
        local i=1
        for b in "${arr[@]}"; do
            echo "  ${i}) ${b}" >&2
            i=$((i + 1))
        done
        echo "  0) Cancel" >&2
        local choice=""
        read -rp "Choice: " choice
        if [[ "${choice}" =~ ^[0-9]+$ ]] && [[ "${choice}" -ge 1 ]] && [[ "${choice}" -le "${#arr[@]}" ]]; then
            echo "${arr[$((choice - 1))]}"
        fi
    fi
}

choose_advanced_action() {
    local status_line
    status_line="$(dualdeck_branch_status_line 2>/dev/null || echo "Installation branch: unknown")"
    if [[ "${ui_active}" -eq 1 ]]; then
        ui_send menu list "Advanced" "${status_line}"
        ui_send item change-branch "Change installation branch" "Choose which GitHub branch updates come from" branch "" "" ""
        ui_send item refresh-branches "Refresh branch list" "Fetch the latest branches from GitHub" refresh "" "" ""
        ui_send item install-branch "Install selected branch" "Download and install the branch you picked" download "" "" ""
        ui_send end
        ui_read
    elif have_kdialog; then
        kdialog --title "DualDeck Host -- Advanced" --menu "${status_line}" \
            change-branch "Change installation branch..." \
            refresh-branches "Refresh branch list" \
            install-branch "Install selected branch" \
            2>/dev/null || echo "cancel"
    else
        {
            echo "Advanced"
            echo "${status_line}"
            echo "  1) Change installation branch..."
            echo "  2) Refresh branch list"
            echo "  3) Install selected branch"
            echo "  4) Back"
        } >&2
        read -rp "Choice [1-4]: " choice
        case "${choice}" in
            1) echo "change-branch" ;;
            2) echo "refresh-branches" ;;
            3) echo "install-branch" ;;
            *) echo "cancel" ;;
        esac
    fi
}

choose_action() {
    # Real user request, 2026-08-01: "Host control should be a constant
    # server from the host, being toggled via the eventual decky menu
    # plugin or the DualDeck Host GUI. Steam should not keep registering
    # it as a game running in the background." -- the label reflects
    # current systemd --user state (mirrors choose_emulator()'s own
    # dynamic custom_label precedent below) so this menu doubles as a
    # status check, not just an action picker.
    local daemon_label="Always-on Host Control: OFF (turn on...)"
    local daemon_status="Always-on Host Control is off."
    local daemon_on=0
    if command -v systemctl >/dev/null 2>&1 && \
       systemctl --user is-active --quiet dualdeck-host-control.service 2>/dev/null; then
        daemon_label="Always-on Host Control: ON (turn off...)"
        daemon_status="Always-on Host Control is on: a Deck can connect any time."
        daemon_on=1
    fi

    if [[ "${ui_active}" -eq 1 ]]; then
        if [[ "${daemon_on}" -eq 1 ]]; then
            ui_send menu home "DualDeck Host" ""
            ui_send status on "A Deck can connect any time, even with no emulator open."
        else
            ui_send menu home "DualDeck Host" ""
            ui_send status off "A Deck can connect while an emulator is running."
        fi
        ui_send item launch "Play" "Launch an emulator and stream it to your Deck. The Deck switches to the game on its own." "" "" "" "DS|melonDS,3DS|Azahar,WII U|Cemu,DESKTOP"
        ui_send item hostcontrol-daemon "Always-on Host Control" "Starts at login, so a Deck can use this PC any time" monitor "" "$([[ "${daemon_on}" -eq 1 ]] && echo on || echo off)" ""
        ui_send item steam-add "Add to Steam" "Shows DualDeck Host in Gaming Mode and Big Picture" plus "" "" ""
        ui_send item update "Check for updates" "Download the newest DualDeck release" refresh "" "" ""
        ui_send item reconfigure-controls "Fix Cemu controls" "When Deck input does nothing in Cemu" gamepad "" "" ""
        ui_send item emudeck "Use my EmuDeck / RetroDECK emulators" "Stream the emulators you already have installed" list "Experimental" warn ""
        ui_send item advanced "Advanced" "Installation branch and other options" gear "" "" ""
        ui_send item steam-remove "Remove from Steam and uninstall" "Your ROMs, saves and firmware are never touched" trash "" danger ""
        ui_send end
        ui_read
    elif have_kdialog; then
        kdialog --title "DualDeck Host${dualdeck_version:+ ${dualdeck_version}}" \
            --menu "${daemon_status}

What would you like to do?" \
            launch "Play: launch an emulator..." \
            hostcontrol-daemon "${daemon_label}" \
            steam-add "Add DualDeck Host to Steam (Gaming Mode / Big Picture)" \
            steam-remove "Remove from Steam and uninstall..." \
            reconfigure-controls "Fix Cemu controls (Deck input does nothing in Cemu)" \
            update "Check for updates" \
            emudeck "Use my EmuDeck/RetroDECK emulators (experimental)" \
            advanced "Advanced..." \
            2>/dev/null || echo "cancel"
    else
        # All of this goes to stderr, not stdout -- the caller captures
        # this function's stdout as the actual selection
        # ("action=\"\$(choose_action)\"" below), so any of the menu
        # display text leaking onto stdout would get appended to that
        # and break the case match entirely.
        {
            echo "DualDeck Host"
            [[ -n "${dualdeck_version}" ]] && echo "Version ${dualdeck_version}"
            echo "${daemon_status}"
            echo "  1) Play: launch an emulator..."
            echo "  2) ${daemon_label}"
            echo "  3) Add DualDeck Host to Steam (Gaming Mode / Big Picture)"
            echo "  4) Remove from Steam and uninstall..."
            echo "  5) Fix Cemu controls (Deck input does nothing in Cemu)"
            echo "  6) Check for updates"
            echo "  7) Use my EmuDeck/RetroDECK emulators (experimental)"
            echo "  8) Advanced..."
            echo "  9) Exit"
        } >&2
        read -rp "Choice [1-9]: " choice
        case "${choice}" in
            1) echo "launch" ;;
            2) echo "hostcontrol-daemon" ;;
            3) echo "steam-add" ;;
            4) echo "steam-remove" ;;
            5) echo "reconfigure-controls" ;;
            6) echo "update" ;;
            7) echo "emudeck" ;;
            8) echo "advanced" ;;
            *) echo "cancel" ;;
        esac
    fi
}

# Real user request, 2026-08-01 (see choose_action()'s own comment): a
# persistent Host Control daemon, toggled via a real GUI now rather than
# only terminal systemctl commands, built so a future Decky Loader
# plugin has an obvious, trivial pair of commands to shell out to
# instead (systemctl --user start/stop dualdeck-host-control.service).
choose_host_control_daemon_action() {
    local active=0
    if command -v systemctl >/dev/null 2>&1 && \
       systemctl --user is-active --quiet dualdeck-host-control.service 2>/dev/null; then
        active=1
    fi

    if [[ "${ui_active}" -eq 1 ]]; then
        if [[ "${active}" -eq 1 ]]; then
            ui_send menu list "Always-on Host Control" "On: a Deck can connect and use this PC's desktop any time, even with no emulator open."
            ui_send status on "A Deck can connect any time."
            ui_send item stop "Turn off" "Stop now and don't start at login" power "" "" ""
        else
            ui_send menu list "Always-on Host Control" "Off: turn it on to let a Deck connect and use this PC's desktop any time. It starts again at every login."
            ui_send status off "A Deck can connect while an emulator is running."
            ui_send item start "Turn on" "Start now and at every login" power "" "" ""
        fi
        ui_send item status "Show service status" "What systemd reports for the background service" info "" "" ""
        ui_send end
        ui_read
    elif have_kdialog; then
        if [[ "${active}" -eq 1 ]]; then
            kdialog --title "DualDeck Host" --menu "Always-on Host Control is ON. A Deck can connect and use this PC's desktop any time, even with no emulator open." \
                stop "Turn off (and don't start at login)" \
                status "Show service status" \
                2>/dev/null || echo "cancel"
        else
            kdialog --title "DualDeck Host" --menu "Always-on Host Control is OFF. Turn it on to let a Deck connect and use this PC's desktop any time, even with no emulator open. It starts again at every login." \
                start "Turn on now (and at every login)" \
                status "Show service status" \
                2>/dev/null || echo "cancel"
        fi
    else
        {
            if [[ "${active}" -eq 1 ]]; then
                echo "Always-on Host Control is ON"
                echo "  1) Turn off (and don't start at login)"
            else
                echo "Always-on Host Control is OFF"
                echo "  1) Turn on now (and at every login)"
            fi
            echo "  2) Show service status"
            echo "  3) Back"
        } >&2
        read -rp "Choice [1-3]: " choice
        case "${choice}" in
            1) [[ "${active}" -eq 1 ]] && echo "stop" || echo "start" ;;
            2) echo "status" ;;
            *) echo "cancel" ;;
        esac
    fi
}

# GitHub issue "rework the host launcher so it does not boot into
# melonDS": picks which system/emulator to actually launch, instead of
# always going straight to melonDS. Azahar (3DS), Cemu (Wii U), and
# "host control only" all use the standalone Host Service's own
# kdialog-based device-approval popup (see
# internal/run-host-azahar.sh's/run-host-cemu.sh's comments and
# kdialog_approval_prompt.h) -- the same zero-typing flow melonDS's own
# in-process dialog already has.
choose_emulator() {
    local custom_conf="${HOME}/.config/dualdeck/custom-emulator.conf"
    local custom_label="Custom (patch my own emulator)"
    if [[ -f "${custom_conf}" ]]; then
        local custom_path
        custom_path="$(sed -n 's/^path=//p' "${custom_conf}")"
        [[ -n "${custom_path}" ]] && custom_label="Custom (${custom_path})"
    fi

    if [[ "${ui_active}" -eq 1 ]]; then
        local custom_detail="Patch and run my own emulator build"
        [[ "${custom_label}" != "Custom (patch my own emulator)" ]] && custom_detail="${custom_label#Custom (}" && custom_detail="${custom_detail%)}"
        ui_send menu cards "Choose a system" "The Deck connects as soon as the emulator is up."
        ui_send item ds "DS" "melonDS, with both screens and the touch screen on the Deck" ds "" "" ""
        ui_send item n3ds "3DS" "Azahar" 3ds "Experimental" warn ""
        ui_send item wiiu "Wii U" "Cemu, with the GamePad screen on the Deck" wiiu "Experimental" warn ""
        ui_send item hostcontrol "Desktop" "Host Control only: use this PC from the Deck, no emulator" desktop "Experimental" warn ""
        ui_send item custom "Custom" "${custom_detail}" custom "" "" ""
        ui_send end
        ui_read
    elif have_kdialog; then
        kdialog --title "DualDeck Host" --menu "Which system do you want to play?" \
            ds "Nintendo DS (melonDS)" \
            n3ds "Nintendo 3DS (Azahar, experimental)" \
            wiiu "Nintendo Wii U (Cemu, experimental)" \
            hostcontrol "Host control only -- no emulator (experimental)" \
            custom "${custom_label}" \
            2>/dev/null || echo "cancel"
    else
        {
            echo "Which system?"
            echo "  1) Nintendo DS (melonDS)"
            echo "  2) Nintendo 3DS (Azahar, experimental)"
            echo "  3) Nintendo Wii U (Cemu, experimental)"
            echo "  4) Host control only -- no emulator (experimental)"
            echo "  5) ${custom_label}"
            echo "  6) Back"
        } >&2
        read -rp "Choice [1-6]: " choice
        case "${choice}" in
            1) echo "ds" ;;
            2) echo "n3ds" ;;
            3) echo "wiiu" ;;
            4) echo "hostcontrol" ;;
            5) echo "custom" ;;
            *) echo "cancel" ;;
        esac
    fi
}

# GitHub issue (real user report, 2026-08-27): "Check for updates /
# changing settings / saving settings all close DualDeck Host instead of
# returning to the menu." Root cause was that this whole action="$(choose_
# action)" + case dispatch used to run exactly once and then fall off the
# end of the script -- there was no loop back to choose_action, so every
# branch except launch/emudeck (which deliberately exec, replacing this
# process) and the explicit cancel/exit branch silently ended the host.
# Wrapped in a loop now: only an explicit top-level Cancel/Exit (the `*`
# branch below) breaks out: everything else -- a completed action, a
# failed one (each internal/ script already shows its own error dialog
# and returns non-zero rather than propagating a set -e exit -- see
# the dualdeck_trap_errors ERR trap above, which only fires for a genuinely unexpected
# failure, not a handled one), or backing out of a submenu -- falls
# through to the bottom of the loop and redisplays this same menu.
ui_start || true

while true; do
action="$(choose_action)"

case "${action}" in
    launch)
        emulator="$(choose_emulator)"
        case "${emulator}" in
            ds)
                # exec, not a plain call: this becomes the foreground
                # process, same as launching melonDS any other way
                # (Steam shortcut, double-clicking internal/run-host.sh
                # directly, etc.) -- no menu process left hanging
                # around behind it.
                ui_stop
                exec ./internal/launch-host.sh
                ;;
            n3ds)
                # No auth token exported: an unrecognized device pops a
                # kdialog Yes/No approval prompt on this desktop instead
                # (see internal/run-host-azahar.sh's comment and
                # kdialog_approval_prompt.h) -- the same zero-typing flow
                # melonDS's own in-process dialog already has. Azahar has
                # no Distrobox launch path yet (see
                # internal/run-host-azahar.sh's own comment) -- called
                # directly, not through launch-host.sh's melonDS-specific
                # Distrobox-vs-plain dispatch.
                ui_stop
                exec ./internal/run-host-azahar.sh
                ;;
            wiiu)
                # Same out-of-process-only shape as n3ds above -- see
                # internal/run-host-cemu.sh's comment and
                # host/cemu-patches/README.md for why Cemu has no
                # in-process device-approval path either. No Distrobox
                # launch path yet -- called directly, not through
                # launch-host.sh's melonDS-specific dispatch.
                ui_stop
                exec ./internal/run-host-cemu.sh
                ;;
            hostcontrol)
                # Exported before the same launch-host.sh dispatch the
                # "ds" case above uses -- DUALDECK_HOST_CONTROL is
                # an environment variable, so run-host.sh (or
                # install-host-distrobox.sh's rejection check, on an
                # immutable system) sees it either way without
                # launch-host.sh itself needing to know this mode
                # exists. No auth token exported here either, same
                # zero-typing kdialog approval as the n3ds case above.
                export DUALDECK_HOST_CONTROL=1
                ui_stop
                exec ./internal/launch-host.sh
                ;;
            custom)
                ui_stop
                exec ./internal/launch-custom-emulator.sh
                ;;
            *)
                # Cancelled/backed out of "Which system?" -- return to
                # the main menu, don't exit the host (see this loop's own
                # header comment).
                ;;
        esac
        ;;
    hostcontrol-daemon)
        daemon_action="$(choose_host_control_daemon_action)"
        case "${daemon_action}" in
            start)
                # Real Bazzite hardware report, 2026-08-02: this used to
                # always show the same generic "check install.log"
                # message on any failure, even when
                # install-host-control-daemon.sh had already printed the
                # real, specific reason to stderr (e.g. its old
                # immutable-system refusal) -- install.log never
                # contained that reason either, since a clean, expected
                # refusal isn't an ERR-trap failure. Capture both
                # commands' combined output and show it directly instead
                # of guessing, falling back to the old generic message
                # only if nothing was actually captured.
                ui_busy "Turning on Host Control…"
                if daemon_start_output="$(./internal/install-host-control-daemon.sh 2>&1 && \
                    systemctl --user enable --now dualdeck-host-control.service 2>&1)"; then
                    info "Always-on Host Control is on. A Deck can connect any time, and it starts again whenever you log in. Launching an emulator still works as before and switches the Deck to that game automatically."
                elif [[ -n "${daemon_start_output}" ]]; then
                    info "Couldn't turn on always-on Host Control:

${daemon_start_output}"
                else
                    info "Couldn't turn on always-on Host Control. See ${error_log} for details, or check whether systemd --user is available on this system."
                fi
                ;;
            stop)
                if systemctl --user disable --now dualdeck-host-control.service 2>/dev/null; then
                    info "Always-on Host Control is off and won't start at login."
                else
                    info "Couldn't turn off always-on Host Control (it may never have been turned on)."
                fi
                ;;
            status)
                status_output="$(systemctl --user status dualdeck-host-control.service --no-pager 2>&1 || true)"
                if [[ "${ui_active}" -eq 1 ]]; then
                    ui_send textview "Host Control service status" "${status_output}" && ui_read >/dev/null
                elif have_kdialog; then
                    status_file="$(mktemp)"
                    echo "${status_output}" > "${status_file}"
                    kdialog --title "DualDeck Host Control daemon status" --textbox "${status_file}" 600 400 2>/dev/null || true
                    rm -f "${status_file}"
                else
                    echo "${status_output}"
                fi
                ;;
            *)
                # Cancelled/backed out -- return to the main menu, don't
                # exit the host (see the outer loop's own header comment).
                ;;
        esac
        ;;
    steam-add)
        ui_busy "Adding DualDeck Host to Steam…"
        if ./internal/install-steam-shortcut.sh; then
            info "Added DualDeck Host to Steam. Restart Steam (or switch to Gaming Mode) to see it, and set its Controller Layout to a plain Gamepad template once it's there."
        fi
        # A failure here already logged and showed its own error dialog
        # (install-steam-shortcut.sh has the same error-trap pattern as
        # this script) -- nothing more to do.
        ;;
    steam-remove)
        if confirm "This removes the Steam shortcut, the installed files, and the Distrobox container if one was created. Your ROMs, saves, and firmware are never touched. Continue?" \
            "Remove DualDeck Host?" "Remove" "Keep" 1; then
            ui_busy "Removing DualDeck Host…"
            # Best-effort, before the files it depends on are removed
            # below -- a full uninstall should also tear down the
            # persistent Host Control daemon if one was ever installed,
            # not leave it running against a now-deleted install.
            ./internal/uninstall-host-control-daemon.sh 2>/dev/null || true
            if ./internal/uninstall-steam-shortcut.sh; then
                info "Removed."
            fi
        fi
        ;;
    reconfigure-controls)
        if confirm "This sets Cemu's own Controller 1 to type 'Wii U GamePad' if it isn't already, so DualDeck's remote input can auto-wire onto it -- fixes a fresh Cemu install showing no controls at all until this is set manually in Cemu's Input Settings. A real controller plugged into this host directly (e.g. for local co-op) stays mapped; only the controller *type* is touched, never existing bindings. Requires restarting Cemu (not just this menu) to take effect. Continue?" \
            "Fix Cemu controls?" "Fix" "Cancel"; then
            ui_busy "Updating Cemu's controller settings…"
            if reconfigure_output="$(./internal/reconfigure-cemu-controls.sh 2>&1)"; then
                info "${reconfigure_output}"
            else
                info "Could not reconfigure Cemu's controls:

${reconfigure_output}"
            fi
        fi
        ;;
    update)
        ui_busy "Checking for updates…"
        update_report="$(../check-for-updates.sh)"
        if echo "${update_report}" | grep -q "update available:"; then
            latest_version="$(echo "${update_report}" | sed -n 's/.*update available: //p' | head -1)"
            if confirm "${update_report}

Install ${latest_version} now? This downloads it from GitHub and also adds/updates the Steam shortcut." \
                "Install ${latest_version}?" "Install" "Not now"; then
                ui_busy "Downloading and installing ${latest_version}…"
                # Captured (not just checked for success) so the message
                # below can tell whether the Steam shortcut's Exe/AppName/
                # LaunchOptions actually changed -- for a routine update
                # they never do (see steam_shortcut.py's shortcut_up_to_date()),
                # so apply-update.sh doesn't touch shortcuts.vdf at all and
                # there's nothing for a running Steam to need reloading.
                # Only mention restarting Steam when apply-update.sh's own
                # output says it actually wrote the file.
                if update_output="$(./internal/apply-update.sh 2>&1)"; then
                    if echo "${update_output}" | grep -q "Restart Steam"; then
                        info "Updated to ${latest_version}. Restart Steam (or switch to Gaming Mode) to see the change."
                    else
                        info "Updated to ${latest_version}."
                    fi
                fi
                # A failure here already logged and showed its own error
                # dialog (apply-update.sh has the same error-trap pattern
                # as this script) -- nothing more to do.
            fi
        else
            info "${update_report}"
        fi
        ;;
    emudeck)
        ui_stop
        exec ./internal/launch-emudeck-integration.sh
        ;;
    advanced)
        while true; do
            adv_action="$(choose_advanced_action)"
            case "${adv_action}" in
                change-branch)
                    new_branch="$(choose_branch_from_list)"
                    if [[ -n "${new_branch}" ]]; then
                        if dualdeck_branch_set_selected "${new_branch}"; then
                            # Selecting only updates local state -- real
                            # requirement: "changing the selection should
                            # not immediately reinstall."
                            info "Selected branch: ${new_branch}. Nothing has been installed yet -- use \"Install selected branch\" when you're ready."
                        else
                            info "'${new_branch}' isn't a valid branch name -- not saved."
                        fi
                    fi
                    ;;
                refresh-branches)
                    ui_busy "Refreshing the branch list…"
                    if dualdeck_branch_refresh >/dev/null 2>&1; then
                        info "Branch list refreshed."
                    else
                        info "Couldn't refresh the branch list from GitHub (offline or rate-limited?). The previous list, if any, is still available."
                    fi
                    ;;
                install-branch)
                    selected_branch="$(dualdeck_branch_get_selected 2>/dev/null || true)"
                    if [[ -z "${selected_branch}" ]]; then
                        info "No branch selected yet -- use \"Change installation branch...\" first."
                    elif confirm "Install DualDeck Host from branch '${selected_branch}'? This resolves it to a specific published commit, downloads and verifies that build, and only replaces the current install if that fully succeeds. Remember to install the same branch on the client too." \
                        "Install branch '${selected_branch}'?" "Install" "Cancel"; then
                        ui_busy "Installing branch '${selected_branch}'…"
                        if install_output="$(./internal/install-branch.sh 2>&1)"; then
                            info "${install_output}"
                        else
                            info "Could not install branch '${selected_branch}':

${install_output}"
                        fi
                    fi
                    ;;
                *)
                    break
                    ;;
            esac
        done
        ;;
    *)
        break
        ;;
esac
done

ui_stop
