# melonDS Remote -- Decky Loader plugin backend.
#
# Talks to melonDS's management listener (ManagementServer, see
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
# docs/known-limitations.md's Decky plugin section for exactly what was
# tested and how). If this plugin fails to load or behaves oddly in
# Decky itself, that's the untested boundary -- please report it.
import os

import decky

import management_client

_SETTINGS_PATH = os.path.join(decky.DECKY_USER_HOME, ".config", "melonds-remote-decky", "settings.json")


class Plugin:
    def _load_settings(self):
        import json
        try:
            with open(_SETTINGS_PATH) as f:
                return json.load(f)
        except (FileNotFoundError, ValueError):
            return {"host": "", "port": 8764, "token": ""}

    def _save_settings(self, settings):
        import json
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

    # --- Decky lifecycle hooks (see decky-plugin-template's main.py) ---

    async def _main(self):
        decky.logger.info("melonDS Remote plugin loaded")

    async def _unload(self):
        decky.logger.info("melonDS Remote plugin unloaded")

    async def _uninstall(self):
        pass
