#!/usr/bin/env bash
# Guards against the exact bug fixed 2026-08-01 (see docs/known-
# limitations.md's "All three host patches embed a frozen protocol copy"
# entry): host/{melonds,azahar,cemu}-patches/0001-remote-server-
# integration.patch each used to embed their own vendored copy of
# protocol.h (and the rest of adapter-sdk/), which silently fell behind
# the live header across several unrelated changes.
#
# The patches no longer carry those files: each emulator's
# shared-files.txt lists the shared sources scripts/lib/emulator_patch.sh
# copies in from this repository at apply time, so the live copy is the
# only copy. This script now checks that arrangement stays intact: every
# listed source exists, and no patch has grown its own copy of a listed
# file again (e.g. from regenerating a patch without excluding them).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

live_version="$(grep -oP 'inline constexpr uint16_t kProtocolVersion = \K[0-9]+' \
    "${repo_root}/protocol/include/dualdeck/protocol.h")"

if [[ -z "${live_version}" ]]; then
    echo "error: couldn't find kProtocolVersion in protocol/include/dualdeck/protocol.h" >&2
    exit 1
fi

echo "live protocol/include/dualdeck/protocol.h: kProtocolVersion=${live_version}"

failed=0
for patch_dir in host/melonds-patches host/azahar-patches host/cemu-patches; do
    patch_path="${repo_root}/${patch_dir}/0001-remote-server-integration.patch"
    manifest="${repo_root}/${patch_dir}/shared-files.txt"
    for f in "${patch_path}" "${manifest}"; do
        if [[ ! -f "${f}" ]]; then
            echo "error: ${f#"${repo_root}"/} not found" >&2
            failed=1
            continue 2
        fi
    done
    count=0
    dir_failed=0
    while read -r from to; do
        count=$((count + 1))
        if [[ ! -f "${repo_root}/${from}" ]]; then
            echo "error: ${patch_dir}/shared-files.txt lists ${from}, which doesn't exist" >&2
            dir_failed=1
        fi
        if grep -qF "diff --git a/${to} b/${to}" "${patch_path}"; then
            echo "error: ${patch_dir}'s patch adds its own copy of ${to} -- drop it from the patch, shared-files.txt already copies it from ${from}" >&2
            dir_failed=1
        fi
    done < <(grep -v -E '^[[:space:]]*(#|$)' "${manifest}")
    if [[ "${dir_failed}" -eq 0 ]]; then
        echo "${patch_dir}: ${count} shared files, copied from the live tree -- in sync"
    else
        failed=1
    fi
done

# Real 2026-08-03 CI failures (twice, in the same commit's follow-up):
# tests/smoke_test.py and tests/device_approval_smoke_test.py each hand-
# maintain their own VERSION constant (no build step reads the live
# header) since they hand-construct raw Hello packets against a real
# dualdeck-host-service binary -- a stale value silently changes what
# every test case actually exercises (every Hello gets rejected as a
# protocol-version mismatch before reaching the specific condition under
# test) rather than failing loudly on its own. Catches this the same way
# the patch loop above catches patch drift, so it can't happen a third
# time unnoticed.
for test_script in tests/smoke_test.py tests/device_approval_smoke_test.py; do
    test_script_path="${repo_root}/${test_script}"
    if [[ ! -f "${test_script_path}" ]]; then
        echo "error: ${test_script} not found" >&2
        failed=1
        continue
    fi
    test_version="$(grep -oP '^VERSION = \K[0-9]+' "${test_script_path}" | head -1 || true)"
    if [[ -z "${test_version}" ]]; then
        echo "warning: ${test_script} doesn't have a top-level VERSION constant -- skipping (not necessarily an error, but check by hand)" >&2
        continue
    fi
    if [[ "${test_version}" != "${live_version}" ]]; then
        echo "error: ${test_script} embeds VERSION=${test_version}, live header is ${live_version} -- update its VERSION constant to match" >&2
        failed=1
    else
        echo "${test_script}: VERSION=${test_version} -- in sync"
    fi
done

exit "${failed}"
