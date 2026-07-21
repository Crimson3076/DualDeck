#pragma once

// Pops a kdialog Yes/No prompt asking whether to approve a pending
// device-approval request (see DeviceApprovalManager), so a human at the
// host can approve/deny with just a mouse/controller click -- no typing on
// either side, and no visible terminal required (SteamOS Desktop Mode/
// Bazzite Gaming Mode has none). This is what lets host-control mode and
// out-of-process adapters (e.g. Azahar), which have no in-process emulator
// window of their own to show a Qt dialog in the way melonDS's own
// integration does, use the exact same zero-typing approval flow melonDS
// already has, instead of requiring a static shared secret.
//
// SteamOS Desktop Mode and Bazzite are both KDE Plasma, where kdialog is
// this project's established choice throughout (see
// scripts/build-release.sh's own kdialog usage in its shell menus) --
// this deliberately doesn't also try GNOME's zenity or any other toolkit,
// matching that same existing convention. If kdialog isn't installed (or
// the process has no display to show it on, e.g. a headless/SSH session),
// the request simply stays pending -- the console ("approve <id>"/
// "deny <id>") path documented in --help keeps working regardless; this
// is purely an additional, optional surface.

#include <string>

namespace melonds_remote::host {

enum class KdialogPromptResult {
    Approved,    // the user clicked Yes
    Denied,      // the user clicked No (or closed the dialog)
    Unavailable, // kdialog isn't installed, or couldn't be launched at all
};

// Blocks the calling thread until the user responds (or kdialog exits) --
// callers must run this on its own thread, never on a thread anything else
// depends on making progress (e.g. NetServer's control-connection thread).
KdialogPromptResult promptDeviceApprovalViaKdialog(const std::string& clientName,
                                                    const std::string& address);

// Exposed separately so the exit-status interpretation -- the only part of
// this that doesn't require a real subprocess/display to exercise -- has
// unit test coverage. promptDeviceApprovalViaKdialog() is a thin wrapper
// around fork()+execlp()+waitpid() calling this with the real exit status.
KdialogPromptResult interpretKdialogExitStatus(bool exitedNormally, int exitCode);

} // namespace melonds_remote::host
