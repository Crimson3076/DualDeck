// DualDeck -- Decky Loader plugin frontend.
//
// Structure follows the official decky-plugin-template (fetched directly
// from SteamDeckHomebrew/decky-plugin-template while writing this) as
// closely as possible, but has NOT been loaded into a real Decky Loader
// instance or compiled against a real @decky/ui package -- there's no
// Decky runtime in this project's sandbox to test against. See this
// plugin's README.md and docs/history.md for exactly what is
// and isn't verified (the host-side management protocol this calls into
// is verified; this frontend/backend glue is not).
import {
  ButtonItem,
  PanelSection,
  PanelSectionRow,
  TextField,
  staticClasses,
} from "@decky/ui";
import { callable, definePlugin, toaster, useQuickAccessVisible } from "@decky/api";
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

interface PendingDevice {
  id: string;
  name: string;
  address: string;
}

// get_local_host's reply: the host service's `status` (see
// host/remote-server/include/host/control_socket.h), or running: false.
interface LocalHost {
  running: boolean;
  error?: string;
  mode?: "emulation" | "host-control";
  overridden?: boolean;
  system?: { id: string; name: string };
  pending?: PendingDevice[];
}

const getLocalHost = callable<[], LocalHost>("get_local_host");

// One entry of `dualdeck-client --discover` (client/src/discovery_json.h).
interface DiscoveredHost {
  address: string;
  name: string;
  system: { id: string; name: string };
  adapter: { id: string; name: string };
}

const discoverHosts = callable<[], { hosts?: DiscoveredHost[]; error?: string }>("discover_hosts");
const answerDevice = callable<[deviceId: string, approve: boolean], string>("answer_device");
const setHostControl = callable<[force: boolean], string>("set_host_control");

// How often the panel re-reads the local host service while it's open, so
// a device asking to connect shows up without pressing anything.
const kLocalHostPollMs = 2000;

// While the panel is closed (in a game, say), the plugin still checks for
// new connection requests this often and raises a Steam toast for each.
const kBackgroundPollMs = 3000;

// Starts the background check; returns a function that stops it. Each
// request is announced once, when it first appears.
function watchForConnectionRequests(): () => void {
  const announced = new Set<string>();
  const timer = setInterval(async () => {
    let host: LocalHost;
    try {
      host = await getLocalHost();
    } catch {
      return; // backend not ready yet; try again next tick
    }
    const pending = host.pending ?? [];
    for (const device of pending) {
      if (announced.has(device.id)) continue;
      announced.add(device.id);
      toaster.toast({
        title: "DualDeck",
        body: `${device.name || "A device"} wants to connect. Open DualDeck to approve or deny.`,
        critical: true,
      });
    }
    // Forget answered requests, so a device that asks again is announced again.
    const stillPending = new Set(pending.map((d) => d.id));
    for (const id of announced) if (!stillPending.has(id)) announced.delete(id);
  }, kBackgroundPollMs);
  return () => clearInterval(timer);
}

// Shown only when this machine is itself running the DualDeck host
// service: approve or deny devices asking to connect, and switch Host
// Control mode, from Big Picture instead of the desktop popup.
function LocalHostSection() {
  const [host, setHost] = useState<LocalHost>({ running: false });
  const [message, setMessage] = useState("");
  const [busy, setBusy] = useState(false);

  const visible = useQuickAccessVisible();

  // A failed read (backend restarting, say) keeps the last state shown;
  // the next poll tries again.
  const refresh = async () => {
    try {
      setHost(await getLocalHost());
    } catch {
      // keep the last state
    }
  };

  useEffect(() => {
    if (!visible) return;
    void refresh();
    const timer = setInterval(refresh, kLocalHostPollMs);
    return () => clearInterval(timer);
  }, [visible]);

  const run = async (action: () => Promise<string>) => {
    setBusy(true);
    try {
      const result = await action();
      setMessage(result === "ok" ? "" : result);
      await refresh();
    } finally {
      setBusy(false);
    }
  };

  if (!host.running) return null;

  const pending = host.pending ?? [];
  const inHostControl = host.mode === "host-control";
  return (
    <PanelSection title="This PC (host)">
      {host.error ? (
        <PanelSectionRow>{`Couldn't read the host service: ${host.error}`}</PanelSectionRow>
      ) : (
        <PanelSectionRow>
          {inHostControl ? "Mode: Host Control" : `Mode: streaming ${host.system?.name || "emulator"}`}
        </PanelSectionRow>
      )}
      {pending.length === 0 ? (
        <PanelSectionRow>No devices waiting to connect.</PanelSectionRow>
      ) : (
        pending.map((device) => (
          <PanelSectionRow key={device.id}>
            <div>{`${device.name || "Unnamed device"} (${device.address}) wants to connect`}</div>
            <ButtonItem layout="below" onClick={() => run(() => answerDevice(device.id, true))} disabled={busy}>
              Approve
            </ButtonItem>
            <ButtonItem layout="below" onClick={() => run(() => answerDevice(device.id, false))} disabled={busy}>
              Deny
            </ButtonItem>
          </PanelSectionRow>
        ))
      )}
      {/* With no emulator attached the service is already in Host Control,
          so the switch is only offered while one is streaming, or to undo it. */}
      {(host.overridden || (!host.error && !inHostControl)) && (
        <PanelSectionRow>
          <ButtonItem layout="below" onClick={() => run(() => setHostControl(!host.overridden))} disabled={busy}>
            {host.overridden ? "Return to the emulator" : "Switch to Host Control"}
          </ButtonItem>
        </PanelSectionRow>
      )}
      {message && <PanelSectionRow>{message}</PanelSectionRow>}
    </PanelSection>
  );
}

function Content() {
  const [host, setHost] = useState("");
  const [port, setPort] = useState("8764");
  const [token, setToken] = useState("");
  const [status, setStatus] = useState<string>("unknown");
  const [busy, setBusy] = useState(false);
  const [found, setFound] = useState<DiscoveredHost[] | null>(null);
  const [scanMessage, setScanMessage] = useState("");

  const onFindHosts = async () => {
    setBusy(true);
    setScanMessage("Looking for hosts...");
    try {
      const result = await discoverHosts();
      setFound(result.hosts ?? []);
      if (result.error) setScanMessage(result.error);
      else if ((result.hosts ?? []).length === 0) setScanMessage("No hosts answered on this network.");
      else setScanMessage("");
    } finally {
      setBusy(false);
    }
  };

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
    <>
      <LocalHostSection />
      <PanelSection title="Remote melonDS">
        <PanelSectionRow>
          <TextField label="Host address" value={host} onChange={(e) => setHost(e.target.value)} />
        </PanelSectionRow>
        <PanelSectionRow>
          <ButtonItem layout="below" onClick={onFindHosts} disabled={busy}>
            Find hosts on this network
          </ButtonItem>
        </PanelSectionRow>
        {scanMessage && <PanelSectionRow>{scanMessage}</PanelSectionRow>}
        {(found ?? []).map((h) => (
          <PanelSectionRow key={h.address}>
            <ButtonItem layout="below" onClick={() => setHost(h.address)} disabled={busy}>
              {`${h.name || h.address} (${h.address})${h.system?.name ? ` · ${h.system.name}` : ""}`}
            </ButtonItem>
          </PanelSectionRow>
        ))}
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
    </>
  );
}

export default definePlugin(() => {
  const stopWatching = watchForConnectionRequests();
  return {
    name: "DualDeck",
    titleView: <div className={staticClasses.Title}>DualDeck</div>,
    content: <Content />,
    icon: <FaGamepad />,
    onDismount() {
      stopWatching();
    },
  };
});
