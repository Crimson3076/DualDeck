#!/usr/bin/env bash
# Undoes install-host-distrobox.sh: removes the Distrobox container it
# created and the central install directory it copied files into. Only
# ever touches things this project itself created -- ROMs, saves,
# firmware, and any other melonDS data all live in your normal shared
# home directory (Distrobox mounts it into the container automatically),
# never inside the container or the central install directory, so none
# of that is affected either way (GitHub issue #10: "uninstall removes
# only DualDeck-installed files... without deleting ROMs, saves,
# firmware, or unrelated melonDS data").
#
# Safe to re-run -- does nothing (not an error) if already uninstalled,
# or if install-host-distrobox.sh was never run at all.
set -uo pipefail

# Keep in sync with the same paths in install-host-distrobox.sh,
# install-steam-shortcut.sh, and uninstall-steam-shortcut.sh.
central_install_dir="${HOME}/.config/dualdeck/install"
container_name="dualdeck-host"

removed_anything=0

# Captured, not piped into `grep -q` -- see run-host-azahar.sh's comment
# for why that combination silently reports "not found" under pipefail.
# An uninstaller that quietly skips removing the container is precisely
# the bug that pattern produces here.
distrobox_containers="$(if command -v distrobox >/dev/null 2>&1; then distrobox list 2>/dev/null || true; fi)"
if grep -qw "${container_name}" <<<"${distrobox_containers}"; then
    echo "Removing Distrobox container \"${container_name}\" ..."
    distrobox rm "${container_name}" --force
    removed_anything=1
fi

for dir in "${central_install_dir}" "${central_install_dir}.new" "${central_install_dir}.previous"; do
    if [[ -d "${dir}" ]]; then
        echo "Removing ${dir} ..."
        rm -rf -- "${dir}"
        removed_anything=1
    fi
done

# check-for-updates.sh/VERSION are staged as siblings of install/ (see
# install-host-distrobox.sh/install-steam-shortcut.sh), not inside any
# of the three directories just removed above -- clean those up too.
for file in "$(dirname "${central_install_dir}")/check-for-updates.sh" "$(dirname "${central_install_dir}")/VERSION"; do
    if [[ -f "${file}" ]]; then
        rm -f -- "${file}"
        removed_anything=1
    fi
done

if [[ "${removed_anything}" -eq 0 ]]; then
    echo "Nothing installed -- already uninstalled, or install-host-distrobox.sh was never run."
fi
