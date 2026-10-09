#!/usr/bin/env bash
# The one thing to double-click to set up or run the DualDeck
# client -- shows a simple menu and delegates to whichever of the
# internal/ scripts actually applies, so a user never has to figure out
# which one they need. See ../host/dualdeck-host.sh for the host
# equivalent (same menu shape).
#
# Uses a graphical kdialog menu when available (SteamOS Desktop Mode
# and Bazzite are both KDE Plasma, where kdialog is standard) and falls
# back to a plain numbered prompt in a terminal otherwise.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

error_log="${HOME}/.config/dualdeck-client/install.log"
on_error() {
    local exit_code="$1" line_no="$2" failing_cmd="$3"
    mkdir -p "$(dirname "${error_log}")"
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") dualdeck-client.sh line ${line_no}: \`${failing_cmd}\` failed (exit ${exit_code})" >> "${error_log}"
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "DualDeck" --error "Something went wrong: ${failing_cmd}
(exit code ${exit_code})

Details logged to:
${error_log}" 2>/dev/null || true
    fi
}
trap 'ec=$?; on_error "${ec}" "${LINENO}" "${BASH_COMMAND}"' ERR

# Keep in sync with the same constant in internal/install-steam-shortcut.sh,
# internal/uninstall-steam-shortcut.sh, internal/apply-update.sh, and
# internal/configure-trackpad-experiment.sh's own copy -- only needed
# directly in this script for steam-remove's quiet, non-restart-helper
# cleanup call below (see its own comment for why that one deliberately
# doesn't go through configure-trackpad-experiment.sh).
client_shortcut_exe="${HOME}/.config/dualdeck-client/install/internal/run-client.sh"
client_shortcut_name="DualDeck"

# Advanced -> Installation branch. Own config dir, own cache, own
# selection -- see internal/dualdeck_branch.sh's own header comment for
# why this is the *same logic* as the host's copy without being the same
# physical file/state (host and client are always separate machines).
export DUALDECK_BRANCH_CONFIG_DIR="${HOME}/.config/dualdeck-client"
# shellcheck source=scripts/lib/dualdeck_branch.sh
source ./internal/dualdeck_branch.sh

have_kdialog() { command -v kdialog >/dev/null 2>&1; }

info() {
    if have_kdialog; then
        kdialog --title "DualDeck" --msgbox "$1" 2>/dev/null
    else
        echo
        echo "$1"
        echo
    fi
}

confirm() {
    if have_kdialog; then
        kdialog --title "DualDeck" --yesno "$1" 2>/dev/null
    else
        read -rp "$1 [y/N] " reply
        [[ "${reply}" =~ ^[Yy]$ ]]
    fi
}

# Real user request, 2026-08-02: "the controller layout should function
# exactly like a steam controller/steam deck" -- specifically, the
# touchpad only forwarded raw touch while the STEAM button was held
# (see steam_input_config.py's own module docstring for the actual
# research and fix: disabling Steam Input for this one shortcut, not a
# custom Controller Layout, per RPCS3's real precedent). This same
# toggle also lives in the client's own in-app Settings screen
# (client/src/main.cpp) -- both call internal/
# configure-trackpad-experiment.sh, the one place that actually knows
# how to check/change this, rather than duplicating that logic here.
# Dynamic label mirrors ../host/dualdeck-host.sh's own
# hostcontrol-daemon precedent -- doesn't write anything to check
# status, so this is always safe/instant to show regardless of whether
# Steam is running.
trackpad_experiment_label() {
    local status
    status="$(./internal/configure-trackpad-experiment.sh --status 2>/dev/null || echo "enabled")"
    if [[ "${status}" == "disabled" ]]; then
        echo "trackpad-experiment (Steam Input disabled, ON)"
    else
        echo "trackpad-experiment (Steam Input enabled, off)"
    fi
}

# Real requirement: "Discover branches through GitHub with pagination.
# ... Never silently fall back to main." choose_branch_from_list()
# builds its menu straight from dualdeck_branch_list() (cached, paginated
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
    if have_kdialog; then
        local -a kd_args=()
        while IFS= read -r b; do
            [[ -z "${b}" ]] && continue
            kd_args+=("${b}" "${b}")
        done <<< "${branches}"
        kdialog --title "DualDeck" --menu "Choose an installation branch${stale_note}" \
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
    if have_kdialog; then
        kdialog --title "DualDeck -- Advanced" --menu "${status_line}" \
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
    if have_kdialog; then
        kdialog --title "DualDeck" --menu "What would you like to do?" \
            launch "Launch DualDeck now" \
            steam-add "Add to Steam (Big Picture / Gaming Mode)" \
            steam-remove "Remove from Steam / uninstall" \
            trackpad-experiment "$(trackpad_experiment_label)" \
            update "Check for updates / update" \
            advanced "Advanced..." \
            2>/dev/null || echo "cancel"
    else
        # All of this goes to stderr, not stdout -- the caller captures
        # this function's stdout as the actual selection
        # ("action=\"\$(choose_action)\"" below), so any of the menu
        # display text leaking onto stdout would get appended to that
        # and break the case match entirely.
        {
            echo "DualDeck"
            echo "  1) Launch DualDeck now"
            echo "  2) Add to Steam (Big Picture / Gaming Mode)"
            echo "  3) Remove from Steam / uninstall"
            echo "  4) $(trackpad_experiment_label)"
            echo "  5) Check for updates / update"
            echo "  6) Advanced..."
            echo "  7) Exit"
        } >&2
        read -rp "Choice [1-7]: " choice
        case "${choice}" in
            1) echo "launch" ;;
            2) echo "steam-add" ;;
            3) echo "steam-remove" ;;
            4) echo "trackpad-experiment" ;;
            5) echo "update" ;;
            6) echo "advanced" ;;
            *) echo "cancel" ;;
        esac
    fi
}

# GitHub issue (real user report, 2026-08-27): "Check for updates /
# changing settings / saving settings all close DualDeck instead of
# returning to the menu." Root cause: this action="$(choose_action)" +
# case dispatch used to run exactly once and then fall off the end of
# the script -- no loop back to choose_action, so every branch except
# launch (which deliberately exec's, replacing this process) and the
# explicit cancel/exit branch silently ended the client menu. Wrapped in
# a loop now: only the top-level Cancel/Exit (the `*` branch below)
# breaks out -- a completed action or a failed one (each internal/
# script shows its own error dialog and returns non-zero rather than
# propagating a set -e exit -- see on_error's ERR trap above, which only
# fires for a genuinely unexpected failure) falls through to the bottom
# of the loop and redisplays this same menu.
while true; do
action="$(choose_action)"

case "${action}" in
    launch)
        # exec, not a plain call: this becomes the foreground process,
        # same as launching any other way (Steam shortcut, double-
        # clicking internal/run-client.sh directly, etc.) -- no menu
        # process left hanging around behind it.
        exec ./internal/run-client.sh
        ;;
    steam-add)
        if ./internal/install-steam-shortcut.sh; then
            info "Added DualDeck to Steam. Restart Steam (or switch to Gaming Mode) to see it, and set its Controller Layout to a plain Gamepad template once it's there."
        fi
        # A failure here already logged and showed its own error dialog
        # (install-steam-shortcut.sh has the same error-trap pattern as
        # this script) -- nothing more to do.
        ;;
    trackpad-experiment)
        current_status="$(./internal/configure-trackpad-experiment.sh --status 2>/dev/null || echo "enabled")"
        if [[ "${current_status}" == "disabled" ]]; then
            if confirm "Re-enable Steam Input for the DualDeck Client shortcut? This turns the trackpad-as-native-touchpad experiment back off -- the touchpad goes back to only working while STEAM is held."; then
                if ./internal/configure-trackpad-experiment.sh --remove; then
                    info "Steam Input re-enabled for DualDeck Client. Restart Steam for this to take effect."
                fi
            fi
        else
            if confirm "Disable Steam Input for the DualDeck Client shortcut? This is an experimental fix for the touchpad only working while STEAM is held -- see docs/known-limitations.md's 2026-08-02 entry. Not yet confirmed on real hardware; you can turn it back off from this same menu."; then
                if ./internal/configure-trackpad-experiment.sh; then
                    info "Steam Input disabled for DualDeck Client. Restart Steam for this to take effect, then relaunch DualDeck and test the touchpad without holding STEAM."
                fi
            fi
        fi
        ;;
    steam-remove)
        if confirm "This removes the Steam shortcut and the installed files. Continue?"; then
            # Best-effort, before the shortcut it's keyed off of is
            # removed below -- a full uninstall should also undo the
            # trackpad experiment if it was ever turned on, not leave a
            # stale Steam Input override pointing at an appid a later
            # re-add would recompute identically from the same --exe/
            # --name (so leaving it wouldn't even be inert -- it would
            # silently carry over to whatever gets installed next). Not
            # --force'd, same as uninstall-steam-shortcut.sh's own plain
            # (non-restart-helper) call just below -- if Steam happens to
            # be running, this silently no-ops rather than risking a
            # clobber; a stale key surviving one uninstall cycle is a
            # minor loose end, not worth forcing past that safety check.
            python3 ./internal/steam_input_config.py --exe "${client_shortcut_exe}" \
                --name "${client_shortcut_name}" --remove >/dev/null 2>&1 || true
            if ./internal/uninstall-steam-shortcut.sh; then
                info "Removed."
            fi
        fi
        ;;
    update)
        update_report="$(../check-for-updates.sh)"
        if echo "${update_report}" | grep -q "update available:"; then
            latest_version="$(echo "${update_report}" | sed -n 's/.*update available: //p' | head -1)"
            if confirm "${update_report}

Install ${latest_version} now? This downloads it from GitHub and also adds/updates the Steam shortcut."; then
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
                    elif confirm "Install DualDeck Client from branch '${selected_branch}'? This resolves it to a specific published commit, downloads and verifies that build, and only replaces the current install if that fully succeeds."; then
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
