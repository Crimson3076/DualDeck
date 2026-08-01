#!/usr/bin/env bash
# Shared by host/internal/install-steam-shortcut.sh and
# host/internal/install-host-distrobox.sh (copied in by
# scripts/build-release.sh alongside ensure-packages.sh, same pattern --
# meant to be `source`d, not executed directly).
#
# Real user report, 2026-08-01 (Bazzite HTPC): the host's remote server
# never showed up on the client even launching directly via the DualDeck
# Host GUI, and docs/bazzite-host-setup.md's firewall section (manual
# `firewall-cmd --add-port=...`) had to be run by hand -- the user asked
# for this to be built into the install instead of a manual step they
# have to look up and type themselves.
#
# Opens the five ports host/remote-server/include/host/net_server.h
# actually binds (NetServerConfig's defaults -- see that header, kept in
# sync here manually since there's no shared machine-readable source of
# truth for these yet):
#   8760/tcp control, 8761/udp input, 8762/tcp video, 8763/udp discovery,
#   8765/udp audio.
# Firewalld runs at the host OS level, not per-container, so this is the
# same fix regardless of whether the host itself ends up running
# natively or inside a Distrobox container (Distrobox shares the host's
# network namespace by default -- no separate firewall to configure
# there).
#
# Best-effort and non-fatal by design: this only ever *adds* to whatever
# firewall config already exists, never modifies unrelated rules, and a
# failure here (no supported firewall manager, sudo declined, etc.)
# logs a clear manual fallback and returns non-zero rather than aborting
# the install that called it -- getting the shortcut installed matters
# more than this succeeding, and the manual firewall-cmd command in
# docs/bazzite-host-setup.md still exists as a fallback.

# ensure_host_firewall_ports
#
# Idempotent -- safe to call on every install (re-adding an already-open
# port is a harmless no-op for both firewalld and ufw). Detects and
# supports firewalld (Bazzite/Fedora's default, this project's primary
# documented HTPC target) and ufw (Debian/Ubuntu-based hosts). Does not
# attempt raw iptables/nftables manipulation directly -- too easy to get
# wrong outside a higher-level tool, and neither of this project's
# documented host platforms needs it.
ensure_host_firewall_ports() {
    local tcp_ports=(8760 8762)
    local udp_ports=(8761 8763 8765)

    if command -v firewall-cmd >/dev/null 2>&1; then
        _ensure_host_firewall_ports_firewalld "${tcp_ports[@]}" -- "${udp_ports[@]}"
        return $?
    fi

    if command -v ufw >/dev/null 2>&1; then
        _ensure_host_firewall_ports_ufw "${tcp_ports[@]}" -- "${udp_ports[@]}"
        return $?
    fi

    echo "host firewall: no supported firewall manager found (firewalld, ufw) --" >&2
    echo "skipping automatic port setup. If the client can't find or connect to" >&2
    echo "this host, your firewall (if any) may be blocking it -- see" >&2
    echo "docs/bazzite-host-setup.md's Firewall section for the ports to open" >&2
    echo "manually." >&2
    return 1
}

# _ensure_host_firewall_ports_firewalld <tcp-port>... -- <udp-port>...
_ensure_host_firewall_ports_firewalld() {
    local tcp=() udp=() seen_sep=0
    for a in "$@"; do
        if [[ "${a}" == "--" ]]; then seen_sep=1; continue; fi
        if [[ "${seen_sep}" -eq 0 ]]; then tcp+=("${a}"); else udp+=("${a}"); fi
    done

    local args=()
    for p in "${tcp[@]}"; do args+=(--add-port="${p}/tcp"); done
    for p in "${udp[@]}"; do args+=(--add-port="${p}/udp"); done

    echo "host firewall: opening DualDeck's ports via firewalld (permanent + reload)..."
    local sudo_cmd=""
    [[ "$(id -u)" -ne 0 ]] && sudo_cmd="sudo"
    if ! ${sudo_cmd} firewall-cmd --permanent "${args[@]}" 2>&1; then
        echo "host firewall: firewall-cmd failed -- see docs/bazzite-host-setup.md's" >&2
        echo "Firewall section to open these ports manually instead." >&2
        return 1
    fi
    if ! ${sudo_cmd} firewall-cmd --reload 2>&1; then
        echo "host firewall: ports were added but 'firewall-cmd --reload' failed --" >&2
        echo "they'll take effect on next boot, or run 'sudo firewall-cmd --reload'" >&2
        echo "yourself to apply them now." >&2
        return 1
    fi
    echo "host firewall: done (${tcp[*]/%/\/tcp} ${udp[*]/%/\/udp})."
    return 0
}

# _ensure_host_firewall_ports_ufw <tcp-port>... -- <udp-port>...
_ensure_host_firewall_ports_ufw() {
    local tcp=() udp=() seen_sep=0
    for a in "$@"; do
        if [[ "${a}" == "--" ]]; then seen_sep=1; continue; fi
        if [[ "${seen_sep}" -eq 0 ]]; then tcp+=("${a}"); else udp+=("${a}"); fi
    done

    echo "host firewall: opening DualDeck's ports via ufw..."
    local sudo_cmd=""
    [[ "$(id -u)" -ne 0 ]] && sudo_cmd="sudo"
    local p
    for p in "${tcp[@]}"; do
        ${sudo_cmd} ufw allow "${p}/tcp" >/dev/null 2>&1 || {
            echo "host firewall: 'ufw allow ${p}/tcp' failed -- see" >&2
            echo "docs/bazzite-host-setup.md's Firewall section to open it manually." >&2
            return 1
        }
    done
    for p in "${udp[@]}"; do
        ${sudo_cmd} ufw allow "${p}/udp" >/dev/null 2>&1 || {
            echo "host firewall: 'ufw allow ${p}/udp' failed -- see" >&2
            echo "docs/bazzite-host-setup.md's Firewall section to open it manually." >&2
            return 1
        }
    done
    echo "host firewall: done (${tcp[*]/%/\/tcp} ${udp[*]/%/\/udp})."
    return 0
}
