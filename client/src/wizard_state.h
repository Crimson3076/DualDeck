#pragma once

// Remembers whether the first-run setup wizard (main.cpp's
// runSetupWizard()) has already completed successfully, so it only runs
// automatically once -- later launches go straight to the normal
// discovery/connect flow, and the wizard is only reachable again by opening
// Settings from the in-app menu (GitHub issue #19).
// Plain marker file: its mere existence means "done", content unused.

#include <string>

namespace melonds_remote::client {

// $HOME/.config/dualdeck-client/setup_complete.txt, or empty if
// $HOME isn't set (persistence then silently unavailable -- the wizard
// runs every launch in that case, same fail-safe default as before this
// existed).
std::string defaultWizardStatePath();

bool isSetupComplete(const std::string& statePath);
void markSetupComplete(const std::string& statePath);

} // namespace melonds_remote::client
