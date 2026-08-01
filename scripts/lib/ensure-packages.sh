#!/usr/bin/env bash
# Shared "install this if missing" helper, meant to be `source`d (not
# executed directly) by scripts/build-release.sh and by the
# run-host.sh/run-client.sh scripts it generates inside the release
# package -- so the same detection logic works both for someone building
# from source and for someone who just downloaded a release tarball.
#
# Deliberately conservative on immutable-filesystem distros (Bazzite,
# SteamOS in Game Mode): installing packages there either needs a reboot
# (rpm-ostree) or an explicitly-disabled read-only root (SteamOS), so
# this refuses to guess and instead points at the documented Distrobox
# path (docs/bazzite-host-setup.md, docs/steam-deck-setup.md) or the
# manual command to run yourself.

# is_immutable_system
#
# True (0) if this looks like an rpm-ostree (immutable) system -- the
# same check ensure_packages() below uses to refuse outright. Factored
# out so a caller that wants to react differently instead of just
# refusing (scripts/emudeck-replace-in-place.sh building inside a
# Distrobox container instead, rather than failing) shares this exact
# detection rather than a second, silently-drifting copy.
is_immutable_system() {
    [[ -f /run/ostree-booted ]] || command -v rpm-ostree >/dev/null 2>&1
}

# ensure_packages <label> <apt-package-list> <dnf-package-list> <pacman-package-list>
#
# Each package-list argument is one space-separated string of package
# names for that package manager (they differ, so all three are always
# needed even though only one is used on any given machine). Checks
# which of the packages for the *detected* manager are missing, and
# installs only those (idempotent -- a no-op if everything's already
# present). Returns non-zero (without raising, since callers generally
# want to continue and let the actual build/run attempt surface a
# clearer error) if packages couldn't be verified/installed for any
# reason -- immutable filesystem, no recognized package manager, or the
# install command itself failing.
ensure_packages() {
    local label="$1" apt_list="$2" dnf_list="$3" pacman_list="$4"

    if is_immutable_system; then
        echo "This looks like an rpm-ostree (immutable) system, e.g. Bazzite." >&2
        echo "Auto-installing ${label} packages onto an immutable base isn't done" >&2
        echo "unattended here since it needs a reboot to take effect. Either:" >&2
        echo "  - build/run inside a Distrobox instead (see docs/bazzite-host-setup.md), or" >&2
        echo "  - run: sudo rpm-ostree install <packages> && systemctl reboot" >&2
        return 1
    fi
    if command -v steamos-readonly >/dev/null 2>&1; then
        echo "This looks like SteamOS. Its root filesystem is read-only by default." >&2
        echo "Run 'sudo steamos-readonly disable' first, or build/run inside a" >&2
        echo "Distrobox instead -- see docs/steam-deck-setup.md." >&2
        return 1
    fi

    local sudo_cmd=""
    if [[ "$(id -u)" -ne 0 ]]; then
        if command -v sudo >/dev/null 2>&1; then
            sudo_cmd="sudo"
        else
            echo "Not running as root and no 'sudo' found; install these ${label}" >&2
            echo "packages manually: ${apt_list}" >&2
            return 1
        fi
    fi

    local -a pkgs=()
    local -a missing=()

    if command -v apt-get >/dev/null 2>&1; then
        read -ra pkgs <<< "${apt_list}"
        for p in "${pkgs[@]}"; do
            dpkg -s "${p}" >/dev/null 2>&1 || missing+=("${p}")
        done
        if [[ ${#missing[@]} -eq 0 ]]; then
            echo "${label}: all dependencies already installed (apt)."
            return 0
        fi
        echo "${label}: installing ${#missing[@]} missing package(s) via apt: ${missing[*]}"
        ${sudo_cmd} apt-get update
        ${sudo_cmd} apt-get install -y --no-install-recommends "${missing[@]}"
    elif command -v dnf >/dev/null 2>&1; then
        read -ra pkgs <<< "${dnf_list}"
        for p in "${pkgs[@]}"; do
            rpm -q "${p}" >/dev/null 2>&1 || missing+=("${p}")
        done
        if [[ ${#missing[@]} -eq 0 ]]; then
            echo "${label}: all dependencies already installed (dnf)."
            return 0
        fi
        echo "${label}: installing ${#missing[@]} missing package(s) via dnf: ${missing[*]}"
        ${sudo_cmd} dnf install -y "${missing[@]}"
    elif command -v pacman >/dev/null 2>&1; then
        read -ra pkgs <<< "${pacman_list}"
        for p in "${pkgs[@]}"; do
            pacman -Qi "${p}" >/dev/null 2>&1 || missing+=("${p}")
        done
        if [[ ${#missing[@]} -eq 0 ]]; then
            echo "${label}: all dependencies already installed (pacman)."
            return 0
        fi
        echo "${label}: installing ${#missing[@]} missing package(s) via pacman: ${missing[*]}"
        ${sudo_cmd} pacman -Sy --noconfirm --needed "${missing[@]}"
    else
        echo "No supported package manager (apt/dnf/pacman) found." >&2
        echo "Install these ${label} packages manually -- apt: ${apt_list}" >&2
        echo "  -- dnf: ${dnf_list}" >&2
        echo "  -- pacman: ${pacman_list}" >&2
        return 1
    fi
}
