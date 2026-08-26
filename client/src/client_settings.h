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

    // Protocol v13: whether this client advertises VideoCodecBit_H264
    // (in addition to the always-advertised VideoCodecBit_Jpeg) in
    // HelloPayload.supportedVideoCodecs -- see
    // docs/known-limitations.md's 2026-08-25 video-codec-negotiation
    // entries for the full design. Defaults off, same "opt-in
    // experimental feature" convention as mirrorHostScreen above: the
    // decoder is real and tested (client/src/h264_decoder.h), but has
    // not yet been exercised against a real game/network session on
    // real hardware, so a session only ever actually negotiates H.264
    // if a user explicitly turns this on. Even with this on, the
    // session might still end up JPEG anyway -- the host has the final
    // say (NetServer::selectVideoCodec()), e.g. if it wasn't built with
    // OpenH264 either.
    bool videoCodecH264Experimental = false;

    // Real user request, 2026-08-26: "some sort of way to show the user
    // what resolution is being streamed, what fps, codec, etc as a debug
    // overlay for the client." Purely a local rendering choice, like
    // mirrorHostScreen above -- no protocol/negotiation involved, so
    // toggling it takes effect immediately, no reconnect needed (see
    // NetClient's receivedFrameCount()/receivedFps()/hostNativeWidth()/
    // negotiatedVideoCodec() etc., what main.cpp actually renders when
    // this is on). Defaults off: it's screen-space a player may not want
    // permanently spent during normal play.
    bool debugOverlayEnabled = false;
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
