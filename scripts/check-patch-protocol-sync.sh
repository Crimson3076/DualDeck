#!/usr/bin/env bash
# Guards against the exact bug fixed 2026-08-01 (see docs/known-
# limitations.md's "All three host patches embed a frozen protocol copy"
# entry): host/{melonds,azahar,cemu}-patches/0001-remote-server-
# integration.patch each embed their own vendored copy of protocol.h
# rather than referencing the live protocol/ directory, and nobody
# noticed the embedded copy silently falling further behind the live
# header (v7 vs v10, three real feature releases) across several
# unrelated changes elsewhere in the repo -- "the patch is untouched" was
# read as evidence of good scoping in each of those changes individually,
# without anyone tracking that the patch was simultaneously drifting
# further out of sync every time. This is not a full guarantee the three
# patches are fully in sync (a change to protocol.h's *content* without a
# kProtocolVersion bump, or a change to adapter_contract.h/net_server.h/
# etc. that isn't reflected here, could still slip through silently) --
# just the cheapest possible tripwire for the specific failure mode that
# actually happened: a bumped kProtocolVersion nobody ever propagated.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

live_version="$(grep -oP 'inline constexpr uint16_t kProtocolVersion = \K[0-9]+' \
    "${repo_root}/protocol/include/melonds_remote/protocol.h")"

if [[ -z "${live_version}" ]]; then
    echo "error: couldn't find kProtocolVersion in protocol/include/melonds_remote/protocol.h" >&2
    exit 1
fi

echo "live protocol/include/melonds_remote/protocol.h: kProtocolVersion=${live_version}"

failed=0
for patch in host/melonds-patches/0001-remote-server-integration.patch \
             host/azahar-patches/0001-remote-server-integration.patch \
             host/cemu-patches/0001-remote-server-integration.patch; do
    patch_path="${repo_root}/${patch}"
    if [[ ! -f "${patch_path}" ]]; then
        echo "error: ${patch} not found" >&2
        failed=1
        continue
    fi
    # The embedded copy's kProtocolVersion line appears as a diff
    # addition ("+inline constexpr ..."), not the file's own literal
    # content -- match either form so this doesn't care whether the
    # patch adds the file fresh (today's case) or modifies an existing
    # one (hypothetically, if melonDS/Azahar/Cemu ever shipped their own
    # copy upstream).
    patch_version="$(grep -oP '^\+?inline constexpr uint16_t kProtocolVersion = \K[0-9]+' \
        "${patch_path}" | head -1 || true)"
    if [[ -z "${patch_version}" ]]; then
        echo "warning: ${patch} doesn't embed a kProtocolVersion at all -- skipping (not necessarily an error, but check by hand)" >&2
        continue
    fi
    if [[ "${patch_version}" != "${live_version}" ]]; then
        echo "error: ${patch} embeds kProtocolVersion=${patch_version}, live header is ${live_version} -- this patch needs regenerating (see docs/known-limitations.md's 2026-08-01 entry for how)" >&2
        failed=1
    else
        echo "${patch}: kProtocolVersion=${patch_version} -- in sync"
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
