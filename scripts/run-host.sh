#!/usr/bin/env bash
# Runs the standalone melonds-remote-server prototype (see
# docs/architecture.md for why it doesn't drive a real melonDS instance
# yet). Builds first if the binary isn't present.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_root}/build"
binary="${build_dir}/host/remote-server/melonds-remote-server"

if [[ ! -x "${binary}" ]]; then
    echo "Host server binary not found, building..." >&2
    cmake -S "${repo_root}" -B "${build_dir}" -DMELONDS_REMOTE_BUILD_CLIENT=OFF
    cmake --build "${build_dir}" -j"$(nproc)"
fi

exec "${binary}" "$@"
