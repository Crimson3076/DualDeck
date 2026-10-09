#include "host_picker.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "address_entry.h"
#include "gamepad_input.h"
#include "screens.h"

namespace dualdeck::client {

namespace {

// Each discoverHosts() call blocks for this long. discoverAndSelectHost()
// runs it on a background thread, repeatedly, for as long as the picker
// screen is shown -- not a total search timeout, just how often the
// on-screen host list refreshes. Originally run inline on the render/
// input thread itself, which meant SDL_PollEvent() (and therefore every
// button press, not just DPAD/SOUTH on the picker) went unserviced for
// this entire duration on every single scan -- reported as buttons
// "sometimes doing nothing" (GitHub issue #21, reopened after the first
// pass only added a "connecting" screen for the *post-selection* wait,
// not this pre-selection one).
constexpr int kDiscoveryScanMs = 1200;
// discoverAndSelectHost()'s own loop no longer blocks on network I/O (see
// kDiscoveryScanMs's comment), so without an explicit cap it would poll
// events and re-render as fast as the platform allows -- measured at a
// full CPU core pinned to 100% under Xvfb software rendering, since
// nothing else paces it. ~60Hz is plenty for a picker/menu screen with
// no low-latency requirement of its own.
constexpr int kPickerFrameIntervalMs = 16;

} // namespace

// Runs on every launch (spec request: "each time the client is booted, it
// should show this screen in the event I want to connect to another
// client"): scans the LAN for DualDeck hosts and always lets the
// user pick one via gamepad D-pad/South or keyboard arrows/Enter -- never
// auto-connects silently, even when only one host answers, so switching
// to a different HTPC is always available, not just when there happens
// to be more than one. The previously-picked host (see discovery_store.h)
// is pre-highlighted as the default selection for a quick one-button
// reconnect, but the user can always navigate to a different one instead.
//
// Keeps rescanning while the list is shown (not just once up front), so
// a host that finishes booting a few seconds late still shows up without
// restarting the client. Selection is preserved across rescans by
// address, and the list is sorted for a stable display order (discovery
// itself doesn't guarantee reply order is consistent scan to scan).
//
// The scan itself runs on its own background thread (GitHub issue #21,
// reopened) -- discoverHosts() blocks for kDiscoveryScanMs, and running
// that inline on this function's own loop, as a first pass of this fix
// did, meant SDL_PollEvent() (and every button press with it) went
// unserviced for the whole 1.2s of every single rescan, not just the
// initial one, making the picker feel randomly unresponsive depending on
// exactly when a press landed relative to the blocking call. This
// thread does nothing but scan-and-publish; the loop below always polls
// events and renders every frame regardless of scan timing.
//
// Returns std::nullopt if the user closed the window before a host was
// chosen (SDL_EVENT_QUIT), or chose EXIT from the L3+R3 menu below;
// main() treats either as "cancel the whole run", not "connect anyway."
// Y (or the menu) opens address entry for a host discovery can't see.
std::optional<DiscoveredHost> discoverAndSelectHost(SDL_Renderer* renderer, SDL_Gamepad*& gamepad,
                                                     uint16_t discoveryPort,
                                                     const std::string& lastHostAddress,
                                                     const std::string& clientVersion, bool* backRequested,
                                                     SettingsMenu* settings, bool* setupWizardRequested) {
    std::vector<DiscoveredHost> hosts;
    int selectedIndex = 0;

    std::mutex scanMutex;
    std::vector<DiscoveredHost> latestScan;
    std::atomic<bool> scanStop{false};
    std::thread scanThread([&]() {
        while (!scanStop.load()) {
            std::vector<DiscoveredHost> fresh = discoverHosts(discoveryPort, kDiscoveryScanMs, &scanStop);
            std::sort(fresh.begin(), fresh.end(),
                      [](const DiscoveredHost& a, const DiscoveredHost& b) { return a.address < b.address; });
            std::lock_guard<std::mutex> lock(scanMutex);
            latestScan = std::move(fresh);
        }
    });
    // Guarantees scanThread is stopped and joined on every return path
    // below (including SDL_EVENT_QUIT and the menu's EXIT) without
    // needing a matching stop+join before each one individually.
    struct ScanThreadGuard {
        std::atomic<bool>& stop;
        std::thread& thread;
        ~ScanThreadGuard() {
            stop = true;
            if (thread.joinable()) thread.join();
        }
    } scanThreadGuard{scanStop, scanThread};

    // L3+R3 "open menu" chord, offering an EXIT control -- this
    // screen previously had none at all (GitHub issues #8, #9).
    // Same deliberate-hold pattern and menu-navigation conventions as
    // main()'s inner loop (see kMenuChordHoldUs's declaration for why).
    std::vector<std::string> menuItems = {"RESUME", "ENTER AN IP ADDRESS"};
    if (settings) menuItems.push_back("SETTINGS");
    menuItems.push_back("EXIT");
    bool menuActive = false;
    // Settings before connecting. The mic rows are shown too: the device
    // and mute choice are saved and used once a host with mic support
    // connects.
    bool settingsActive = false;
    auto openSettings = [&]() {
        menuActive = false;
        settingsActive = true;
        settings->open();
    };
    int menuSelectedIndex = 0;
    uint64_t menuChordSinceUs = 0;
    bool menuChordFired = false;
    MenuStickState stick;
    const uint64_t openedAtUs = SDL_GetTicksNS() / 1000;

    // For a host that never answers discovery (another subnet, a
    // firewall): type its address instead. Ports are the defaults.
    auto enterAddress = [&]() -> std::optional<DiscoveredHost> {
        auto address = runAddressEntry(renderer, SDL_GetRenderWindow(renderer), gamepad, lastHostAddress);
        if (!address) return std::nullopt;
        DiscoveredHost host;
        host.address = *address;
        return host;
    };

    while (true) {
        MenuAction action = MenuAction::None;
        bool enterAddressPressed = false;
        bool openSettingsPressed = false;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (handleGamepadHotplug(event, gamepad)) continue;
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    return std::nullopt;
                case SDL_EVENT_KEY_DOWN:
                    // Only honored with no gamepad connected (Desktop
                    // Mode/keyboard testing convenience) -- see the
                    // matching gate in main()'s inner loop for why.
                    if (!gamepad && event.key.key == SDLK_ESCAPE) {
                        if (settingsActive) {
                            settingsActive = false;
                            menuActive = true;
                        } else {
                            menuActive = !menuActive;
                            menuSelectedIndex = 0;
                        }
                    } else {
                        action = menuActionForKey(event.key.key);
                    }
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    if (!menuActive && !settingsActive && event.gbutton.button == SDL_GAMEPAD_BUTTON_NORTH) {
                        enterAddressPressed = true;
                    } else if (settings && !menuActive && !settingsActive &&
                               event.gbutton.button == SDL_GAMEPAD_BUTTON_WEST) {
                        openSettingsPressed = true;
                    } else {
                        action = menuActionForButton(event.gbutton.button);
                    }
                    break;
                default:
                    break;
            }
        }

        // Held L3+R3 toggles the menu -- see kMenuChordHoldUs's
        // declaration for why a deliberate hold is required (and why L3+R3
        // rather than Start+Select).
        bool menuChordHeld = gamepad && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_STICK) &&
                             SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
        uint64_t nowForChordUs = SDL_GetTicksNS() / 1000;
        if (menuChordHeld) {
            if (menuChordSinceUs == 0) menuChordSinceUs = nowForChordUs;
            if (!menuChordFired && nowForChordUs - menuChordSinceUs >= kMenuChordHoldUs) {
                if (settingsActive) {
                    settingsActive = false;
                } else {
                    menuActive = !menuActive;
                    menuSelectedIndex = 0;
                }
                menuChordFired = true;
            }
        } else {
            menuChordSinceUs = 0;
            menuChordFired = false;
        }
        if (action == MenuAction::None) action = pollMenuStick(gamepad, stick, nowForChordUs);

        if (settingsActive) {
            switch (settings->handle(action, true, nullptr)) {
                case SettingsMenu::Result::Stay: break;
                case SettingsMenu::Result::Close:
                    settingsActive = false;
                    menuActive = true;
                    break;
                case SettingsMenu::Result::RunSetupWizard:
                    if (setupWizardRequested) *setupWizardRequested = true;
                    return std::nullopt;
            }
        } else if (menuActive) {
            const int menuCount = static_cast<int>(menuItems.size());
            if (action == MenuAction::Up) menuSelectedIndex = (menuSelectedIndex + menuCount - 1) % menuCount;
            if (action == MenuAction::Down) menuSelectedIndex = (menuSelectedIndex + 1) % menuCount;
            if (action == MenuAction::Back) menuActive = false; // back/cancel, no action taken
            if (action == MenuAction::Select) {
                const std::string& picked = menuItems[static_cast<size_t>(menuSelectedIndex)];
                if (picked == "EXIT") return std::nullopt;
                menuActive = false;
                if (picked == "ENTER AN IP ADDRESS") enterAddressPressed = true;
                if (picked == "SETTINGS") openSettingsPressed = true;
            }
        } else if (action == MenuAction::Back && backRequested) {
            *backRequested = true;
            return std::nullopt;
        } else if (!hosts.empty()) {
            const int count = static_cast<int>(hosts.size());
            if (action == MenuAction::Up) selectedIndex = (selectedIndex + count - 1) % count;
            if (action == MenuAction::Down) selectedIndex = (selectedIndex + 1) % count;
            if (action == MenuAction::Select) return hosts[static_cast<size_t>(selectedIndex)];
        }

        if (enterAddressPressed) {
            if (auto host = enterAddress()) return host;
        }
        if (openSettingsPressed) openSettings();

        if (settingsActive) {
            settings->render(renderer, true, -1.0f);
            SDL_Delay(kPickerFrameIntervalMs);
            continue;
        }

        if (menuActive) {
            renderPauseMenu(renderer, menuItems, menuSelectedIndex);
            SDL_Delay(kPickerFrameIntervalMs);
            continue;
        }

        const bool canGoBack = backRequested != nullptr;
        if (hosts.empty()) {
            const auto secondsSearching = static_cast<int>((nowForChordUs - openedAtUs) / 1'000'000);
            renderDiscoverySearching(renderer, clientVersion, secondsSearching, canGoBack, settings != nullptr);
        } else {
            renderDiscoveryList(renderer, hosts, selectedIndex, clientVersion, lastHostAddress, canGoBack,
                                settings != nullptr);
        }

        // Pull whatever the background scan thread has published so far --
        // never blocks on network I/O itself, just a quick copy under a
        // mutex, so this loop iterates at normal frame rate regardless of
        // scan timing (see the function-level comment above for why this
        // used to run discoverHosts() inline here instead).
        std::string selectedAddress = hosts.empty() ? "" : hosts[static_cast<size_t>(selectedIndex)].address;
        {
            std::lock_guard<std::mutex> lock(scanMutex);
            hosts = latestScan;
        }

        if (hosts.empty()) {
            selectedIndex = 0;
        } else {
            auto it = std::find_if(hosts.begin(), hosts.end(),
                                    [&](const DiscoveredHost& h) { return h.address == selectedAddress; });
            if (it != hosts.end()) {
                selectedIndex = static_cast<int>(it - hosts.begin());
            } else {
                auto lastIt = std::find_if(hosts.begin(), hosts.end(), [&](const DiscoveredHost& h) {
                    return h.address == lastHostAddress;
                });
                selectedIndex = lastIt != hosts.end() ? static_cast<int>(lastIt - hosts.begin()) : 0;
            }
        }

        SDL_Delay(kPickerFrameIntervalMs);
    }
}

} // namespace dualdeck::client
