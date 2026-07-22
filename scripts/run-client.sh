#!/usr/bin/env bash
# Runs the SDL3 Steam Deck client. Requires SDL3 development packages to be
# installed (see docs/building.md) -- not available in every environment,
# unlike run-host.sh.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_root}/build"
binary="${build_dir}/client/dualdeck-client"

if [[ ! -x "${binary}" ]]; then
    echo "Client binary not found, building..." >&2
    cmake -S "${repo_root}" -B "${build_dir}" -DDUALDECK_BUILD_CLIENT=ON
    cmake --build "${build_dir}" -j"$(nproc)"
fi

# Forwards all arguments, e.g.:
#   ./scripts/run-client.sh 127.0.0.1
#   ./scripts/run-client.sh --host 127.0.0.1 --auth-token some-shared-secret
if [[ $# -eq 0 ]]; then
    set -- "127.0.0.1"
fi
exec "${binary}" "$@"
