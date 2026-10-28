#!/usr/bin/env bash
# Launches a custom-patched emulator binary the user built themselves
# (via scripts/patch-existing-emulator.sh in the DualDeck repository, or
# manually following its same steps) instead of the bundled melonDS/
# Azahar binaries -- for anyone who already has an emulator set up
# elsewhere and doesn't want a separate DualDeck-managed copy alongside
# it. Normally launched via ../../dualdeck-host.sh's "Launch..."
# menu's "Custom" choice, not directly.
#
# The chosen path and system type are remembered in
# ~/.config/dualdeck/custom-emulator.conf so this only has to be
# configured once; pass --reconfigure to point at a different binary or
# change the system type.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
host_root="$(cd .. && pwd)"

config_dir="${HOME}/.config/melonds-remote"
config_file="${config_dir}/custom-emulator.conf"
mkdir -p "${config_dir}"

have_kdialog() { command -v kdialog >/dev/null 2>&1; }

prompt_path() {
    if have_kdialog; then
        kdialog --title "DualDeck Host" --getopenfilename "${HOME}" 2>/dev/null || true
    else
        read -rp "Path to your patched emulator binary: " path >&2
        echo "${path}"
    fi
}

prompt_type() {
    if have_kdialog; then
        kdialog --title "DualDeck Host" --menu "Which system is this?" \
            ds "Nintendo DS (melonDS-based)" \
            n3ds "Nintendo 3DS (Azahar-based)" \
            wiiu "Nintendo Wii U (Cemu-based)" \
            2>/dev/null || true
    else
        read -rp "Is this a (d)S-based, (3)DS-based, or (w)ii U-based emulator? [d/3/w]: " t >&2
        case "${t}" in
            3) echo "n3ds" ;;
            w) echo "wiiu" ;;
            *) echo "ds" ;;
        esac
    fi
}

if [[ "${1:-}" == "--reconfigure" ]]; then
    rm -f "${config_file}"
    shift
fi

if [[ ! -f "${config_file}" ]]; then
    path="$(prompt_path)"
    if [[ -z "${path}" || ! -x "${path}" ]]; then
        echo "error: no valid, executable binary path given." >&2
        exit 1
    fi
    type="$(prompt_type)"
    if [[ -z "${type}" ]]; then
        echo "error: no system type chosen." >&2
        exit 1
    fi
    {
        echo "type=${type}"
        echo "path=${path}"
    } > "${config_file}"
    chmod 600 "${config_file}"
fi

# shellcheck disable=SC1090
source "${config_file}"

if [[ ! -x "${path}" ]]; then
    echo "error: ${path} no longer exists or isn't executable -- run this again with" >&2
    echo "--reconfigure to point at a different binary." >&2
    exit 1
fi

case "${type}" in
    ds)
        # A custom melonDS-based build understands MELONDS_REMOTE_ENABLE
        # directly (its own patched-in in-process server + interactive
        # device-approval dialog, same as the bundled host/melonDS) --
        # no Host Service to start here.
        export MELONDS_REMOTE_ENABLE=1
        MELONDS_REMOTE_VERSION="$(cat "$(dirname "${host_root}")/VERSION" 2>/dev/null || true)"
        export MELONDS_REMOTE_VERSION
        exec "${path}" "$@"
        ;;
    n3ds)
        if [[ ! -x "${host_root}/internal/dualdeck-host-service" ]]; then
            echo "error: host/internal/dualdeck-host-service is missing from this" >&2
            echo "install -- re-download the release archive." >&2
            exit 1
        fi
        run_dir="${HOME}/.config/dualdeck/run"
        mkdir -p "${run_dir}"
        adapter_socket="${run_dir}/custom-adapter.sock"
        rm -f "${adapter_socket}"

        AZAHAR_REMOTE_VERSION="$(cat "$(dirname "${host_root}")/VERSION" 2>/dev/null || true)"

        export AZAHAR_REMOTE_VERSION
        # No --auth-token by default: an unrecognized device pops the same
        # zero-typing kdialog Yes/No approval prompt melonDS's in-process
        # dialog gives you (see kdialog_approval_prompt.h). ${token:-} only
        # exists for config files written by older releases that generated
        # one automatically.
        auth_token_args=()
        if [[ -n "${token:-}" ]]; then
            auth_token_args=(--auth-token "${token}")
        fi
        # See build-release.sh's bundle_library_dependencies() call for
        # this binary -- host/internal/lib ships its runtime deps
        # (libturbojpeg) so it doesn't depend on the host having it.
        env LD_LIBRARY_PATH="${host_root}/internal/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
            "${host_root}/internal/dualdeck-host-service" --adapter-ipc --adapter-socket "${adapter_socket}" \
            --state-dir "${HOME}/.config/melonds-remote" "${auth_token_args[@]}" \
            --app-version "${AZAHAR_REMOTE_VERSION}" &
        host_service_pid=$!
        trap 'kill "${host_service_pid}" 2>/dev/null || true' EXIT
        sleep 0.5

        export AZAHAR_REMOTE_ENABLE=1
        export AZAHAR_REMOTE_ADAPTER_SOCKET="${adapter_socket}"
        # See internal/run-host-azahar.sh's identical comment: works
        # around a known Qt6-on-Linux native-file-dialog crash by
        # disabling the GTK3 platform theme integration that triggers
        # it (cosmetic-only downside).
        export QT_QPA_PLATFORMTHEME=""
        "${path}" "$@"
        ;;
    wiiu)
        if [[ ! -x "${host_root}/internal/dualdeck-host-service" ]]; then
            echo "error: host/internal/dualdeck-host-service is missing from this" >&2
            echo "install -- re-download the release archive." >&2
            exit 1
        fi
        run_dir="${HOME}/.config/dualdeck/run"
        mkdir -p "${run_dir}"
        adapter_socket="${run_dir}/custom-adapter.sock"
        rm -f "${adapter_socket}"

        CEMU_REMOTE_VERSION="$(cat "$(dirname "${host_root}")/VERSION" 2>/dev/null || true)"

        export CEMU_REMOTE_VERSION
        # Same zero-typing kdialog approval as the n3ds case above --
        # see that case's identical comment.
        auth_token_args=()
        if [[ -n "${token:-}" ]]; then
            auth_token_args=(--auth-token "${token}")
        fi
        # See the n3ds case's identical comment above.
        env LD_LIBRARY_PATH="${host_root}/internal/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
            "${host_root}/internal/dualdeck-host-service" --adapter-ipc --adapter-socket "${adapter_socket}" \
            --state-dir "${HOME}/.config/melonds-remote" "${auth_token_args[@]}" \
            --app-version "${CEMU_REMOTE_VERSION}" &
        host_service_pid=$!
        trap 'kill "${host_service_pid}" 2>/dev/null || true' EXIT
        sleep 0.5

        export CEMU_REMOTE_ENABLE=1
        export CEMU_REMOTE_ADAPTER_SOCKET="${adapter_socket}"
        "${path}" "$@"
        ;;
    *)
        echo "error: unknown system type '${type}' in ${config_file} -- re-run with" >&2
        echo "--reconfigure." >&2
        exit 1
        ;;
esac
