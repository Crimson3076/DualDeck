// DualDeck -- Decky Loader plugin frontend.
//
// Structure follows the official decky-plugin-template (fetched directly
// from SteamDeckHomebrew/decky-plugin-template while writing this) as
// closely as possible, but has NOT been loaded into a real Decky Loader
// instance or compiled against a real @decky/ui package -- there's no
// Decky runtime in this project's sandbox to test against. See this
// plugin's README.md and docs/known-limitations.md for exactly what is
// and isn't verified (the host-side management protocol this calls into
// is verified; this frontend/backend glue is not).
import {
  ButtonItem,
  PanelSection,
  PanelSectionRow,
  TextField,
  staticClasses,
} from "@decky/ui";
import { callable, definePlugin } from "@decky/api";
import { useEffect, useState } from "react";
import { FaGamepad } from "react-icons/fa";

interface Settings {
  host: string;
  port: number;
  token: string;
}

const getSettings = callable<[], Settings>("get_settings");
const saveSettings = callable<[host: string, port: number, token: string], void>("save_settings");
const getStatus = callable<[], string>("get_status");
const setEnabled = callable<[enable: boolean], string>("set_enabled");

function Content() {
  const [host, setHost] = useState("");
  const [port, setPort] = useState("8764");
  const [token, setToken] = useState("");
  const [status, setStatus] = useState<string>("unknown");
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    getSettings().then((s) => {
      setHost(s.host ?? "");
      setPort(String(s.port ?? 8764));
      setToken(s.token ?? "");
    });
    refreshStatus();
  }, []);

  const refreshStatus = async () => {
    setStatus(await getStatus());
  };

  const onSaveSettings = async () => {
    setBusy(true);
    try {
      await saveSettings(host, parseInt(port, 10) || 8764, token);
      await refreshStatus();
    } finally {
      setBusy(false);
    }
  };

  const onToggle = async (enable: boolean) => {
    setBusy(true);
    try {
      const result = await setEnabled(enable);
      setStatus(result === "ok" ? (enable ? "enabled" : "disabled") : result);
    } finally {
      setBusy(false);
    }
  };

  return (
    <PanelSection title="DualDeck">
      <PanelSectionRow>
        <TextField label="Host address" value={host} onChange={(e) => setHost(e.target.value)} />
      </PanelSectionRow>
      <PanelSectionRow>
        <TextField label="Management port" value={port} onChange={(e) => setPort(e.target.value)} />
      </PanelSectionRow>
      <PanelSectionRow>
        <TextField label="Management token" value={token} onChange={(e) => setToken(e.target.value)} bIsPassword />
      </PanelSectionRow>
      <PanelSectionRow>
        <ButtonItem layout="below" onClick={onSaveSettings} disabled={busy}>
          Save
        </ButtonItem>
      </PanelSectionRow>

      <PanelSectionRow>{`Status: ${status}`}</PanelSectionRow>
      <PanelSectionRow>
        <ButtonItem layout="below" onClick={() => onToggle(true)} disabled={busy}>
          Start streaming
        </ButtonItem>
      </PanelSectionRow>
      <PanelSectionRow>
        <ButtonItem layout="below" onClick={() => onToggle(false)} disabled={busy}>
          Stop streaming
        </ButtonItem>
      </PanelSectionRow>
      <PanelSectionRow>
        <ButtonItem layout="below" onClick={refreshStatus} disabled={busy}>
          Refresh status
        </ButtonItem>
      </PanelSectionRow>
    </PanelSection>
  );
}

export default definePlugin(() => {
  return {
    name: "DualDeck",
    titleView: <div className={staticClasses.Title}>DualDeck</div>,
    content: <Content />,
    icon: <FaGamepad />,
    onDismount() {},
  };
});
