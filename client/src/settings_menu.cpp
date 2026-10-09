#include "settings_menu.h"

#include <algorithm>
#include <cstdio>
#include <utility>

#include "screens.h"

namespace dualdeck::client {

namespace {

// runCaptureStdout <command>
//
// Real user report, 2026-08-02: the trackpad-experiment toggle
// (docs/history.md's entry of the same date) originally only
// lived in dualdeck-client.sh's outer shell menu, which is unreachable
// from Gaming Mode (the Steam shortcut execs run-client.sh directly,
// bypassing that menu). Moved into this Settings screen instead, which
// shells out to configure-trackpad-experiment.sh (in the same directory
// as this binary's CWD -- see below) -- the one place (shared with
// dualdeck-client.sh's own menu) that actually knows how to check/
// toggle it -- rather than reimplementing that logic
// (Steam-restart-on-conflict safety, localconfig.vdf editing) in C++.
// Relies on this binary always being launched with CWD ==
// .../internal/ (true whenever launched via run-client.sh, which `cd`s
// into internal/ and execs the binary without ever cd'ing back out --
// see run-client.sh's own comment; this project has no existing
// executable-path-resolution convention to fall back on for a
// different launch method), so a bare "./configure-trackpad-
// experiment.sh" -- NOT "./internal/configure-trackpad-experiment.sh"
// -- is the correct relative path from here; real user report,
// 2026-08-02, the "internal/" prefix silently pointed at a
// nonexistent ".../internal/internal/..." path, so the toggle did
// nothing at all.
//
// Blocking (popen() waits for the child to exit) -- acceptable here
// since this only ever runs in direct response to a menu selection
// (never per-frame; see SettingsMenu::trackpadExperimentEnabled_ on why the
// *status* query is cached, not re-run every frame). Toggle calls below
// always pass --no-restart: real user report, 2026-08-02, triggering
// configure-trackpad-experiment.sh's normal Steam-restart handoff from
// in here (this client is normally itself a Steam-launched process in
// Gaming Mode) "seemingly crashe[d] Steam and restart[ed] it" -- killing
// Steam out from under the game it's actively running it, rather than
// something a standalone Desktop Mode menu action does before anything
// is even launched. See that script's own comment on --no-restart.
// Returns the child's stdout with trailing newlines stripped, or an
// empty string if the command couldn't even be started (matching this
// codebase's "degrade gracefully, log once, never crash" convention for
// an unavailable optional resource elsewhere, e.g.
// HostControlAdapter::isDeviceReady()).
std::string runCaptureStdout(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return "";
    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

// Labels ClientSettings::videoQuality as a handful of named presets
// rather than a raw number -- this text UI has no slider widget.
std::string videoQualityLabel(int quality) {
    if (quality == 0) return "AUTO";
    if (quality <= 40) return "LOW (SLOWEST LINKS)";
    if (quality <= 65) return "MEDIUM";
    if (quality <= 85) return "HIGH";
    return "MAXIMUM (LARGEST)";
}

bool startsWith(const std::string& text, const char* prefix) { return text.rfind(prefix, 0) == 0; }

} // namespace

SettingsMenu::SettingsMenu(ClientSettings& settings, std::string settingsPath, bool offerSetupWizard)
    : settings_(settings), settingsPath_(std::move(settingsPath)), offerSetupWizard_(offerSetupWizard) {}

void SettingsMenu::open() {
    selectedIndex_ = 0;
    // A change made from the host picker, before any connection, needs
    // no reconnect; don't let it carry over into the next session's menu.
    reconnectRequested_ = false;
    refreshTrackpadExperimentStatus();
}

std::vector<std::string> SettingsMenu::items(bool showMic) const {
    std::vector<std::string> items{
        std::string("AUTO UPDATE ON LAUNCH: ") + (settings_.autoUpdateOnLaunch ? "ON" : "OFF"),
        std::string("VIDEO QUALITY: ") + videoQualityLabel(settings_.videoQuality),
        std::string("TRACKPAD AS NATIVE INPUT (EXPERIMENTAL): ") + (trackpadExperimentEnabled_ ? "ON" : "OFF"),
        std::string("MIRROR HOST SCREEN (EXPERIMENTAL): ") + (settings_.mirrorHostScreen ? "ON" : "OFF"),
        std::string("VIDEO CODEC (EXPERIMENTAL): ") +
            (settings_.videoCodecPyroWaveExperimental ? "PYROWAVE"
             : settings_.videoCodecH264Experimental  ? "H264"
                                                     : "JPEG"),
        std::string("DEBUG OVERLAY: ") + (settings_.debugOverlayEnabled ? "ON" : "OFF"),
    };
    if (offerSetupWizard_) items.push_back("RUN SETUP WIZARD");
    if (showMic) {
        std::string micLabel = settings_.micDeviceName.empty() ? "SYSTEM DEFAULT" : settings_.micDeviceName;
        items.push_back(std::string("MICROPHONE: ") + micLabel);
        items.push_back(std::string("MIC: ") + (settings_.micMuted ? "MUTED" : "ON"));
    }
    items.push_back("BACK");
    return items;
}

SettingsMenu::Result SettingsMenu::handle(MenuAction action, bool showMic, MicCapture* micCapture) {
    if (action == MenuAction::None) return Result::Stay;
    if (action == MenuAction::Back) return Result::Close;

    const std::vector<std::string> rows = items(showMic);
    const int count = static_cast<int>(rows.size());
    // The list can shrink while open (the mic rows go away if the host
    // drops), so keep the selection in range.
    selectedIndex_ = std::clamp(selectedIndex_, 0, count - 1);
    if (action == MenuAction::Up) {
        selectedIndex_ = (selectedIndex_ + count - 1) % count;
        return Result::Stay;
    }
    if (action == MenuAction::Down) {
        selectedIndex_ = (selectedIndex_ + 1) % count;
        return Result::Stay;
    }

    // D-pad right/left step a value forward/back; A steps it forward.
    const int direction = action == MenuAction::Left ? -1 : action == MenuAction::Right ? 1 : 0;
    const int step = direction == 0 ? 1 : direction;
    const std::string& picked = rows[static_cast<size_t>(selectedIndex_)];
    if (startsWith(picked, "AUTO UPDATE ON LAUNCH:")) {
        settings_.autoUpdateOnLaunch = !settings_.autoUpdateOnLaunch;
        save();
    } else if (startsWith(picked, "VIDEO QUALITY:")) {
        cycleVideoQuality(step);
    } else if (startsWith(picked, "TRACKPAD AS NATIVE INPUT")) {
        toggleTrackpadExperiment();
    } else if (startsWith(picked, "MIRROR HOST SCREEN")) {
        settings_.mirrorHostScreen = !settings_.mirrorHostScreen;
        save();
    } else if (startsWith(picked, "VIDEO CODEC")) {
        cycleVideoCodec(step);
    } else if (startsWith(picked, "DEBUG OVERLAY:")) {
        settings_.debugOverlayEnabled = !settings_.debugOverlayEnabled;
        save();
    } else if (startsWith(picked, "MICROPHONE:")) {
        cycleMicDevice(step, micCapture);
    } else if (startsWith(picked, "MIC:")) {
        settings_.micMuted = !settings_.micMuted;
        save();
    } else if (direction == 0 && picked == "RUN SETUP WIZARD") {
        return Result::RunSetupWizard;
    } else if (direction == 0 && picked == "BACK") {
        return Result::Close;
    }
    return Result::Stay;
}

void SettingsMenu::render(SDL_Renderer* renderer, bool showMic, float micLevel) const {
    renderPauseMenu(renderer, items(showMic), selectedIndex_, "SETTINGS",
                    saveFailed_ ? "COULD NOT SAVE SETTINGS" : "", micLevel, "",
                    {{"D-PAD", "MOVE"}, {"LEFT/RIGHT", "CHANGE"}, {"A", "SELECT"}, {"B", "BACK"}});
}

bool SettingsMenu::takeReconnectRequest() { return std::exchange(reconnectRequested_, false); }

void SettingsMenu::save() { saveFailed_ = !saveClientSettings(settingsPath_, settings_); }

// Cycles through a fixed set of presets (see videoQualityLabel()),
// wrapping back to AUTO. Only takes effect on the next connection --
// there's no packet type for changing an already-connected session's
// compression quality -- so this asks for a reconnect, which main.cpp
// does once Settings closes rather than after every tap (see
// reconnectRequested's comment there for the user report behind it).
void SettingsMenu::cycleVideoQuality(int direction) {
    static constexpr int kPresets[] = {0, 40, 65, 85, 100};
    constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));
    int currentIndex = 0;
    for (int i = 0; i < kPresetCount; ++i) {
        if (kPresets[i] == settings_.videoQuality) {
            currentIndex = i;
            break;
        }
    }
    settings_.videoQuality = kPresets[(currentIndex + kPresetCount + direction) % kPresetCount];
    save();
    reconnectRequested_ = true;
}

// Cycles JPEG -> H264 -> PYROWAVE -> JPEG across
// ClientSettings::videoCodecH264Experimental/videoCodecPyroWaveExperimental
// (see the latter's comment for why it's two flags). Negotiated once, in
// Hello, so it needs a reconnect the same as cycleVideoQuality().
void SettingsMenu::cycleVideoCodec(int direction) {
    // 0 = JPEG, 1 = H264, 2 = PYROWAVE.
    int current = settings_.videoCodecPyroWaveExperimental ? 2 : settings_.videoCodecH264Experimental ? 1 : 0;
    int next = (current + 3 + direction) % 3;
    settings_.videoCodecH264Experimental = next == 1;
    settings_.videoCodecPyroWaveExperimental = next == 2;
    save();
    reconnectRequested_ = true;
}

// Moves to the next enumerated recording device (wrapping back to
// SYSTEM DEFAULT), saves, and reopens capture on it if it's open.
void SettingsMenu::cycleMicDevice(int direction, MicCapture* micCapture) {
    auto devices = listMicDevices();
    if (devices.empty()) return;
    size_t currentIndex = 0;
    for (size_t i = 0; i < devices.size(); ++i) {
        if (devices[i].name == settings_.micDeviceName) {
            currentIndex = i;
            break;
        }
    }
    size_t nextIndex =
        (direction < 0 ? currentIndex + devices.size() - 1 : currentIndex + 1) % devices.size();
    // SYSTEM DEFAULT is stored as an empty name (see
    // ClientSettings::micDeviceName's comment), never the literal label --
    // so a later SDL enumeration change can't strand a saved setting that
    // no longer matches anything.
    settings_.micDeviceName =
        devices[nextIndex].id == SDL_AUDIO_DEVICE_DEFAULT_RECORDING ? "" : devices[nextIndex].name;
    save();
    if (micCapture) micCapture->open(settings_.micDeviceName);
}

void SettingsMenu::refreshTrackpadExperimentStatus() {
    // NOT "./internal/configure-trackpad-experiment.sh" -- see
    // runCaptureStdout's comment on why this binary's CWD is already
    // .../internal/, so that extra prefix silently pointed at a
    // nonexistent path (real user report, 2026-08-02: the toggle appeared
    // to do nothing at all).
    trackpadExperimentEnabled_ =
        runCaptureStdout("./configure-trackpad-experiment.sh --status 2>/dev/null") == "disabled";
}

void SettingsMenu::toggleTrackpadExperiment() {
    // --no-restart: see runCaptureStdout's comment -- never let this
    // trigger Steam restarting itself while this client is running as a
    // live Steam-launched process.
    runCaptureStdout(trackpadExperimentEnabled_ ? "./configure-trackpad-experiment.sh --remove --no-restart 2>&1"
                                                : "./configure-trackpad-experiment.sh --no-restart 2>&1");
    // Re-queried rather than just flipping the cached bool: checking
    // what's actually on disk is cheap and more honest than assuming.
    refreshTrackpadExperimentStatus();
}

} // namespace dualdeck::client
