# DualDeck -- Decky Loader plugin backend.
#
# On the host PC, talks to dualdeck-host-service's local control socket
# (host_control_client.py) to approve devices and switch Host Control.
#
# On the Deck, talks to melonDS's management listener (ManagementServer, see
# host/melonds-patches/README.md and management_client.py in this
# directory) to start/stop remote streaming on an already-running
# melonDS on your HTPC, without walking over to it or restarting it.
#
# IMPORTANT: this file follows the structure of the official Decky
# plugin template (SteamDeckHomebrew/decky-plugin-template, fetched
# directly while writing this) as closely as possible, but it has NOT
# been loaded into a real Decky Loader instance -- there is no Decky
# runtime available to test against here. The one part that *is*
# independently verified against a real, patched, running melonDS is
# management_client.py (see its own file and
# docs/history.md's Decky plugin section for exactly what was
# tested and how). If this plugin fails to load or behaves oddly in
# Decky itself, that's the untested boundary -- please report it.
import asyncio
import json
import os
import pwd

import decky

import host_control_client
import management_client

_SETTINGS_PATH = os.path.join(decky.DECKY_USER_HOME, ".config", "dualdeck-decky", "settings.json")
_LEGACY_SETTINGS_PATH = os.path.join(decky.DECKY_USER_HOME, ".config", "melonds-remote-decky", "settings.json")


class Plugin:
    def _load_settings(self):
        # One-time melonDS-Remote -> DualDeck rebrand migration: copy an
        # old settings file forward if the new one doesn't exist yet,
        # same as the client binary's config-dir migration. Never
        # deletes the old file.
        if not os.path.isfile(_SETTINGS_PATH) and os.path.isfile(_LEGACY_SETTINGS_PATH):
            try:
                os.makedirs(os.path.dirname(_SETTINGS_PATH), exist_ok=True)
                with open(_LEGACY_SETTINGS_PATH) as legacy_f:
                    legacy_settings = legacy_f.read()
                with open(_SETTINGS_PATH, "w") as new_f:
                    new_f.write(legacy_settings)
            except OSError:
                pass  # best-effort -- _load_settings below still has a safe default
        try:
            with open(_SETTINGS_PATH) as f:
                return json.load(f)
        except (FileNotFoundError, ValueError):
            return {"host": "", "port": 8764, "token": ""}

    def _save_settings(self, settings):
        os.makedirs(os.path.dirname(_SETTINGS_PATH), exist_ok=True)
        with open(_SETTINGS_PATH, "w") as f:
            json.dump(settings, f)

    # --- Called from the frontend (src/index.tsx) via @decky/api's callable() ---

    async def get_settings(self) -> dict:
        return self._load_settings()

    async def save_settings(self, host: str, port: int, token: str) -> None:
        self._save_settings({"host": host, "port": port, "token": token})

    async def get_status(self) -> str:
        """Returns "enabled", "disabled", or "error: <message>"."""
        settings = self._load_settings()
        if not settings.get("host") or not settings.get("token"):
            return "error: not configured -- set host/port/token first"
        try:
            running = management_client.status(settings["host"], int(settings["port"]), settings["token"])
            return "enabled" if running else "disabled"
        except Exception as exc:  # noqa: BLE001 -- surfaced to the UI as plain text either way
            return f"error: {exc}"

    async def set_enabled(self, enable: bool) -> str:
        """Returns "ok", or "error: <message>"."""
        settings = self._load_settings()
        if not settings.get("host") or not settings.get("token"):
            return "error: not configured -- set host/port/token first"
        try:
            fn = management_client.enable if enable else management_client.disable
            ok = fn(settings["host"], int(settings["port"]), settings["token"])
            return "ok" if ok else "error: host reported failure"
        except Exception as exc:  # noqa: BLE001
            return f"error: {exc}"

    # --- This PC as a host: dualdeck-host-service's local control socket ---

    def _host_socket(self):
        try:
            uid = pwd.getpwnam(decky.DECKY_USER).pw_uid
        except (KeyError, AttributeError):
            uid = os.getuid()
        candidates = host_control_client.socket_path_candidates(home=decky.DECKY_USER_HOME, uid=uid)
        return host_control_client.find_socket(candidates)

    async def get_local_host(self) -> dict:
        """The host service running on this machine, if any:
        {"running": False} when there is none, otherwise its status reply
        (mode, overridden, system, adapter, pending) plus "running": True,
        or {"running": True, "error": "..."} if it couldn't be read."""
        path = self._host_socket()
        if path is None:
            return {"running": False}
        try:
            reply = host_control_client.status(path)
        except Exception as exc:  # noqa: BLE001 -- surfaced to the UI as plain text
            return {"running": True, "error": str(exc)}
        reply["running"] = True
        return reply

    async def answer_device(self, device_id: str, approve: bool) -> str:
        """Approves or denies a pending connection request. Returns "ok",
        or "error: <message>"."""
        return self._local_host_command(host_control_client.answer_device, device_id, approve)

    async def set_host_control(self, force: bool) -> str:
        """Forces Host Control mode on (True) or hands the choice back to
        the running emulator (False). Returns "ok" or "error: <message>"."""
        return self._local_host_command(host_control_client.set_host_control, force)

    def _local_host_command(self, fn, *args):
        path = self._host_socket()
        if path is None:
            return "error: the DualDeck host service isn't running on this PC"
        try:
            fn(path, *args)
            return "ok"
        except Exception as exc:  # noqa: BLE001
            return f"error: {exc}"

    # --- LAN discovery, through the installed client's `--discover` ---

    def _client_root(self):
        # Where the client menu's "Add to Steam" installs it (see
        # packaging/client/internal/run-client.sh).
        return os.path.join(decky.DECKY_USER_HOME, ".config", "dualdeck-client", "install")

    async def discover_hosts(self) -> dict:
        """Runs one LAN scan with `dualdeck-client --discover`:
        {"hosts": [...]} (see client/src/discovery_json.h), or
        {"error": "..."} if the client isn't installed or the scan failed."""
        root = self._client_root()
        binary = os.path.join(root, "dualdeck-client")
        if not os.access(binary, os.X_OK):
            return {"error": "the DualDeck client isn't installed on this device"}
        env = dict(os.environ)
        lib_dir = os.path.join(root, "lib")
        env["LD_LIBRARY_PATH"] = lib_dir + (":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
        try:
            proc = await asyncio.create_subprocess_exec(
                binary, "--discover",
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL, env=env)
            stdout, _ = await asyncio.wait_for(proc.communicate(), timeout=10)
        except asyncio.TimeoutError:
            proc.kill()
            return {"error": "the scan didn't finish"}
        except OSError as exc:
            return {"error": str(exc)}
        if proc.returncode != 0:
            return {"error": f"the scan failed (exit {proc.returncode})"}
        try:
            return {"hosts": json.loads(stdout.decode("utf-8"))}
        except ValueError:
            return {"error": "the client printed something unexpected"}

    # --- Decky lifecycle hooks (see decky-plugin-template's main.py) ---

    async def _main(self):
        decky.logger.info("DualDeck plugin loaded")

    async def _unload(self):
        decky.logger.info("DualDeck plugin unloaded")

    async def _uninstall(self):
        pass
