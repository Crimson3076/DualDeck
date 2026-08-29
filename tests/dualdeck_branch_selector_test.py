#!/usr/bin/env python3
"""Regression/behavior test for Advanced -> Installation branch
(`scripts/lib/dualdeck_branch.sh`, plus its wiring into the generated
`dualdeck-host.sh` menu and `host/internal/install-branch.sh`).

Covers the real requirements this feature was built against:
  - branch discovery is paginated and cached, with an explicit Refresh
  - changing the selected branch never installs anything by itself
  - offline / GitHub-rate-limited / deleted-branch / no-release-for-this-
    commit are all distinct, non-crashing outcomes that install nothing
  - it never silently falls back to `main` on any failure
  - branch names are validated before ever reaching a URL or a path
  - a successful "Install selected branch" verifies the download's
    checksum before extracting anything, and only records the branch as
    installed once the whole staged install actually completes
  - a failed step (bad checksum, network failure) leaves the previous
    install/state completely untouched -- no partial "success"

None of this touches the real GitHub API: a small fake `curl` fixture on
PATH answers exactly the requests `dualdeck_branch.sh` and
`install-branch.sh` make, covering success, pagination, 404, and
403-rate-limited cases, plus a real (checksummed) fixture release
archive for the install path.

Usage:
    python3 tests/dualdeck_branch_selector_test.py
"""

import hashlib
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BUILD_RELEASE = REPO_ROOT / "scripts" / "build-release.sh"
BRANCH_LIB = REPO_ROOT / "scripts" / "lib" / "dualdeck_branch.sh"

REPO = "Crimson3076/DualDeck"


def extract_heredoc(src: str, marker: str) -> str:
    idx = src.index(marker)
    start = src.index("\n", idx) + 1
    end = src.index("\nWRAP\n", start)
    return src[start:end]


def write_exec(path: Path, content: str) -> None:
    path.write_text(content)
    path.chmod(path.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)


FAKE_CURL_TEMPLATE = r"""#!/usr/bin/env bash
# Fake curl for testing dualdeck_branch.sh / install-branch.sh against a
# scripted set of GitHub API + release-download responses, without any
# real network access.
if [[ -n "${DUALDECK_FAKE_CURL_OFFLINE:-}" ]]; then exit 7; fi
headers="" body="" url="" want_status=0
args=("$@")
i=0
while [[ $i -lt ${#args[@]} ]]; do
  case "${args[$i]}" in
    -D) i=$((i+1)); headers="${args[$i]}" ;;
    -o) i=$((i+1)); body="${args[$i]}" ;;
    -H|--max-time) i=$((i+1)) ;;
    -w) i=$((i+1)); want_status=1 ;;
    -*) ;;
    http*) url="${args[$i]}" ;;
  esac
  i=$((i+1))
done

status=""
content_file=""
# Defaults established BEFORE the case statement below -- a matching
# case arm may overwrite $headers (e.g. a rate-limit fixture writing
# x-ratelimit-remaining) or write $body directly; doing this after the
# case would truncate whatever the matched arm just wrote.
[[ -n "$headers" ]] && : > "$headers"
[[ -n "$body" ]] && : > "$body"
case "$url" in
%(cases)s
  *) status=500 ;;
esac

# Binary/file-backed fixtures (release downloads) populate content_file;
# JSON-body fixtures already wrote directly to "$body" inside the case
# arm above.
if [[ -n "$content_file" && -n "$body" ]]; then
  cp "$content_file" "$body"
fi

if [[ "$want_status" -eq 1 ]]; then
  echo -n "$status"
  exit 0
fi

if [[ "$status" == "200" ]]; then
  exit 0
else
  exit 22
fi
"""


def build_fixture_release(fixture_dir: Path, tag: str, install_stub_behavior: str = "exit 0") -> None:
    """Builds a real, checksummed fixture release archive (matching
    build-release.sh's own SHA256SUMS convention: filenames listed as
    bare basenames) containing a host/internal/install-steam-shortcut.sh
    stub, so install-branch.sh's real tar+sha256sum+extract logic runs
    unmodified against real bytes."""
    pkg_name = f"melonds-remote-{tag}"
    pkg_dir = fixture_dir / pkg_name
    (pkg_dir / "host" / "internal").mkdir(parents=True)
    (pkg_dir / "client" / "internal").mkdir(parents=True)
    write_exec(pkg_dir / "host" / "internal" / "install-steam-shortcut.sh",
               f"#!/usr/bin/env bash\necho \"fixture install-steam-shortcut.sh --force: $*\" >&2\n{install_stub_behavior}\n")
    write_exec(pkg_dir / "client" / "internal" / "install-steam-shortcut.sh",
               f"#!/usr/bin/env bash\necho \"fixture install-steam-shortcut.sh --force: $*\" >&2\n{install_stub_behavior}\n")

    archive_path = fixture_dir / "melonds-remote-linux-x86_64.tar.gz"
    with tarfile.open(archive_path, "w:gz") as tf:
        tf.add(pkg_dir, arcname=pkg_name)

    digest = hashlib.sha256(archive_path.read_bytes()).hexdigest()
    (fixture_dir / "SHA256SUMS").write_text(f"{digest}  melonds-remote-linux-x86_64.tar.gz\n")


def build_fake_curl(bin_dir: Path, fixtures: dict) -> None:
    """`fixtures` maps an exact URL (or the branches/releases pagination
    URLs, matched by suffix glob) to either a JSON string body, "404",
    "403-ratelimited", or a local file path (for binary downloads)."""
    cases = []
    for url_pattern, response in fixtures.items():
        if response == "404":
            cases.append(f'  {url_pattern}) status=404 ;;')
        elif response == "403-ratelimited":
            cases.append(f'  {url_pattern}) status=403; echo "x-ratelimit-remaining: 0" > "${{headers:-/dev/null}}" ;;')
        elif isinstance(response, Path):
            cases.append(f'  {url_pattern}) status=200; content_file="{response}" ;;')
        else:
            escaped = response.replace("'", "'\\''")
            cases.append(f"  {url_pattern}) status=200; printf '%s' '{escaped}' > \"${{body:-/dev/null}}\" ;;")
    script = FAKE_CURL_TEMPLATE % {"cases": "\n".join(cases)}
    write_exec(bin_dir / "curl", script)


def default_fixtures(release_archive: Path | None = None, release_sums: Path | None = None) -> dict:
    branches_page1 = (
        '[{"name":"main","commit":{"sha":"aaa111deadbeefaaa111deadbeefaaa111deadbe"}},'
        '{"name":"feature-x","commit":{"sha":"bbb222bbb222bbb222bbb222bbb222bbb222bbb2"}},'
        '{"name":"no-release-branch","commit":{"sha":"ccc333ccc333ccc333ccc333ccc333ccc333ccc3"}}]'
    )
    releases_page1 = (
        '[{"tag_name":"v0.1.50","target_commitish":"aaa111deadbeefaaa111deadbeefaaa111deadbe"},'
        '{"tag_name":"v0.1.49","target_commitish":"zzz999zzz999zzz999zzz999zzz999zzz999zzz9"}]'
    )
    fixtures = {
        f"*/repos/{REPO}/branches\\?per_page=100\\&page=1)": branches_page1,
        f"*/repos/{REPO}/branches\\?per_page=100\\&page=2)": "[]",
        f"*/repos/{REPO}/branches/main)": '{"name":"main","commit":{"sha":"aaa111deadbeefaaa111deadbeefaaa111deadbe"}}',
        f"*/repos/{REPO}/branches/no-release-branch)": '{"name":"no-release-branch","commit":{"sha":"ccc333ccc333ccc333ccc333ccc333ccc333ccc3"}}',
        f"*/repos/{REPO}/branches/deleted-branch)": "404",
        f"*/repos/{REPO}/branches/rate-limited-branch)": "403-ratelimited",
        f"*/repos/{REPO}/releases\\?per_page=100\\&page=1)": releases_page1,
        f"*/repos/{REPO}/releases\\?per_page=100\\&page=2)": "[]",
        f"*/repos/{REPO})": '{"default_branch":"main"}',
    }
    # Re-key from the bash-case-pattern form (has a trailing `)`) already
    # embedded above -- build_fake_curl expects patterns without it, so
    # patch back.
    fixtures = {k[:-1]: v for k, v in fixtures.items()}
    if release_archive and release_sums:
        fixtures[f"https://github.com/{REPO}/releases/download/v0.1.50/melonds-remote-linux-x86_64.tar.gz"] = release_archive
        fixtures[f"https://github.com/{REPO}/releases/download/v0.1.50/SHA256SUMS"] = release_sums
    return fixtures


def run_bash(script: str, cwd: Path, env: dict, timeout: int = 20) -> subprocess.CompletedProcess:
    return subprocess.run(["bash", "-c", script], cwd=cwd, capture_output=True, text=True, env=env, timeout=timeout)


def run() -> int:
    failures = []

    def check(condition: bool, message: str):
        if condition:
            print(f"[ok] {message}")
        else:
            print(f"[FAIL] {message}")
            failures.append(message)

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        bin_dir = tmp / "bin"
        bin_dir.mkdir()
        cfg_dir = tmp / "cfg"
        build_fake_curl(bin_dir, default_fixtures())
        env = {"PATH": f"{bin_dir}:/usr/bin:/bin", "DUALDECK_BRANCH_CONFIG_DIR": str(cfg_dir)}

        # --- validate() rejects untrusted/malformed branch names ---
        script = f"""
set -euo pipefail
source '{BRANCH_LIB}'
dualdeck_branch_validate 'main' && echo main:OK
dualdeck_branch_validate 'feature/x-1' && echo feature-x-1:OK
dualdeck_branch_validate '' && echo BAD-empty || echo empty:REJECTED
dualdeck_branch_validate '-rf' && echo BAD-dash || echo dash:REJECTED
dualdeck_branch_validate 'a;rm -rf /' && echo BAD-injection || echo injection:REJECTED
dualdeck_branch_validate '../etc/passwd' && echo BAD-traversal || echo traversal:REJECTED
dualdeck_branch_validate "$(printf 'a%.0s' {{1..300}})" && echo BAD-long || echo long:REJECTED
"""
        proc = run_bash(script, tmp, env)
        out = proc.stdout
        check(proc.returncode == 0, "validate: script itself ran cleanly")
        for expect in ("main:OK", "feature-x-1:OK", "empty:REJECTED", "dash:REJECTED",
                       "injection:REJECTED", "traversal:REJECTED", "long:REJECTED"):
            check(expect in out, f"validate: {expect}")

        # --- paginated discovery + cache ---
        script = f"""
set -euo pipefail
source '{BRANCH_LIB}'
dualdeck_branch_list
"""
        proc = run_bash(script, tmp, env)
        check(proc.returncode == 0, "list: succeeded")
        for name in ("main", "feature-x", "no-release-branch"):
            check(name in proc.stdout, f"list: discovered branch '{name}' via paginated GET")
        check((cfg_dir / "branch-cache.list").exists(), "list: cache file written")

        # --- resolve: success ---
        script = f"""
set -euo pipefail
source '{BRANCH_LIB}'
dualdeck_branch_resolve main
"""
        proc = run_bash(script, tmp, env)
        check(proc.returncode == 0, "resolve(main): succeeded")
        check(proc.stdout.strip() == "aaa111deadbeefaaa111deadbeefaaa111deadbe\tv0.1.50",
              "resolve(main): returned the exact commit+tag of the release built at its tip")

        # --- resolve: never falls back to main on failure ---
        for branch, expected_rc, label in (
            ("no-release-branch", 4, "no release published at this commit"),
            ("deleted-branch", 3, "branch not found (404)"),
            ("rate-limited-branch", 2, "GitHub rate limit"),
            ("not a valid branch;", 5, "invalid branch name"),
        ):
            script = f"""
set -euo pipefail
source '{BRANCH_LIB}'
set +e
dualdeck_branch_resolve '{branch}'
rc=$?
set -e
echo "RC=$rc"
"""
            proc = run_bash(script, tmp, env)
            check(f"RC={expected_rc}" in proc.stdout, f"resolve('{branch}'): {label} -> rc={expected_rc}, never substitutes main")

        offline_env = dict(env, DUALDECK_FAKE_CURL_OFFLINE="1")
        offline_cfg = tmp / "cfg-offline"
        offline_env["DUALDECK_BRANCH_CONFIG_DIR"] = str(offline_cfg)
        script = f"""
set -euo pipefail
source '{BRANCH_LIB}'
set +e
dualdeck_branch_resolve main
rc=$?
set -e
echo "RC=$rc"
"""
        proc = run_bash(script, tmp, offline_env)
        check("RC=1" in proc.stdout, "resolve(main) while offline: rc=1, never substitutes main")
        check(not offline_cfg.exists() or not any(offline_cfg.iterdir()),
              "offline resolve: no cache/state file created from a failed call")

        # --- selecting a branch never installs / never touches branch.conf ---
        select_cfg = tmp / "cfg-select"
        select_env = dict(env, DUALDECK_BRANCH_CONFIG_DIR=str(select_cfg))
        script = f"""
set -euo pipefail
source '{BRANCH_LIB}'
dualdeck_branch_set_selected feature-x
dualdeck_branch_status_line
"""
        proc = run_bash(script, tmp, select_env)
        check(proc.returncode == 0, "set_selected: succeeded")
        check("feature-x" in proc.stdout, "set_selected: status line reflects the new selection")
        check(not (select_cfg / "branch.conf").exists(),
              "set_selected: branch.conf (the *installed* record) was NOT written just by selecting")
        check((select_cfg / "branch.conf.selected").exists(),
              "set_selected: only the separate 'selected' file was written")

        # --- existing-user default: no branch.conf yet -> shows default_branch, writes nothing ---
        fresh_cfg = tmp / "cfg-fresh"
        fresh_env = dict(env, DUALDECK_BRANCH_CONFIG_DIR=str(fresh_cfg))
        script = f"""
set -euo pipefail
source '{BRANCH_LIB}'
dualdeck_branch_status_line
"""
        proc = run_bash(script, tmp, fresh_env)
        check("main" in proc.stdout, "status_line with no prior install: labels it with the repo's default branch")
        check(not (fresh_cfg / "branch.conf").exists(),
              "status_line with no prior install: does not create branch.conf (no implied reinstall)")

    # === Full menu-level test: Advanced submenu install, success + failure paths ===
    src = BUILD_RELEASE.read_text()

    def build_host_dir(root: Path, fixtures: dict, install_behavior: str = "exit 0") -> tuple[Path, Path]:
        host_dir = root / "host"
        internal_dir = host_dir / "internal"
        internal_dir.mkdir(parents=True)
        write_exec(host_dir / "dualdeck-host.sh",
                   extract_heredoc(src, "cat > \"${pkg_dir}/host/dualdeck-host.sh\" <<'WRAP'"))
        write_exec(internal_dir / "install-branch.sh",
                   extract_heredoc(src, "cat > \"${pkg_dir}/host/internal/install-branch.sh\" <<'WRAP'"))
        shutil.copy(BRANCH_LIB, internal_dir / "dualdeck_branch.sh")
        for name in ("apply-update.sh", "install-steam-shortcut.sh", "uninstall-steam-shortcut.sh",
                     "uninstall-host-control-daemon.sh", "reconfigure-cemu-controls.sh",
                     "launch-host.sh", "run-host-azahar.sh", "run-host-cemu.sh",
                     "launch-custom-emulator.sh", "launch-emudeck-integration.sh",
                     "install-host-control-daemon.sh"):
            write_exec(internal_dir / name, "#!/usr/bin/env bash\nexit 0\n")
        write_exec(root / "check-for-updates.sh", "#!/usr/bin/env bash\necho ok\n")
        bin_dir = root / "bin"
        bin_dir.mkdir()
        build_fake_curl(bin_dir, fixtures)
        return host_dir, bin_dir

    def run_menu(host_dir: Path, bin_dir: Path, home_dir: Path, stdin_choices: list[str]):
        env = {"PATH": f"{bin_dir}:/usr/bin:/bin", "HOME": str(home_dir)}
        home_dir.mkdir(parents=True, exist_ok=True)
        return subprocess.run(["./dualdeck-host.sh"], cwd=host_dir,
                               input="\n".join(stdin_choices) + "\n",
                               capture_output=True, text=True, env=env, timeout=20)

    # --- Successful install end-to-end ---
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        fixture_dir = tmp / "fixture"
        fixture_dir.mkdir()
        build_fixture_release(fixture_dir, "v0.1.50")
        fixtures = default_fixtures(release_archive=fixture_dir / "melonds-remote-linux-x86_64.tar.gz",
                                     release_sums=fixture_dir / "SHA256SUMS")
        host_dir, bin_dir = build_host_dir(tmp / "pkg", fixtures)
        home_dir = tmp / "home"
        # 8=Advanced, 1=change branch, 2=select "main" (alphabetical: feature-x, main, no-release-branch), 3=install, y=confirm, 4=back, 9=exit
        proc = run_menu(host_dir, bin_dir, home_dir, ["8", "1", "2", "3", "y", "4", "9"])
        check(proc.returncode == 0, "menu install: host stayed alive through the whole flow, clean exit")
        combined = proc.stdout + proc.stderr
        check("Installed v0.1.50" in combined, "menu install: reported success only after the real work completed")
        branch_conf = home_dir / ".config" / "dualdeck" / "branch.conf"
        check(branch_conf.exists(), "menu install: branch.conf was written")
        conf_text = branch_conf.read_text()
        check("branch=main" in conf_text and "resolved_release_tag=v0.1.50" in conf_text,
              "menu install: branch.conf records the resolved branch/tag")

    # --- Checksum mismatch: nothing is recorded as installed, host stays alive ---
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        fixture_dir = tmp / "fixture"
        fixture_dir.mkdir()
        build_fixture_release(fixture_dir, "v0.1.50")
        # Corrupt the checksum file so verification fails.
        (fixture_dir / "SHA256SUMS").write_text("0" * 64 + "  melonds-remote-linux-x86_64.tar.gz\n")
        fixtures = default_fixtures(release_archive=fixture_dir / "melonds-remote-linux-x86_64.tar.gz",
                                     release_sums=fixture_dir / "SHA256SUMS")
        host_dir, bin_dir = build_host_dir(tmp / "pkg", fixtures)
        home_dir = tmp / "home"
        proc = run_menu(host_dir, bin_dir, home_dir, ["8", "1", "2", "3", "y", "4", "9"])
        check(proc.returncode == 0, "menu install (bad checksum): host stayed alive, clean exit")
        combined = proc.stdout + proc.stderr
        check("Could not install" in combined and "checksum" in combined.lower(),
              "menu install (bad checksum): reported failure, not false success")
        branch_conf = home_dir / ".config" / "dualdeck" / "branch.conf"
        check(not branch_conf.exists(), "menu install (bad checksum): nothing recorded as installed")

    # --- No release published for this branch's commit: refuses, no fallback to main ---
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        fixtures = default_fixtures()
        host_dir, bin_dir = build_host_dir(tmp / "pkg", fixtures)
        home_dir = tmp / "home"
        # select "no-release-branch" (3rd in the alphabetical list: feature-x, main, no-release-branch)
        proc = run_menu(host_dir, bin_dir, home_dir, ["8", "1", "3", "3", "y", "4", "9"])
        check(proc.returncode == 0, "menu install (no release yet): host stayed alive, clean exit")
        combined = proc.stdout + proc.stderr
        check("Could not install" in combined, "menu install (no release yet): reported failure")
        check("main" not in combined.split("Could not install")[-1].split("\n")[0:3].__str__()
              or "no DualDeck release" in combined,
              "menu install (no release yet): error explains the real reason, not a silent main substitution")
        branch_conf = home_dir / ".config" / "dualdeck" / "branch.conf"
        check(not branch_conf.exists(), "menu install (no release yet): nothing installed")

    if failures:
        print(f"\n{len(failures)} check(s) failed:")
        for f in failures:
            print(f"  - {f}")
        return 1

    print("\nINSTALLATION BRANCH SELECTOR TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(run())
