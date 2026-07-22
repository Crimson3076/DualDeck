#!/usr/bin/env bash
# The single file a new user needs to download to install DualDeck
# (GitHub issue #26's "Desired first-install experience") -- no need to
# already have a release archive extracted first. Presents Install
# Client / Install Host / Install Both / Repair / Uninstall via kdialog
# (SteamOS/Bazzite/any KDE desktop) -> zenity -> a plain terminal menu
# as the fallback chain, or via non-interactive --client/--host/--both/
# --repair/--uninstall flags for scripting.
#
# Deliberately a thin, safe wrapper, not a from-scratch reimplementation:
# it downloads the latest published release archive, verifies its
# SHA256SUMS checksum *before* extracting anything, then delegates to
# that archive's own client/internal/install-steam-shortcut.sh and
# host/internal/install-steam-shortcut.sh (or their uninstall-*
# counterparts) -- the same well-tested atomic stage-then-swap scripts
# every existing install/update path already goes through (see those
# files for the actual file-safety logic: staged into a `.new` sibling
# directory first, only swapped into place once staging succeeds, one
# generation kept as `.previous`).
#
# This is Phase 1 of GitHub issue #26 specifically: a single downloadable
# bootstrap entry point plus download integrity verification. The
# issue's later phases -- an XDG-standard ~/.local/share/dualdeck/
# install layout with versioned symlinks, a JSON release manifest with
# separately-published host/client archives, and unified launch-time
# auto-update for both components -- are intentionally not attempted
# here; today's already-working per-component install/update scripts
# (each with their own atomic staging and update-on-launch behavior) are
# reused as-is rather than replaced in the same change that adds this
# entry point. See docs/known-limitations.md for the exact list of what
# issue #26 asks for that remains outstanding.
set -uo pipefail

repo="Crimson3076/DualDeck"
archive_name="dualdeck-linux-x86_64.tar.gz"
log_file="${HOME}/.config/dualdeck-installer.log"

log() {
    mkdir -p "$(dirname "${log_file}")" 2>/dev/null || true
    echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") $1" >> "${log_file}" 2>/dev/null || true
}

have_kdialog() { command -v kdialog >/dev/null 2>&1 && [[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]]; }
have_zenity() { command -v zenity >/dev/null 2>&1 && [[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]]; }

info() {
    log "info: $1"
    if have_kdialog; then
        kdialog --title "DualDeck Installer" --msgbox "$1" 2>/dev/null
    elif have_zenity; then
        zenity --title "DualDeck Installer" --info --text "$1" 2>/dev/null
    else
        echo
        echo "$1"
        echo
    fi
}

fail() {
    log "error: $1"
    if have_kdialog; then
        kdialog --title "DualDeck Installer" --error "$1

Details logged to:
${log_file}" 2>/dev/null || true
    elif have_zenity; then
        zenity --title "DualDeck Installer" --error --text "$1

Details logged to:
${log_file}" 2>/dev/null || true
    else
        echo "error: $1" >&2
        echo "Details logged to: ${log_file}" >&2
    fi
    exit 1
}

confirm() {
    if have_kdialog; then
        kdialog --title "DualDeck Installer" --yesno "$1" 2>/dev/null
    elif have_zenity; then
        zenity --title "DualDeck Installer" --question --text "$1" 2>/dev/null
    else
        read -rp "$1 [y/N] " reply
        [[ "${reply}" =~ ^[Yy]$ ]]
    fi
}

# Informational only (GitHub issue #26: "identify Steam Deck/SteamOS,
# Bazzite, or a conventional Linux desktop when possible") -- nothing
# below branches on this. The actual immutable-OS-vs-regular distinction
# that matters for the host (Distrobox or not) is already handled at
# every launch by host/internal/launch-host.sh inside the archive
# itself, not at install time, so duplicating that detection here would
# just be a second, potentially-drifting copy of the same logic.
detect_platform() {
    local id="" id_like=""
    if [[ -r /etc/os-release ]]; then
        # shellcheck disable=SC1091
        id="$(. /etc/os-release && echo "${ID:-}")"
        # shellcheck disable=SC1091
        id_like="$(. /etc/os-release && echo "${ID_LIKE:-}")"
    fi
    case "${id}" in
        steamos) echo "Steam Deck (SteamOS)" ;;
        bazzite) echo "Bazzite" ;;
        *)
            if [[ "${id_like}" == *bazzite* ]]; then
                echo "Bazzite-based Linux"
            else
                echo "Linux desktop (${id:-unknown distro})"
            fi
            ;;
    esac
}

choose_action() {
    if have_kdialog; then
        kdialog --title "DualDeck Installer" --menu "What would you like to do?" \
            client "Install Client" \
            host "Install Host" \
            both "Install Both" \
            repair "Repair Existing Installation" \
            uninstall "Uninstall" \
            2>/dev/null || echo "cancel"
    elif have_zenity; then
        zenity --title "DualDeck Installer" --list --radiolist \
            --column "" --column "Action" \
            TRUE "Install Client" \
            FALSE "Install Host" \
            FALSE "Install Both" \
            FALSE "Repair Existing Installation" \
            FALSE "Uninstall" \
            2>/dev/null | sed -e 's/Install Client/client/' -e 's/Install Host/host/' \
                -e 's/Install Both/both/' -e 's/Repair Existing Installation/repair/' \
                -e 's/Uninstall/uninstall/' || echo "cancel"
    else
        {
            echo "DualDeck Installer -- detected: $(detect_platform)"
            echo
            echo "  1) Install Client"
            echo "  2) Install Host"
            echo "  3) Install Both"
            echo "  4) Repair Existing Installation"
            echo "  5) Uninstall"
            echo "  6) Exit"
        } >&2
        read -rp "Choice [1-6]: " choice
        case "${choice}" in
            1) echo "client" ;;
            2) echo "host" ;;
            3) echo "both" ;;
            4) echo "repair" ;;
            5) echo "uninstall" ;;
            *) echo "cancel" ;;
        esac
    fi
}

usage() {
    cat <<USAGE
Usage: $(basename "$0") [--client | --host | --both | --repair | --uninstall]

With no arguments, shows an interactive menu (graphical if kdialog or
zenity is available, a terminal prompt otherwise).

  --client     Install (or update) the Steam Deck client only
  --host       Install (or update) the HTPC host only
  --both       Install (or update) both
  --repair     Re-install over an existing installation, bypassing the
               "Steam is running" safety check that a fresh install
               respects (matches how automatic updates already behave)
  --uninstall  Remove the Steam shortcut(s) and installed files for
               both components (never touches ROMs, saves, or firmware)
USAGE
}

action=""
case "${1:-}" in
    --client) action="client" ;;
    --host) action="host" ;;
    --both) action="both" ;;
    --repair) action="repair" ;;
    --uninstall) action="uninstall" ;;
    --help|-h) usage; exit 0 ;;
    "") ;;
    *) echo "unrecognized argument: $1" >&2; usage >&2; exit 2 ;;
esac

if [[ "$(uname -m)" != "x86_64" ]]; then
    fail "DualDeck currently only publishes x86_64 Linux builds. Detected: $(uname -m)."
fi

if [[ -z "${action}" ]]; then
    action="$(choose_action)"
fi

if [[ "${action}" == "cancel" || -z "${action}" ]]; then
    exit 0
fi

log "starting action=${action} platform=$(detect_platform)"

if ! command -v curl >/dev/null 2>&1; then
    fail "curl is required to download DualDeck -- install it and try again."
fi
if ! command -v sha256sum >/dev/null 2>&1; then
    fail "sha256sum is required to verify the download -- install coreutils and try again."
fi
if ! command -v tar >/dev/null 2>&1; then
    fail "tar is required to extract the download -- install it and try again."
fi

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

# Overridable for testing only (e.g. pointing at a local fake HTTP
# server serving fixture release assets) -- never something a real user
# needs to set.
download_base="${DUALDECK_INSTALLER_DOWNLOAD_BASE:-https://github.com/${repo}/releases/latest/download}"

echo "Downloading DualDeck..."
if ! curl -fsSL --max-time 180 -o "${work_dir}/${archive_name}" "${download_base}/${archive_name}"; then
    fail "Couldn't download ${archive_name} from the latest GitHub release. Check your internet connection, or download it manually from https://github.com/${repo}/releases/latest"
fi
if ! curl -fsSL --max-time 30 -o "${work_dir}/SHA256SUMS" "${download_base}/SHA256SUMS"; then
    fail "Couldn't download SHA256SUMS from the latest GitHub release -- refusing to install an unverified download."
fi

echo "Verifying download integrity..."
if ! (cd "${work_dir}" && sha256sum -c --ignore-missing SHA256SUMS) >/dev/null 2>&1; then
    fail "Checksum verification failed -- the downloaded archive doesn't match SHA256SUMS. Not installing anything. Try again, or download manually from https://github.com/${repo}/releases/latest"
fi
log "checksum verified OK"

echo "Extracting..."
tar xzf "${work_dir}/${archive_name}" -C "${work_dir}"

extracted_dir=""
for candidate in "${work_dir}"/melonds-remote-*; do
    [[ -d "${candidate}" ]] && extracted_dir="${candidate}" && break
done
if [[ -z "${extracted_dir}" ]]; then
    fail "Downloaded archive didn't contain the expected directory structure -- this looks like a packaging bug, not a network problem."
fi

install_client() {
    local force_flag=()
    [[ "${action}" == "repair" ]] && force_flag=(--force)
    echo "Installing client..."
    if "${extracted_dir}/client/internal/install-steam-shortcut.sh" "${force_flag[@]}"; then
        log "client install/repair succeeded"
    else
        fail "Client installation failed. Details logged to ${HOME}/.config/dualdeck-client/install.log and ${log_file}."
    fi
}

install_host() {
    local force_flag=()
    [[ "${action}" == "repair" ]] && force_flag=(--force)
    echo "Installing host..."
    if "${extracted_dir}/host/internal/install-steam-shortcut.sh" "${force_flag[@]}"; then
        log "host install/repair succeeded"
    else
        fail "Host installation failed. Details logged to ${HOME}/.config/dualdeck/install.log and ${log_file}."
    fi
}

uninstall_both() {
    local any_failed=0
    echo "Removing client..."
    "${extracted_dir}/client/internal/uninstall-steam-shortcut.sh" || any_failed=1
    echo "Removing host..."
    "${extracted_dir}/host/internal/uninstall-steam-shortcut.sh" || any_failed=1
    if [[ "${any_failed}" -ne 0 ]]; then
        fail "One or more components failed to uninstall cleanly. Details logged to ${log_file} and the per-component install logs."
    fi
    log "uninstall succeeded"
}

case "${action}" in
    client)
        install_client
        info "Client installed. Restart Steam (or switch to Gaming Mode) to see the shortcut."
        ;;
    host)
        install_host
        info "Host installed. Restart Steam (or switch to Gaming Mode) to see the shortcut."
        ;;
    both)
        install_client
        install_host
        info "Client and host installed. Restart Steam (or switch to Gaming Mode) to see both shortcuts."
        ;;
    repair)
        if confirm "This re-installs both the client and host over any existing installation. Settings, device identity, and approved devices are preserved. Continue?"; then
            install_client
            install_host
            info "Repair complete."
        fi
        ;;
    uninstall)
        if confirm "This removes the Steam shortcut(s) and installed files for both the client and host. Your ROMs, saves, and firmware are never touched. Continue?"; then
            uninstall_both
            info "Uninstalled."
        fi
        ;;
esac
