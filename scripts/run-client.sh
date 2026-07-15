#!/usr/bin/env bash
# Runs the SDL3 Steam Deck client. Requires SDL3 development packages to be
# installed (see docs/building.md) -- not available in every environment,
# unlike run-host.sh.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_root}/build"
binary="${build_dir}/client/melonds-remote-client"
host_address="${1:-127.0.0.1}"

if [[ ! -x "${binary}" ]]; then
    echo "Client binary not found, building..." >&2
    cmake -S "${repo_root}" -B "${build_dir}" -DMELONDS_REMOTE_BUILD_CLIENT=ON
    cmake --build "${build_dir}" -j"$(nproc)"
fi

exec "${binary}" "${host_address}"
