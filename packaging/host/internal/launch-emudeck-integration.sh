#!/usr/bin/env bash
# Entry point for ../../dualdeck-host.sh's "Patch my EmuDeck/RetroDECK-
# installed emulators" menu choice -- runs the bundled
# emudeck-integration/scripts/emudeck-replace-in-place.sh (see
# scripts/build-release.sh's "Bundling EmuDeck integration tool" step in
# the DualDeck repository, and docs/known-limitations.md's Phase A and
# 2026-08-28 "RetroDECK" entries there for the full design).
#
# Despite the directory/tool name (kept as-is to avoid churning every
# path this integration already relies on), this also installs a fresh
# standalone AppImage with no EmuDeck present at all -- RetroDECK's own
# ES-DE fork (and any other ES-DE-based frontend) checks the exact same
# ~/Applications/*.AppImage path before falling back to its own bundled,
# unpatched copy. See emudeck-replace-in-place.sh's own header and
# install_fresh_standalone_appimage()'s comment.
#
# Unlike every other dualdeck-host.sh menu choice, this one downloads and
# installs over files EmuDeck itself manages, with its own per-emulator
# y/N confirmation prompts read from the terminal -- not a quick
# kdialog-driven action. Two things this needs that no other menu choice
# does, both from real user feedback:
#   1. Which emulator(s) to patch -- asked up front and passed through
#      as --emulator flags, instead of always patching every EmuDeck
#      install found.
#   2. The terminal window must NOT vanish the instant the tool exits
#      (whether it succeeds or fails) -- the whole point of running in
#      a visible terminal is so download/install progress and prompts can
#      be read, which a window that closes itself immediately defeats.
#      Building a small wrapper script that pauses on a "Press Enter to
#      close" prompt afterward is what actually fixes this, since a bare
#      `konsole -e sometool` still closes konsole the moment sometool
#      exits regardless of what sometool itself printed.
#
# If this script is already running with a visible terminal attached
# (e.g. launched from a terminal, or a file manager that runs .sh files
# "in a terminal"), that terminal is reused directly. Otherwise
# (typically: launched from Steam/Gaming Mode, which attaches no
# terminal at all) this re-launches inside a terminal emulator window,
# falling back to a kdialog message with the exact command to run
# manually if no terminal emulator can be found.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
host_root="$(cd .. && pwd)"

have_kdialog() { command -v kdialog >/dev/null 2>&1; }

tool="${host_root}/emudeck-integration/scripts/emudeck-replace-in-place.sh"
if [[ ! -x "${tool}" ]]; then
    msg="The EmuDeck integration tool is missing from this install (host/emudeck-integration/scripts/emudeck-replace-in-place.sh) -- re-download the release archive."
    if have_kdialog; then
        kdialog --title "DualDeck Host" --error "${msg}" 2>/dev/null || true
    else
        echo "error: ${msg}" >&2
    fi
    exit 1
fi

# ---- Which emulators? ----
choose_emulators() {
    if have_kdialog; then
        kdialog --title "DualDeck Host" --separate-output --checklist \
            "Which emulators should be patched?" \
            melonds "Nintendo DS (melonDS)" on \
            azahar "Nintendo 3DS (Azahar)" on \
            cemu "Nintendo Wii U (Cemu)" on \
            2>/dev/null || true
    else
        {
            echo "Which emulators should be patched?"
            echo "  1) All (default)"
            echo "  2) Nintendo DS (melonDS) only"
            echo "  3) Nintendo 3DS (Azahar) only"
            echo "  4) Nintendo Wii U (Cemu) only"
        } >&2
        local choice=""
        # < /dev/tty: this script can itself be reached via a chain
        # that started as a piped/non-interactive invocation -- see
        # DualDeck-Installer.sh's identical fix for the same reason.
        # Note the pre-assigned "" above: under `set -u`, `local choice`
        # alone leaves it genuinely unbound (not just empty) until
        # read succeeds, so a failed/no-op read (e.g. no /dev/tty at
        # all) would otherwise crash the case statement below instead
        # of falling through to its default.
        read -rp "Choice [1-4, Enter for all]: " choice < /dev/tty || true
        case "${choice}" in
            2) echo "melonds" ;;
            3) echo "azahar" ;;
            4) echo "cemu" ;;
            ""|1) printf '%s\n' melonds azahar cemu ;;
            *) ;;
        esac
    fi
}

mapfile -t selected_emulators < <(choose_emulators)
if [[ "${#selected_emulators[@]}" -eq 0 ]]; then
    exit 0 # cancelled / nothing selected
fi

emulator_args=()
for e in "${selected_emulators[@]}"; do
    emulator_args+=(--emulator "${e}")
done

# ---- Keep the window open afterward? ----
keep_open=1
if have_kdialog; then
    kdialog --title "DualDeck Host" --yesno "Keep this window open after finishing so you can read the output? (Recommended)" 2>/dev/null || keep_open=0
else
    reply=""
    read -rp "Keep this window open after finishing so you can read the output? [Y/n] " reply < /dev/tty || true
    [[ "${reply}" =~ ^[Nn]$ ]] && keep_open=0
fi

# ---- Wrapper: runs the tool, then (if requested) pauses on a "Press
# Enter" prompt regardless of success/failure before the window closes.
# Deliberately NOT `set -e` in this generated wrapper -- with it, a
# failing tool invocation would skip straight past the pause below
# instead of leaving the error visible, defeating the entire point.
run_wrapper="$(mktemp --suffix=.sh)"
{
    echo '#!/usr/bin/env bash'
    printf '%q ' "${tool}" "${emulator_args[@]}"
    echo
    echo 'status=$?'
    if [[ "${keep_open}" -eq 1 ]]; then
        echo 'echo'
        echo 'if [ "${status}" -eq 0 ]; then echo "Finished."; else echo "Finished with an error (exit code ${status}) -- see the output above."; fi'
        echo 'echo "Press Enter to close this window..."'
        echo 'read -r'
    fi
    echo 'rm -f "$0"'
    echo 'exit "${status}"'
} > "${run_wrapper}"
chmod +x "${run_wrapper}"

# Already has a real terminal (stdout is a tty) -- just run it here,
# same as every other menu choice in this script.
if [[ -t 1 ]]; then
    exec "${run_wrapper}"
fi

# No terminal attached -- re-launch inside whichever terminal emulator
# is available, trying the ones most likely to exist on SteamOS/Bazzite
# (both KDE Plasma, so konsole) first.
for term_cmd in "konsole -e" "xterm -e" "x-terminal-emulator -e" "gnome-terminal --"; do
    # shellcheck disable=SC2086
    set -- ${term_cmd}
    term_bin="$1"
    if command -v "${term_bin}" >/dev/null 2>&1; then
        exec "$@" "${run_wrapper}"
    fi
done

rm -f "${run_wrapper}"
manual_cmd="${tool}"
for a in "${emulator_args[@]}"; do
    manual_cmd="${manual_cmd} $(printf '%q' "${a}")"
done
msg="This needs a terminal window to show download progress and ask y/N
before replacing each emulator, but no terminal emulator (konsole,
xterm, gnome-terminal) was found to open one automatically.

Open a terminal yourself and run:
${manual_cmd}"
if have_kdialog; then
    kdialog --title "DualDeck Host" --error "${msg}" 2>/dev/null || true
else
    echo "error: ${msg}" >&2
fi
exit 1
