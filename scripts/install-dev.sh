#!/usr/bin/env bash
# Convenience script for setting up a development build (protocol + host
# server + tests, strict warnings, no SDL3 required). Client build is
# opt-in via --with-client since it needs SDL3 installed separately.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_root}/build"

with_client=OFF
for arg in "$@"; do
    case "${arg}" in
        --with-client) with_client=ON ;;
        *) echo "unrecognized argument: ${arg}" >&2; exit 1 ;;
    esac
done

cmake -S "${repo_root}" -B "${build_dir}" \
    -DMELONDS_REMOTE_BUILD_CLIENT="${with_client}" \
    -DMELONDS_REMOTE_ENABLE_WARNINGS=ON \
    -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic -Wconversion -Wshadow"

cmake --build "${build_dir}" -j"$(nproc)"
ctest --test-dir "${build_dir}" --output-on-failure

echo
echo "Build complete. Run the integration smoke test with:"
echo "  python3 ${repo_root}/tests/smoke_test.py ${build_dir}/host/remote-server/melonds-remote-server"
