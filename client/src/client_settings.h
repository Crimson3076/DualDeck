#pragma once

#include <string>

namespace melonds_remote::client {

struct ClientSettings {
    // Preserve the release launcher's existing behavior for users who do
    // not have a settings file yet.
    bool autoUpdateOnLaunch = true;

    // Empty means "use the system default recording device" (SDL3's
    // SDL_AUDIO_DEVICE_DEFAULT_RECORDING) -- the common case, and the
    // safe fallback if a previously-selected named device is
    // unplugged/renamed at next launch (GitHub issue #2: "the client
    // device's built-in microphone as the default when available").
    // Otherwise holds the exact string SDL_GetAudioDeviceName() returned
    // for the device the user picked in Settings.
    std::string micDeviceName;

    // Persisted mute state (issue #2's "mute ... controls"), separate
    // from whether a capture device is even open -- muting doesn't close
    // the device, it just stops sending captured audio to the host, so
    // the level meter still works while muted.
    bool micMuted = false;

    // JPEG quality (1-100) requested for this session's video (protocol
    // v9, HelloPayload::videoQuality), or 0 to defer to the host's own
    // configured default. 80 was a reasonable one-size-fits-all default
    // when introduced for Cemu's much larger, often bandwidth-constrained
    // GamePad surface, but turned out to over-compress DS/3DS, whose
    // small frame sizes have bandwidth to spare -- this lets a user
    // dial it back up (or down further, for a genuinely slow link)
    // per client rather than being stuck with one host-wide value.
    int videoQuality = 0;

    // Real user request, 2026-08-03: "add an option to the client's
    // host control to mirror the screen." Purely a local rendering
    // choice -- the host already sends real video during Host Control
    // mode whenever it has DUALDECK_HOSTCONTROL_MIRROR_SCREEN set (see
    // host/remote-server/src/host_control_adapter.cpp), over the exact
    // same VideoFrame wire path Emulation mode already uses, so no new
    // protocol message is needed here: this just decides whether the
    // client falls through to the normal video-render path while in
    // Host Control mode, or keeps showing its own static placeholder
    // screen (renderHostControlScreen() in main.cpp). Defaults off,
    // matching this project's "opt-in experimental feature" convention
    // (e.g. trackpadExperimentEnabled) -- if the host isn't actually
    // mirroring, turning this on just falls back to showing the built-in
    // test pattern texture instead of real video, which is harmless but
    // not the point, so it's not force-enabled by default.
    bool mirrorHostScreen = false;

    // Real user report, 2026-08-03: Azahar's X/Y face buttons read
    // swapped. Root-caused to Steam Input's default controller template
    // for the DualDeck Client's Steam shortcut assigning A/B and X/Y to
    // the physically wrong positions -- confirmed via the user's own
    // Steam Input action-set screenshot, after the whole DualDeck-owned
    // chain (client SDL mapping, wire protocol, host translation,
    // Azahar's own source, and the bundled SDL3's own Xbox 360 HID
    // driver) was independently re-verified correct at every layer. This
    // happens entirely upstream of DualDeck's own code, in Steam Input's
    // translation layer, so there is nothing to "fix" in Azahar's
    // mapping table specifically without breaking it for every user
    // whose Steam Input config (or lack of one) doesn't have this quirk.
    // A client-side remap the affected user can opt into is the
    // correctly-scoped fix instead: applies uniformly to whatever system
    // is running (this is a controller-reading issue, not an
    // Azahar-specific one), defaults off, and doesn't assume every
    // user's Steam Input template is broken the same way. See
    // buildButtonsFromGamepad()'s swapXY parameter.
    bool swapXYButtons = false;
};

// $HOME/.config/dualdeck-client/settings.conf, or empty if HOME is
// unavailable. An empty path makes loading return defaults and saving fail
// without touching the filesystem.
std::string defaultClientSettingsPath();

ClientSettings loadClientSettings(const std::string& settingsPath);

// Replaces the settings file atomically where the platform permits it.
// Returns false when the directory or file could not be written.
bool saveClientSettings(const std::string& settingsPath, const ClientSettings& settings);

} // namespace melonds_remote::client
