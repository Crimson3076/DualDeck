#pragma once

// The client's Settings screen: its rows, what D-pad/A/B do on them, and
// saving each change. Shared by the in-session pause menu (main.cpp) and
// the host picker's menu, so settings can be changed before connecting.

#include <SDL3/SDL.h>

#include <string>
#include <vector>

#include "client_settings.h"
#include "gamepad_input.h"
#include "mic_capture.h"

namespace dualdeck::client {

class SettingsMenu {
public:
    // offerSetupWizard adds a RUN SETUP WIZARD row (hidden for an explicit
    // --host, which has no picker for the wizard to fall back to).
    SettingsMenu(ClientSettings& settings, std::string settingsPath, bool offerSetupWizard);

    enum class Result { Stay, Close, RunSetupWizard };

    // Call each time the screen opens: back to the top row, any pending
    // reconnect request dropped, and the trackpad experiment's on-disk
    // state re-read.
    void open();

    // showMic adds the MICROPHONE and MIC rows. micCapture, when given, is
    // reopened on the newly picked device.
    std::vector<std::string> items(bool showMic) const;
    Result handle(MenuAction action, bool showMic, MicCapture* micCapture);
    // micLevel < 0 hides the level meter.
    void render(SDL_Renderer* renderer, bool showMic, float micLevel) const;

    // True once after a change that only applies on the next connection
    // (video quality, codec); reading it clears it.
    bool takeReconnectRequest();

private:
    void save();
    void cycleVideoQuality(int direction);
    void cycleStreamFps(int direction);
    void cycleVideoCodec(int direction);
    void cycleMicDevice(int direction, MicCapture* micCapture);
    void refreshTrackpadExperimentStatus();
    void toggleTrackpadExperiment();

    ClientSettings& settings_;
    std::string settingsPath_;
    bool offerSetupWizard_;
    int selectedIndex_ = 0;
    bool saveFailed_ = false;
    bool reconnectRequested_ = false;
    // Cached rather than queried per frame: reading it shells out to a
    // script (see refreshTrackpadExperimentStatus()).
    bool trackpadExperimentEnabled_ = false;
};

} // namespace dualdeck::client
