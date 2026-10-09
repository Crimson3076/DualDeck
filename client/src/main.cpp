// DualDeck -- Steam Deck client.
//
// What this does today:
//  - Opens a 1280x800 window (Steam Deck panel resolution)
//  - On every launch, scans the LAN for available DualDeck hosts
//    and shows a gamepad/keyboard-navigable selection screen (spec
//    section 8.1's discovery, adapted per user request: always show the
//    picker rather than silently reconnecting to whichever host was used
//    last, so switching to a different HTPC is always one screen away)
//  - Connects to the chosen dualdeck-host-service host and displays
//    whatever bottom-screen frames it sends, aspect-correct-fit inside
//    the window
//  - Reads the first connected gamepad and maps it to DS buttons per
//    SPEC.md section 7.3
//  - Reads touchscreen (finger) events, and left-click/drag mouse events
//    (GitHub issue #23 -- e.g. a Steam Deck trackpad configured as a
//    mouse via Steam Input's "Trackpad" binding, an alternative to an
//    actual touchscreen), maps both through
//    melonds_remote::computeAspectFitRect / mapPointToDSCoords, and
//    ignores touches/clicks outside the rendered DS rectangle
//  - Sends a full ControllerState packet at a fixed ~120Hz rate
//    regardless of whether anything changed (spec section 6.3)
//  - Logs connection/controller/touch/frame events to stderr as the
//    "debug overlay" for this milestone (an on-screen overlay is future
//    work, see docs/architecture.md "Known gaps") -- stderr specifically,
//    not stdout, since stdout is fully buffered once redirected to a file
//    (the common case for troubleshooting), which can delay or lose these
//    messages entirely for a while; stderr isn't. Every one of these log
//    lines also goes to a persistent ~/.config/dualdeck-client/client.log
//    (see client_log.h) -- Steam Big Picture/Gaming Mode has no visible
//    terminal, so stderr alone is otherwise unrecoverable after the fact.
//  - Implements device-approval authentication (spec section 13,
//    replacing an earlier 6-digit-code-entry screen that required typing
//    on the client -- unworkable since Steam Input doesn't reliably bring
//    up a virtual keyboard in Gaming Mode, see docs/known-limitations.md):
//    sends a persistent, self-generated device identity on every Hello; a
//    human at the host approves or denies it, no typing anywhere.

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "bitmap_font.h"
#include "client_log.h"
#include "client_settings.h"
#include "device_identity.h"
#include "discovery_client.h"
#include "discovery_store.h"
#include "gamepad_input.h"
#include "host_picker.h"
#include "mic_capture.h"
#include "melonds_remote/protocol.h"
#include "melonds_remote/touch_mapping.h"
#include "net_client.h"
#include "screens.h"
#include "setup_wizard.h"
#include "wizard_state.h"

using namespace melonds_remote;
using namespace melonds_remote::client;

namespace {

// Matches host::NetServerConfig::discoveryPort's default (net_server.h).
constexpr uint16_t kDefaultDiscoveryPort = 8763;

// Wall-clock (epoch) microseconds, for the wire ControllerState.clientTimestampUs
// field specifically. Deliberately not SDL_GetTicksNS() (which is time since
// SDL_Init(), not comparable across processes/machines) -- the host uses this
// to estimate one-way input latency, which only makes sense against a shared
// time base (spec section 8.5). This assumes client and host clocks are
// reasonably synced (e.g. via NTP), same as the LAN latency targets in the
// spec already assume.
uint64_t wallClockNowUs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
}

// runCaptureStdout <command>
//
// Real user report, 2026-08-02: the trackpad-experiment toggle
// (docs/known-limitations.md's entry of the same date) originally only
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
// (never per-frame; see settingsMenuItems()'s own comment on why the
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

// logGamepadTouchpadDiagnostics <gamepad>
} // namespace

int main(int argc, char** argv) {
    initClientLog();

    NetClientConfig netConfig;
    // Set from DUALDECK_VERSION (exported by run-client.sh, read from
    // the archive's VERSION file) rather than baked in at compile time
    // -- see net_server.h's NetServerConfig::appVersion and
    // protocol.h's HelloPayload::appVersion for what this is compared
    // against and why. This is this client binary's own env var, not
    // the one the patched melonDS/Azahar reads for the same purpose on
    // the host side (MELONDS_REMOTE_VERSION/AZAHAR_REMOTE_VERSION,
    // deliberately left alone -- see docs/known-limitations.md's
    // rebrand section for why those stay as-is). Empty (unset) disables
    // the version-mismatch check for this connection, e.g. for a
    // from-source dev build run directly, not via run-client.sh.
    if (const char* envVersion = std::getenv("DUALDECK_VERSION")) {
        netConfig.appVersion = envVersion;
    }
    bool authTokenExplicit = false; // --auth-token given: skip device-approval entirely (CI/scripting use)
    bool hostExplicit = false;      // --host/positional given: skip LAN discovery entirely
    uint16_t discoveryPort = kDefaultDiscoveryPort;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto nextArg = [&]() -> std::string {
            if (i + 1 >= argc) {
                logLine("missing value for %s\n", arg.c_str());
                std::exit(1);
            }
            return argv[++i];
        };

        if (arg == "--host") {
            netConfig.hostAddress = nextArg();
            hostExplicit = true;
        } else if (arg == "--auth-token") {
            netConfig.authToken = nextArg();
            authTokenExplicit = true;
        } else if (arg == "--client-name") {
            netConfig.clientName = nextArg();
        } else if (arg == "--discovery-port") {
            discoveryPort = static_cast<uint16_t>(std::stoi(nextArg()));
        } else if (arg == "--app-version") {
            netConfig.appVersion = nextArg(); // overrides DUALDECK_VERSION above
        } else if (!arg.empty() && arg[0] != '-') {
            // Positional host address, for scripts/run-client.sh's
            // `dualdeck-client 127.0.0.1` convenience form.
            netConfig.hostAddress = arg;
            hostExplicit = true;
        } else {
            logLine("unrecognized argument: %s\n", arg.c_str());
            return 1;
        }
    }

    const std::string discoveryStorePath = defaultLastHostStorePath();

    // SDL_INIT_AUDIO is required for MicCapture's SDL_OpenAudioDeviceStream
    // calls (GitHub issue #2) to succeed at all -- without it every open()
    // just fails silently, so the client would connect fine but never
    // actually stream microphone audio no matter what the user picks in
    // Settings.
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        logLine("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("DualDeck", kWindowWidth, kWindowHeight,
                                           SDL_WINDOW_FULLSCREEN);
    if (!window) {
        logLine("SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        logLine("SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Every UI/touch-hit-test coordinate in this file is computed against
    // the kWindowWidth/kWindowHeight constants (1280x800, Steam Deck's
    // exact panel resolution) -- SDL_CreateWindow's SDL_WINDOW_FULLSCREEN
    // flag actually fullscreens at the real display's native resolution
    // regardless of the size passed to it (confirmed in SDL3's own
    // SDL_video.h: "fullscreen window at desktop resolution"), so on any
    // other device -- an 1920x1080 ROG Ally, or simply a future display --
    // this file's own 1280x800-based rendering just occupied the top-left
    // 1280x800 pixels of a larger real backbuffer, uncentered and
    // unscaled. SDL_SetRenderLogicalPresentation makes SDL do the
    // scale-and-letterbox itself on every subsequent SDL_Render* call, so
    // none of this file's existing 1280x800 layout math needs to change --
    // it draws into a virtual 1280x800 canvas that SDL maps onto whatever
    // the real window/display size turns out to be. LETTERBOX (not
    // STRETCH) to preserve the UI's own aspect ratio rather than
    // distorting bitmap text; touch coordinates need no corresponding
    // fix since event.tfinger.x/y are already normalized 0..1 fractions
    // of the real window, independent of its actual pixel size.
    if (!SDL_SetRenderLogicalPresentation(renderer, kWindowWidth, kWindowHeight,
                                           SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
        logLine("SDL_SetRenderLogicalPresentation failed: %s\n", SDL_GetError());
    }

    // The wire format (docs/protocol.md) and melonDS's own software-renderer
    // output are B,G,R,X bytes in memory -- SDL_PIXELFORMAT_BGRA32 is the
    // constant that actually means that. SDL_PIXELFORMAT_BGRA8888 (no "32")
    // is a *packed*-format name, not a byte-order-in-memory one: on a
    // little-endian machine it names a completely different byte order
    // (equivalent to ARGB8888's byte order) due to how SDL defines packed
    // formats as a bit layout read MSB-to-LSB of a 32-bit int, which is
    // reversed in memory on little-endian -- feeding real B,G,R,X bytes to
    // a texture declared BGRA8888 showed as flatly wrong colors (verified:
    // pure red bytes rendered as black). Do not "fix" this back to
    // BGRA8888 -- see SDL_pixels.h's SDL_PIXELFORMAT_BGRA32 definition.
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_BGRA32,
                                              SDL_TEXTUREACCESS_STREAMING, kDSWidth, kDSHeight);
    if (!texture) {
        logLine("SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    // Real user question, 2026-08-26: a Wii U title's captured GamePad
    // frame (Cemu's own internal render resolution, commonly 854x480 --
    // see host/cemu-patches/'s CemuAdapter.cpp, unrelated to the
    // client's own display size) stretched up to fill a much larger
    // Deck screen looked soft. Made explicit here rather than left as an
    // implicit default: SDL3's SDL_CreateTexture() already defaults new
    // textures to SDL_SCALEMODE_LINEAR (confirmed by reading SDL's own
    // source, src/render/SDL_render.c), so this call is a no-op today,
    // not a fix -- there was no accidental nearest-neighbor/blocky
    // scaling bug here to begin with. Spelled out explicitly so this
    // renderer's actual scaling behavior doesn't silently depend on a
    // default that could change in a future SDL release, and so a
    // reader doesn't have to go check SDL's source to know what this
    // does. The real lever for a sharper picture is upstream of this
    // texture entirely -- how much detail Cemu's own DRC render target
    // actually captured -- not anything this client can conjure from a
    // low-resolution source via a smarter resize filter.
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    // Opaque video feed, not a translucent overlay -- SDL3 defaults a
    // texture's blend mode to SDL_BLENDMODE_BLEND whenever its pixel
    // format carries an alpha channel (SDL_PIXELFORMAT_BGRA32 does), so
    // without this, any frame byte whose alpha isn't exactly 0xFF gets
    // alpha-composited against the window's black background instead of
    // drawn as-is. Some Azahar 3D content (confirmed via renderer_vulkan.cpp:
    // ApplySecondLayerOpacity's constant-alpha blend pipeline, used by
    // DrawScreens for every screen blit) can leave a captured frame's alpha
    // channel at less than 0xFF -- invisible on the host's own window
    // (compositors generally treat a normal window surface as opaque
    // regardless of its alpha channel) but very visible here once streamed,
    // matching the "3D content renders too dark" reports. Forcing NONE here
    // makes the alpha byte inert, which is correct for every source (DS and
    // 3DS alike) since this texture only ever holds an already-composited
    // screen capture that was never meant to be translucent.
    if (!SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE)) {
        logLine("SDL_SetTextureBlendMode failed: %s\n", SDL_GetError());
        return 1;
    }
    // Starts at DS's native size (matching the texture just created above)
    // and is recreated at the connected host's own reported native
    // dimensions once a real HelloAck arrives -- see the "nowConnected &&
    // !wasConnected" block below. Not every host is DS-sized: AzaharAdapter
    // (3DS) reports 320x240, and a video frame that size used to always be
    // silently dropped since this texture, and every size check against
    // it, stayed hardcoded at kDSWidth/kDSHeight regardless of what the
    // host actually streamed.
    int textureWidth = kDSWidth;
    int textureHeight = kDSHeight;

    SDL_Gamepad* gamepad = nullptr;
    int gamepadCount = 0;
    SDL_JoystickID* gamepadIds = SDL_GetGamepads(&gamepadCount);
    if (gamepadIds && gamepadCount > 0) {
        gamepad = SDL_OpenGamepad(gamepadIds[0]);
        logLine("[input] opened gamepad: %s\n", gamepad ? SDL_GetGamepadName(gamepad) : "?");
        // See logGamepadTouchpadDiagnostics's own comment -- the Deck's
        // built-in controller is opened right here, not through the
        // SDL_EVENT_GAMEPAD_ADDED handler further down, so this is the
        // one place that actually needs to call it for that case.
        logGamepadTouchpadDiagnostics(gamepad);
    }
    if (gamepadIds) SDL_free(gamepadIds);

    // Device-approval authentication (spec section 13): unless the caller
    // gave an explicit static --auth-token, send this client's own
    // persistent device identity -- the same value used with every host,
    // every time. A human at the host approves or denies it once; there
    // is nothing to type or store per-host on the client side (see
    // device_identity.h and docs/protocol.md's "Authentication and
    // device approval" section). Loaded once, outside the reconnect-loop
    // below, since it doesn't depend on which host is chosen.
    if (!authTokenExplicit) {
        netConfig.authToken = loadOrCreateDeviceIdentity(defaultDeviceIdentityStorePath());
    }

    // First-run setup wizard (GitHub issue #19): runs once automatically,
    // then only reachable again from the Settings screen in the pause menu
    // below (see wizard_state.h). Skipped entirely for an explicit --host/
    // positional address, same reasoning as "CHANGE HOST" being hidden in
    // that case -- an explicit host address means scripted/CI use, not an
    // interactive first-time user.
    const std::string wizardStatePath = defaultWizardStatePath();
    bool runWizardNow = !hostExplicit && !isSetupComplete(wizardStatePath);
    const std::string clientSettingsPath = defaultClientSettingsPath();
    ClientSettings clientSettings = loadClientSettings(clientSettingsPath);

    // L3+R3 "open menu" chord state -- see kMenuChordHoldUs's
    // declaration above for why a deliberate hold is required.
    // menuChordSinceUs == 0 means "not currently held"; menuChordFired
    // prevents re-triggering on every frame for as long as the hold
    // continues, only resetting once the chord is released.
    uint64_t menuChordSinceUs = 0;
    bool menuChordFired = false;

    // Last known emulated-system/adapter identity (GitHub issue #28),
    // shown in the connected-session menu and on the disconnected/
    // reconnecting overlay. Primed from the discovery selection (so it's
    // available even before the first successful handshake) and kept
    // up to date from net.hostSystemIdentity()/hostAdapterIdentity()
    // once connected; deliberately never cleared on disconnect, so a
    // dropped connection's reconnect/error screen still shows "what was
    // I connected to" rather than going blank (issue #28: "preserve it
    // on reconnect/error screens where practical"). Reset naturally on
    // "Change Host" since a fresh discovery selection overwrites it.
    std::string sessionSystemName;
    std::string sessionAdapterName;
    // Machine-comparable systemId ("nds", "3ds", ...), tracked alongside
    // sessionSystemName/sessionAdapterName for the same reasons (primed
    // from discovery, refreshed from HelloAck) -- used to gate the
    // stick-as-alternate-d-pad convenience below to DS sessions only, see
    // its use site's comment.
    std::string sessionSystemId;
    auto identityLine = [&]() -> std::string {
        if (sessionSystemName.empty() && sessionAdapterName.empty()) return "";
        if (sessionAdapterName.empty()) return sessionSystemName;
        if (sessionSystemName.empty()) return sessionAdapterName;
        // The bitmap font (client/src/bitmap_font.cpp) only has glyphs
        // for space, 0-9, A-Z, '.', '-', ':' -- no multi-byte UTF-8
        // separator like the middle dot in issue #28's own example text,
        // so "-" stands in for it on this screen.
        return sessionSystemName + " - " + sessionAdapterName;
    };

    // Outer loop lets "Change Host" (from the in-app menu below) return to
    // the discovery/selection screen without exiting the whole process --
    // everything from discovery through the connected render loop reruns
    // per host. Only reachable when !hostExplicit (an explicit --host/
    // positional address has nothing to fall back to, so that menu entry
    // is hidden in that case -- see menuItems below).
    bool quitApp = false;
    // Real user report, 2026-08-26: "I am not sure changing the video
    // quality changes until restart." Root cause: it never applied until
    // the *next connection* (see netConfig.videoQuality's comment below
    // -- there's no packet type for changing an already-connected
    // session's compression quality), but nothing actually triggered a
    // new connection when the setting changed -- a user who didn't
    // separately, manually pick CHANGE HOST (which also detours through
    // the full discovery picker, even to reconnect to the exact same
    // host) would see the old quality/codec keep streaming indefinitely,
    // indistinguishable from the setting simply not having saved.
    // cycleVideoQuality()/toggleVideoCodec() below (settingsMenuItems())
    // now set this flag -- but the actual reconnect only happens once the
    // user leaves the Settings screen (every settingsActive -> false
    // transition below checks it), not the instant either value changes,
    // so cycling through VIDEO QUALITY's presets or flipping VIDEO CODEC
    // a few times while still browsing Settings doesn't reconnect after
    // every single tap; at most one reconnect happens, covering whatever
    // was last picked when Settings is actually closed. Declared here
    // (outside the loop, alongside netConfig itself) rather than freshly
    // inside each outer iteration, since it must still be readable at the
    // very top of the *next* iteration (see the discovery-picker check
    // just inside this loop) to skip the picker and silently reconnect to
    // the same host instead of asking the user to re-pick it.
    bool reconnectRequested = false;
    while (!quitApp) {
        if (runWizardNow) {
            runWizardNow = false;
            bool wizardCompleted =
                runSetupWizard(window, renderer, texture, gamepad, discoveryPort, netConfig, discoveryStorePath);
            if (wizardCompleted) {
                markSetupComplete(wizardStatePath);
            } else {
                logLine("[wizard] cancelled during first run -- exiting\n");
                quitApp = true;
                break;
            }
            continue; // re-enter the loop: show the normal discovery screen next, same as any other launch
        }

        // LAN discovery (spec section 8.1): always shown unless --host/a
        // positional address was given, matching the existing scripted/CI
        // use (run-client.sh, --auth-token flows). Per user request, this
        // always runs -- even if only one host answers, or it's the same
        // one as last time -- rather than silently reconnecting, so a
        // different HTPC is always one screen away.
        if (reconnectRequested) {
            // A settings change (cycleVideoQuality()/toggleVideoCodec())
            // asked for a fresh connection to actually apply -- reuse
            // netConfig.hostAddress/ports exactly as the just-ended
            // connection had them (they're never cleared between outer-
            // loop iterations), same as the hostExplicit branch below,
            // rather than sending the user through the discovery picker
            // to re-pick a host they never asked to change.
            reconnectRequested = false;
            renderConnecting(renderer, netConfig.hostAddress);
            logLine("[settings] reconnecting to \"%s\" to apply the changed setting\n",
                        netConfig.hostAddress.c_str());
        } else if (!hostExplicit) {
            std::string lastHost = loadLastHost(discoveryStorePath).value_or("");
            auto selected = discoverAndSelectHost(renderer, gamepad, discoveryPort, lastHost, netConfig.appVersion);
            if (!selected) {
                logLine("[discovery] cancelled before a host was chosen -- exiting\n");
                quitApp = true;
                break;
            }
            netConfig.hostAddress = selected->address;
            netConfig.controlPort = selected->controlPort;
            netConfig.inputPort = selected->inputPort;
            netConfig.videoPort = selected->videoPort;
            netConfig.audioPort = selected->audioPort;
            // Primed from discovery so the identity line has something to
            // show during "CONNECTING..." -- refreshed from the real
            // HelloAck once connected (see the render loop below), which
            // is authoritative if it ever disagrees with what discovery
            // last reported.
            sessionSystemName = selected->system.systemName;
            sessionAdapterName = selected->adapter.adapterName;
            sessionSystemId = selected->system.systemId;
            // Acknowledge the button press before saving the selection or
            // starting any socket work. The previous synchronous connect
            // left the picker frozen until the host responded, making a
            // successful selection look ignored in Gaming Mode (GitHub
            // issue #21).
            renderConnecting(renderer, netConfig.hostAddress);
            saveLastHost(discoveryStorePath, netConfig.hostAddress);
            logLine("[discovery] selected host \"%s\" at %s\n", selected->hostName.c_str(),
                        netConfig.hostAddress.c_str());
        } else {
            // Explicit-host/scripted launches skip the picker but should
            // still expose the same connection state once the window opens.
            renderConnecting(renderer, netConfig.hostAddress);
        }

        // Read fresh every time a NetClient is (re)constructed, not just
        // once at startup, so picking a new VIDEO QUALITY in Settings and
        // then reconnecting (CHANGE HOST, or a dropped connection) takes
        // effect without needing to relaunch -- see settingsMenuItems()'s
        // cycleVideoQuality() below for how clientSettings.videoQuality
        // gets changed.
        netConfig.videoQuality = static_cast<uint8_t>(clientSettings.videoQuality);
        // Same "read fresh on every (re)construction" reasoning as
        // videoQuality above -- see NetClientConfig::preferH264's own
        // comment for why the host still gets the final say either way.
        netConfig.preferH264 = clientSettings.videoCodecH264Experimental;
        netConfig.preferPyroWave = clientSettings.videoCodecPyroWaveExperimental;
        NetClient net(netConfig);

        // Forwards every logLine() call to the host as a ClientLog packet
        // (host debugging/app development) for as long as this particular
        // `net` is alive -- cleared by the guard below before `net` itself
        // is destroyed, since a sink still holding a reference to an
        // already-destroyed NetClient would dangle the moment this loop
        // picks a new host or exits.
        setLogForwardSink([&net](const std::string& line) { net.sendClientLog(line); });
        struct LogForwardGuard {
            ~LogForwardGuard() { setLogForwardSink({}); }
        } logForwardGuard;

        // Auto-reconnect (spec section 7.2): connect() does several blocking
        // socket calls, so retries run on their own thread rather than
        // stalling the render/input loop below. Backoff caps at 5s so a
        // permanently-unreachable (or not-yet-approved) host doesn't spin the
        // CPU. Unlike the old pairing-code flow, there is no reason to ever
        // pause these retries waiting on client-side user action -- approval
        // happens entirely on the host, so the same reconnect loop that
        // handles a temporarily-down host also naturally handles "not
        // approved yet" (it'll just start succeeding once approved). This
        // thread owns every attempt, including the initial one; previously
        // main() made a synchronous attempt first and then immediately
        // handed the same disconnected client to this thread, causing a
        // duplicate attempt after an initial failure (GitHub issue #21).
        std::atomic<bool> shuttingDown{false};
        std::thread reconnectThread([&]() {
            uint32_t backoffMs = 1000;
            constexpr uint32_t kMaxBackoffMs = 5000;
            while (!shuttingDown.load()) {
                if (!net.isConnected()) {
                    logLine("[net] attempting to (re)connect to %s...\n",
                                netConfig.hostAddress.c_str());
                    if (net.connect()) {
                        logLine("[net] connected (session %u)\n", net.sessionId());
                        backoffMs = 1000;
                    } else {
                        backoffMs = std::min(backoffMs * 2, kMaxBackoffMs);
                    }
                }
                for (uint32_t waited = 0; waited < backoffMs && !shuttingDown.load(); waited += 100) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        });

        uint32_t sequence = 0;
        // See the identity-refresh block below (GitHub issue #28) for
        // why this exists: detects the disconnected->connected edge
        // rather than re-reading net.host*Identity() every frame.
        bool wasConnected = false;
        // Tracks the same edge for net.hostMode() (GitHub issue #4 Phase
        // E) -- a host can flip modes mid-session (an adapter connecting/
        // disconnecting, or a manual override), each transition carrying
        // a fresh identity via ModeChanged that the block below should
        // pick up the same way it already does for the initial connect.
        HostMode lastHostMode = HostMode::Emulation;
        // See the SDL_SetWindowRelativeMouseMode() call further down for
        // why this exists -- tracked separately from lastHostMode/
        // wasConnected so the toggle only ever fires on a real transition,
        // never redundantly every frame (SDL_mouse.h: this call "will
        // flush any pending mouse motion for this window" every time it's
        // called, so calling it unconditionally on every frame would
        // discard real, in-flight motion constantly).
        bool wasRelativeMouseMode = false;
        bool touchActive = false;
        uint16_t touchX = 0;
        uint16_t touchY = 0;
        std::optional<int64_t> activeFingerId;
        // Mouse-click touch (GitHub issue #23): left click/drag maps to a
        // touch the same way a finger does, so a Steam Deck trackpad
        // configured as a mouse (Steam Input's default "Trackpad" binding,
        // or a real mouse in Desktop Mode) works as an alternative to an
        // actual touchscreen. Tracked separately from activeFingerId so
        // releasing one input source doesn't clear a touch still being
        // held by the other -- touchActive is the OR of both below.
        bool mouseTouchDown = false;

        // Host-control mode's virtual mouse (see
        // host::HostControlAdapter): accumulated relative motion since
        // the last ControllerState packet was sent (reset to 0 on every
        // send, below), and current left/right button-held state. Unlike
        // touchX/Y/mouseTouchDown above, this is never gated on the DS
        // rectangle or on a button already being held -- Host Control has
        // no notion of either, it's plain cursor motion. Accumulating and
        // sending unconditionally (not just while nowHostMode ==
        // HostMode::HostControl) is harmless: HostControlAdapter is the
        // only thing that ever reads these fields (see
        // protocol.h::ControllerState's own comment), so a real emulator
        // adapter simply never looks at them, the same way
        // HostControlAdapter never looks at touchX/Y.
        int32_t hostControlMouseDeltaX = 0;
        int32_t hostControlMouseDeltaY = 0;
        uint8_t hostControlMouseButtons = 0;

        // Real user report, 2026-08-01: SDL_EVENT_MOUSE_MOTION above never
        // fired at all on real hardware -- it only exists if Steam Input's
        // *currently bound control scheme* maps a touchpad to "as mouse,"
        // which most gamepad-style templates (including whatever this
        // client's own Steam shortcut defaults to) don't do. SDL's gamepad
        // touchpad API (SDL_EVENT_GAMEPAD_TOUCHPAD_*) reads the Deck's
        // touchpads directly as raw touch data instead -- the same
        // mechanism PS4/PS5 controller touchpad support uses -- and Steam
        // Input passes this through unconditionally, regardless of
        // whatever the active control scheme binds face buttons/sticks to.
        // This is the reliable path; SDL_EVENT_MOUSE_MOTION above is kept
        // too (harmless if it never fires) for a real desktop mouse in
        // Desktop Mode, or a control scheme that does bind "as mouse".
        //
        // Reports absolute, normalized (0..1) finger position per
        // (touchpad, finger) -- converted to a delta against the
        // previously-seen position for that same finger, since the wire
        // protocol wants relative motion (see mouseDeltaX/Y's own
        // comment). No delta is emitted for the first position seen after
        // a touch begins (nothing to diff against yet -- would otherwise
        // produce a spurious jump from whatever an uninitialized "previous
        // position" happened to be). Bounded array rather than a map:
        // the Deck has 2 touchpads with 1 finger each in practice: sized
        // generously and bounds-checked below so an unexpected controller
        // with more of either is just silently ignored past the bound,
        // not a crash.
        constexpr int kMaxTrackedTouchpads = 4;
        constexpr int kMaxTrackedFingersPerTouchpad = 2;
        // Chosen so a full swipe across the Deck's ~3.2cm touchpad (the
        // normalized 0..1 range) covers roughly a third of the 1280px-wide
        // client window's worth of cursor travel -- arbitrary but usable;
        // no real-hardware feel testing has tuned this yet (see
        // docs/known-limitations.md).
        constexpr float kTouchpadMouseSensitivity = 900.0f;
        std::optional<std::pair<float, float>>
            lastTouchpadPos[kMaxTrackedTouchpads][kMaxTrackedFingersPerTouchpad];
        // See the SDL_EVENT_GAMEPAD_TOUCHPAD_* case's own comment.
        bool loggedFirstTouchpadEvent = false;

        std::vector<uint8_t> frame;
        // Matches texture's current dimensions (textureWidth/textureHeight),
        // whatever they are at this point -- correct as long as every
        // recreation of texture below also resizes this alongside it.
        std::vector<uint8_t> testPattern(static_cast<size_t>(textureWidth) * textureHeight * 4, 0x40);

        // Latency-audit finding: this render loop used to call
        // net.getLatestFrame() (a full-vector copy of the decoded frame)
        // and SDL_UpdateTexture() (a GPU upload of that copy)
        // unconditionally on every single iteration, whether or not a new
        // frame had actually arrived since the last one -- with no vsync
        // cap on this renderer (see SDL_CreateRenderer() above), that's
        // real, wasted CPU/GPU work repeated far more often than
        // NetClient::receivedFps() ever changes. These four track what the
        // texture was last updated to reflect, so the block below
        // (`videoTextureNeedsUpdate` and its uses) can skip the copy+
        // upload entirely when nothing has changed -- see
        // NetClient::latestFrameGeneration()'s own comment for why a
        // generation counter is what actually answers "is there anything
        // new," not receivedFrameCount() (that would also tick up for a
        // frame this exact caller already saw). lastVideoTextureGeneration
        // starts at 0, same as a fresh NetClient's own generation counter,
        // so the very first iteration would otherwise look like "nothing
        // changed" -- videoTextureNeverUpdated forces the first real check
        // regardless.
        bool videoTextureNeverUpdated = true;
        uint64_t lastVideoTextureGeneration = 0;
        bool lastVideoTextureConnected = false;
        int lastVideoTextureWidth = textureWidth;
        int lastVideoTextureHeight = textureHeight;

        // Recreates `texture` (and its matching testPattern filler) at
        // newWidth x newHeight if that differs from what's currently
        // allocated -- a no-op cheap enough to call every frame. Used both
        // at the connect/mode-transition edge below (an Azahar/3DS host's
        // 320x240 vs. a melonDS/DS one's 256x192) and, per-frame, for a
        // genuine mid-session resize (real bug this fixes: CemuAdapter's
        // actual capture resolution isn't known at Hello time and can
        // differ from whatever was negotiated then -- see
        // adapter_contract.h's SurfaceFrame comment -- so the correct size
        // may only become known partway through a connection, not at its
        // start).
        auto resizeTextureIfNeeded = [&](int newWidth, int newHeight) {
            if (newWidth == textureWidth && newHeight == textureHeight) return;
            SDL_DestroyTexture(texture);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_BGRA32,
                                         SDL_TEXTUREACCESS_STREAMING, newWidth, newHeight);
            if (!texture) {
                // Extremely unlikely (the original creation at startup
                // already proved the renderer can make textures at all) --
                // fall back to the old dimensions rather than leaving
                // texture null and crashing the next SDL_UpdateTexture/
                // RenderTexture call below.
                logLine("SDL_CreateTexture failed while resizing for new host "
                        "dimensions: %s\n",
                        SDL_GetError());
                texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_BGRA32,
                                             SDL_TEXTUREACCESS_STREAMING, textureWidth, textureHeight);
            } else {
                textureWidth = newWidth;
                textureHeight = newHeight;
                testPattern.assign(static_cast<size_t>(textureWidth) * textureHeight * 4, 0x40);
                logLine("[video] texture resized to %dx%d for this host\n", textureWidth,
                              textureHeight);
            }
            // Every fresh texture needs the same opaque blend mode as the
            // startup one (see its own comment) -- SDL3 resets blend mode
            // to its own per-format default on each new SDL_CreateTexture
            // call, it isn't inherited from the destroyed texture. Same
            // reasoning for scale mode (see the startup texture's own
            // SDL_SetTextureScaleMode() comment) -- a fresh texture
            // starts back at SDL_CreateTexture()'s own default rather
            // than inheriting the destroyed texture's mode either.
            if (texture && !SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE)) {
                logLine("SDL_SetTextureBlendMode failed: %s\n", SDL_GetError());
            }
            if (texture) {
                SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
            }
        };

        const uint64_t inputIntervalUs = 1'000'000 / 120; // spec section 6.3
        uint64_t lastInputSendUs = SDL_GetTicksNS() / 1000;

        // In-app menu (spec request: "no sort of menu to configure settings
        // or exit"): held L3+R3 toggles it. "Change Host" is only
        // offered when discovery is in play at all -- an explicit --host
        // has no host list to go back to.
        std::vector<std::string> menuItems = {"RESUME"};
        if (!hostExplicit) menuItems.push_back("CHANGE HOST");
        menuItems.push_back("SETTINGS");
        // "EXIT EMULATION" ends the game/app running on the HOST (GitHub
        // issue #25) -- distinct from "EXIT" below, which only quits this
        // client. Picking it opens a second-level confirm menu rather than
        // acting immediately, since either choice there is destructive on
        // a machine the user isn't necessarily looking at (per SPEC.md's
        // "Wii U GamePad" model, the host's own screen is showing the top
        // screen only while a client streams, and no one may be at the
        // host to see or undo a mistake) -- see exitEmulationItems below.
        menuItems.push_back("EXIT EMULATION");
        menuItems.push_back("EXIT");
        bool menuActive = false;
        int menuSelectedIndex = 0;
        bool settingsActive = false;
        int settingsSelectedIndex = 0;
        bool settingsSaveFailed = false;

        // Microphone capture (GitHub issue #2). Opened once the host's
        // HelloAck reports micSupported; reopened only when the user
        // picks a different device in Settings. Muting doesn't close it
        // -- see MicCapture::open()'s comment -- so the level meter
        // keeps reflecting real input while muted, distinct from "no
        // signal." Closed on disconnect (below) so a stale device isn't
        // left open across host switches. Declared before
        // settingsMenuItems/cycleMicDevice below since their `[&]`
        // lambdas can only capture names already in scope.
        melonds_remote::client::MicCapture micCapture;
        std::vector<int16_t> micPendingSamples;
        uint32_t micSequence = 0;
        float micLevel = 0.0f;

        // Labels the current ClientSettings::videoQuality value for the
        // settings menu -- a handful of named presets rather than a raw
        // number, matching this menu's cycle-through-fixed-choices style
        // (MICROPHONE:/AUTO UPDATE ON LAUNCH: above) instead of a
        // continuous slider this text UI has no widget for.
        auto videoQualityLabel = [](int quality) -> std::string {
            if (quality == 0) return "AUTO";
            if (quality <= 40) return "LOW (SLOWEST LINKS)";
            if (quality <= 65) return "MEDIUM";
            if (quality <= 85) return "HIGH";
            return "MAXIMUM (LARGEST)";
        };
        // Trackpad-as-native-input experiment (see runCaptureStdout's own
        // comment). Cached, not queried live inside settingsMenuItems()
        // below -- that lambda runs every frame while the Settings screen
        // is open (it's called from the render loop, see this function's
        // renderPauseMenu() call), and shelling out to a script on every
        // single frame would be wasteful/janky. Instead this is refreshed
        // only when Settings is actually opened (both the keyboard and
        // gamepad "SETTINGS" handlers below) and right after toggling it,
        // matching micLevel/micPendingSamples' own "updated at specific
        // trigger points, not recomputed on every read" pattern above.
        bool trackpadExperimentEnabled = false;
        auto refreshTrackpadExperimentStatus = [&]() {
            // NOT "./internal/configure-trackpad-experiment.sh" -- see
            // runCaptureStdout's own comment on why this binary's CWD is
            // already .../internal/, so that extra prefix silently
            // pointed at a nonexistent path (real user report,
            // 2026-08-02: the toggle appeared to do nothing at all).
            trackpadExperimentEnabled =
                runCaptureStdout("./configure-trackpad-experiment.sh --status 2>/dev/null") == "disabled";
        };
        auto toggleTrackpadExperiment = [&]() {
            // --no-restart: see runCaptureStdout's own comment -- never
            // let this trigger Steam restarting itself while this
            // client is running as a live Steam-launched process.
            runCaptureStdout(trackpadExperimentEnabled
                                  ? "./configure-trackpad-experiment.sh --remove --no-restart 2>&1"
                                  : "./configure-trackpad-experiment.sh --no-restart 2>&1");
            // Re-queries rather than just flipping the cached bool --
            // the underlying script writes with --force in --no-restart
            // mode (see its own comment), so this should reliably
            // reflect the just-requested state, but re-checking what's
            // actually on disk is still cheap and more honest than
            // assuming.
            refreshTrackpadExperimentStatus();
        };
        auto settingsMenuItems = [&]() {
            std::vector<std::string> items{
                std::string("AUTO UPDATE ON LAUNCH: ") +
                    (clientSettings.autoUpdateOnLaunch ? "ON" : "OFF"),
                std::string("VIDEO QUALITY: ") + videoQualityLabel(clientSettings.videoQuality),
                std::string("TRACKPAD AS NATIVE INPUT (EXPERIMENTAL): ") +
                    (trackpadExperimentEnabled ? "ON" : "OFF"),
                std::string("MIRROR HOST SCREEN (EXPERIMENTAL): ") +
                    (clientSettings.mirrorHostScreen ? "ON" : "OFF"),
                std::string("VIDEO CODEC (EXPERIMENTAL): ") +
                    (clientSettings.videoCodecPyroWaveExperimental ? "PYROWAVE"
                     : clientSettings.videoCodecH264Experimental  ? "H264"
                                                                  : "JPEG"),
                std::string("DEBUG OVERLAY: ") + (clientSettings.debugOverlayEnabled ? "ON" : "OFF"),
            };
            if (!hostExplicit) items.push_back("RUN SETUP WIZARD");
            if (net.hostMicSupported()) {
                std::string micLabel =
                    clientSettings.micDeviceName.empty() ? "SYSTEM DEFAULT" : clientSettings.micDeviceName;
                items.push_back(std::string("MICROPHONE: ") + micLabel);
                items.push_back(std::string("MIC: ") + (clientSettings.micMuted ? "MUTED" : "ON"));
            }
            items.push_back("BACK");
            return items;
        };
        // Cycles clientSettings.videoQuality through a fixed set of
        // presets (see videoQualityLabel above), wrapping back to AUTO.
        // Only takes effect on the next connection (see netConfig.
        // videoQuality's own comment above, near NetClient's
        // construction) -- there's no packet type for changing an
        // already-connected session's compression quality. Sets
        // reconnectRequested so leaving Settings afterward actually gets
        // that next connection, instead of silently keeping the old
        // quality until some unrelated later reconnect -- see
        // reconnectRequested's own comment, near the outer loop, for the
        // real user report this fixes and why the reconnect itself is
        // deferred to Settings-exit rather than firing immediately here.
        auto cycleVideoQuality = [&]() {
            static constexpr int kPresets[] = {0, 40, 65, 85, 100};
            constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));
            int currentIndex = 0;
            for (int i = 0; i < kPresetCount; ++i) {
                if (kPresets[i] == clientSettings.videoQuality) {
                    currentIndex = i;
                    break;
                }
            }
            clientSettings.videoQuality = kPresets[(currentIndex + 1) % kPresetCount];
            settingsSaveFailed = !saveClientSettings(clientSettingsPath, clientSettings);
            reconnectRequested = true;
        };
        // Cycles JPEG -> H264 -> PYROWAVE -> JPEG across
        // clientSettings.videoCodecH264Experimental/
        // videoCodecPyroWaveExperimental (see the latter's own comment
        // for why it's two flags). Same "only takes effect on the next
        // connection" limitation and the same reconnectRequested fix as
        // cycleVideoQuality() above -- codec preference is negotiated
        // once, in Hello, same as videoQuality.
        auto toggleVideoCodec = [&]() {
            if (clientSettings.videoCodecPyroWaveExperimental) {
                clientSettings.videoCodecPyroWaveExperimental = false;
                clientSettings.videoCodecH264Experimental = false;
            } else if (clientSettings.videoCodecH264Experimental) {
                clientSettings.videoCodecH264Experimental = false;
                clientSettings.videoCodecPyroWaveExperimental = true;
            } else {
                clientSettings.videoCodecH264Experimental = true;
            }
            settingsSaveFailed = !saveClientSettings(clientSettingsPath, clientSettings);
            reconnectRequested = true;
        };
        // Cycles clientSettings.micDeviceName to the next enumerated
        // recording device (wrapping back to "SYSTEM DEFAULT"), saves,
        // and reopens capture on the new device. Shared by the keyboard
        // and gamepad settings handlers below, same as the inline
        // AUTO UPDATE ON LAUNCH toggle they already duplicate.
        auto cycleMicDevice = [&]() {
            auto devices = melonds_remote::client::listMicDevices();
            size_t currentIndex = 0;
            for (size_t i = 0; i < devices.size(); ++i) {
                if (devices[i].name == clientSettings.micDeviceName) {
                    currentIndex = i;
                    break;
                }
            }
            size_t nextIndex = (currentIndex + 1) % devices.size();
            // "SYSTEM DEFAULT" is stored as an empty name (see
            // ClientSettings::micDeviceName's comment), never the literal
            // label -- so a later SDL enumeration change can't strand a
            // saved setting that no longer matches anything.
            clientSettings.micDeviceName =
                devices[nextIndex].id == SDL_AUDIO_DEVICE_DEFAULT_RECORDING ? "" : devices[nextIndex].name;
            settingsSaveFailed = !saveClientSettings(clientSettingsPath, clientSettings);
            micCapture.open(clientSettings.micDeviceName);
        };
        bool exitEmulationConfirm = false;
        int exitEmulationSelectedIndex = 0;
        // A lambda re-evaluated at every use site (same pattern as
        // settingsMenuItems above), not a fixed const vector -- the
        // middle item names whichever adapter is actually connected
        // (e.g. "EXIT AZAHAR ENTIRELY"), which used to be hardcoded to
        // "EXIT MELONDS ENTIRELY" even when connected to a non-melonDS
        // adapter.
        auto exitEmulationItems = [&]() {
            std::string adapterUpper = sessionAdapterName;
            std::transform(adapterUpper.begin(), adapterUpper.end(), adapterUpper.begin(),
                            [](unsigned char c) { return std::toupper(c); });
            std::string middleItem =
                adapterUpper.empty() ? "EXIT EMULATION ENTIRELY" : "EXIT " + adapterUpper + " ENTIRELY";
            return std::vector<std::string>{"EXIT ROM", middleItem, "CANCEL"};
        };
        // Sent as ControllerState::emulatorActions for a fixed window after
        // confirming a choice above, not just one packet -- input goes over
        // UDP (spec section 6.3), so a single-packet one-shot action risks
        // silently doing nothing if that one packet is lost. Continuous
        // per-frame state (buttons, touch) doesn't have this problem since
        // it's resent every packet regardless; a momentary action like this
        // needs its own resend window. See pendingEmulatorActionMs below
        // for how long.
        uint16_t pendingEmulatorAction = 0;
        uint64_t pendingEmulatorActionUntilUs = 0;
        constexpr uint64_t kPendingEmulatorActionUs = 250'000; // 250ms of ~120Hz packets
        bool changeHostRequested = false;
        bool setupWizardRequested = false;

        bool runningInner = true;
        while (runningInner) {
            // Use the connected host's actual reported aspect ratio, not the
            // DS/3DS-only 4:3 default -- textureWidth/textureHeight are
            // updated to the real HelloAck-reported dimensions per host
            // (e.g. Cemu's 854x480 GamePad surface), so a fixed 4:3 here
            // would letterbox/stretch anything else incorrectly.
            RenderRect dsRect = computeAspectFitRect(
                kWindowWidth, kWindowHeight,
                static_cast<double>(textureWidth) / static_cast<double>(textureHeight));

            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                switch (event.type) {
                    case SDL_EVENT_QUIT:
                        runningInner = false;
                        quitApp = true;
                        break;
                    case SDL_EVENT_GAMEPAD_ADDED:
                        if (!gamepad) {
                            gamepad = SDL_OpenGamepad(event.gdevice.which);
                            logGamepadTouchpadDiagnostics(gamepad);
                        }
                        break;
                    case SDL_EVENT_GAMEPAD_REMOVED:
                        if (gamepad && SDL_GetGamepadID(gamepad) == event.gdevice.which) {
                            SDL_CloseGamepad(gamepad);
                            gamepad = nullptr;
                            logLine("[input] gamepad disconnected\n");
                        }
                        break;
                    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
                    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
                    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP: {
                        // One-shot diagnostic (real user report, 2026-08-01:
                        // touchpad input never registered at all) -- logged
                        // before any of the filtering below, so this fires
                        // even for input this code ends up discarding,
                        // confirming definitively whether SDL ever delivers
                        // this event type at all on real hardware.
                        if (!loggedFirstTouchpadEvent) {
                            loggedFirstTouchpadEvent = true;
                            logLine("[input] first gamepad-touchpad event received: type=%d touchpad=%d "
                                    "finger=%d x=%.3f y=%.3f\n",
                                    static_cast<int>(event.type), event.gtouchpad.touchpad,
                                    event.gtouchpad.finger, event.gtouchpad.x, event.gtouchpad.y);
                        }
                        // Host-control cursor motion via SDL's gamepad
                        // touchpad API -- see lastTouchpadPos's own
                        // declaration comment for why this is the reliable
                        // path (independent of Steam Input's control
                        // scheme), unlike SDL_EVENT_MOUSE_MOTION above.
                        if (menuActive || settingsActive || !gamepad ||
                            event.gtouchpad.which != SDL_GetGamepadID(gamepad) ||
                            event.gtouchpad.touchpad < 0 ||
                            event.gtouchpad.touchpad >= kMaxTrackedTouchpads ||
                            event.gtouchpad.finger < 0 ||
                            event.gtouchpad.finger >= kMaxTrackedFingersPerTouchpad) {
                            break;
                        }
                        auto& lastPos = lastTouchpadPos[event.gtouchpad.touchpad][event.gtouchpad.finger];
                        if (event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_UP) {
                            lastPos.reset();
                            break;
                        }
                        if (lastPos) {
                            hostControlMouseDeltaX += static_cast<int32_t>(
                                (event.gtouchpad.x - lastPos->first) * kTouchpadMouseSensitivity);
                            hostControlMouseDeltaY += static_cast<int32_t>(
                                (event.gtouchpad.y - lastPos->second) * kTouchpadMouseSensitivity);
                        }
                        // else: SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN, or the
                        // first MOTION event this client happened to see
                        // for a finger it missed the DOWN for -- either
                        // way, nothing to diff against yet, so this
                        // position is recorded but no delta is emitted.
                        lastPos = std::make_pair(event.gtouchpad.x, event.gtouchpad.y);
                        break;
                    }
                    case SDL_EVENT_FINGER_DOWN:
                    case SDL_EVENT_FINGER_MOTION: {
                        if (menuActive || settingsActive) break;
                        double px = static_cast<double>(event.tfinger.x) * kWindowWidth;
                        double py = static_cast<double>(event.tfinger.y) * kWindowHeight;
                        auto mapped = mapPointToDSCoords(px, py, dsRect);
                        if (mapped) {
                            touchActive = true;
                            touchX = mapped->first;
                            touchY = mapped->second;
                            activeFingerId = static_cast<int64_t>(event.tfinger.fingerID);
                        } else if (event.type == SDL_EVENT_FINGER_DOWN) {
                            // touch started outside the DS rectangle: ignored per spec 7.4
                        }
                        break;
                    }
                    case SDL_EVENT_FINGER_UP:
                        if (activeFingerId &&
                            *activeFingerId == static_cast<int64_t>(event.tfinger.fingerID)) {
                            // Only clears touchActive if a mouse-driven touch
                            // isn't also currently in progress -- see
                            // mouseTouchDown's declaration above.
                            touchActive = mouseTouchDown;
                            activeFingerId.reset();
                        }
                        break;
                    case SDL_EVENT_MOUSE_BUTTON_DOWN:
                        // which == SDL_TOUCH_MOUSEID means this button event
                        // was synthesized from a real touch SDL already
                        // delivered as SDL_EVENT_FINGER_DOWN above -- skip it
                        // entirely (both the touch-emulation path below and
                        // the host-control click path) to avoid double-
                        // handling the same physical touch as two separate
                        // input sources.
                        if (menuActive || settingsActive || event.button.which == SDL_TOUCH_MOUSEID) {
                            break;
                        }
                        // Host-control click (see hostControlMouseButtons'
                        // declaration above): recorded regardless of where
                        // on screen the click landed -- Host Control has no
                        // "DS rectangle" concept, unlike the touch-emulation
                        // path below, which only fires for SDL_BUTTON_LEFT
                        // inside dsRect.
                        if (event.button.button == SDL_BUTTON_LEFT) {
                            hostControlMouseButtons |= MouseButton_Left;
                        } else if (event.button.button == SDL_BUTTON_RIGHT) {
                            hostControlMouseButtons |= MouseButton_Right;
                        }
                        if (event.button.button != SDL_BUTTON_LEFT) {
                            break;
                        }
                        if (auto mapped = mapPointToDSCoords(event.button.x, event.button.y, dsRect)) {
                            touchActive = true;
                            touchX = mapped->first;
                            touchY = mapped->second;
                            mouseTouchDown = true;
                        }
                        // else: click started outside the DS rectangle, same
                        // as an out-of-bounds finger touch above -- ignored
                        // for the touch-screen path (the host-control click
                        // above was already recorded either way).
                        break;
                    case SDL_EVENT_MOUSE_MOTION:
                        // Host-control cursor motion (see
                        // hostControlMouseDeltaX/Y's declaration above):
                        // accumulated on every real motion event regardless
                        // of whether a button is held -- a mouse cursor
                        // moves on plain hover, unlike the touch-drag path
                        // below, which only cares about motion while
                        // mouseTouchDown.
                        if (!menuActive && !settingsActive && event.motion.which != SDL_TOUCH_MOUSEID) {
                            hostControlMouseDeltaX += static_cast<int32_t>(event.motion.xrel);
                            hostControlMouseDeltaY += static_cast<int32_t>(event.motion.yrel);
                        }
                        // Only a drag (button already down) counts as touch
                        // movement -- plain cursor motion with no button
                        // held isn't a touch, unlike SDL_EVENT_FINGER_MOTION
                        // (a touchscreen has no "hover" state to generate
                        // motion events for in the first place).
                        if (menuActive || settingsActive || !mouseTouchDown ||
                            event.motion.which == SDL_TOUCH_MOUSEID) {
                            break;
                        }
                        if (auto mapped = mapPointToDSCoords(event.motion.x, event.motion.y, dsRect)) {
                            touchX = mapped->first;
                            touchY = mapped->second;
                        }
                        // else: dragged outside the DS rectangle -- keeps the
                        // last in-bounds position, same as finger motion.
                        break;
                    case SDL_EVENT_MOUSE_BUTTON_UP:
                        if (event.button.which == SDL_TOUCH_MOUSEID) {
                            break;
                        }
                        if (event.button.button == SDL_BUTTON_LEFT) {
                            hostControlMouseButtons &= static_cast<uint8_t>(~MouseButton_Left);
                        } else if (event.button.button == SDL_BUTTON_RIGHT) {
                            hostControlMouseButtons &= static_cast<uint8_t>(~MouseButton_Right);
                        }
                        if (event.button.button != SDL_BUTTON_LEFT) {
                            break;
                        }
                        if (mouseTouchDown) {
                            // Only clears touchActive if a finger touch isn't
                            // also currently in progress.
                            touchActive = activeFingerId.has_value();
                            mouseTouchDown = false;
                        }
                        break;
                    case SDL_EVENT_KEY_DOWN: {
                        // Only honored with no gamepad connected (Desktop
                        // Mode/keyboard testing convenience). On real Steam
                        // Deck hardware a gamepad is always present, and
                        // Steam Input's default binding template for a
                        // newly-added non-Steam shortcut synthesizes a
                        // keyboard Escape for individual button presses
                        // (observed: B and Start both opened the menu on
                        // real hardware even though neither is bound to it
                        // alone in the gamepad chord below) -- gating this
                        // on !gamepad means those synthesized keys are
                        // ignored and only the real L3+R3 gamepad
                        // chord can open the menu.
                        if (!gamepad && event.key.key == SDLK_ESCAPE) {
                            if (settingsActive) {
                                settingsActive = false;
                                menuActive = true;
                                if (reconnectRequested) runningInner = false;
                            } else {
                                menuActive = !menuActive;
                                menuSelectedIndex = 0;
                                exitEmulationConfirm = false;
                            }
                        } else if (settingsActive && event.key.key == SDLK_UP) {
                            int count = static_cast<int>(settingsMenuItems().size());
                            settingsSelectedIndex = (settingsSelectedIndex + count - 1) % count;
                        } else if (settingsActive && event.key.key == SDLK_DOWN) {
                            int count = static_cast<int>(settingsMenuItems().size());
                            settingsSelectedIndex = (settingsSelectedIndex + 1) % count;
                        } else if (settingsActive && event.key.key == SDLK_RETURN) {
                            const std::string picked =
                                settingsMenuItems()[static_cast<size_t>(settingsSelectedIndex)];
                            if (picked.rfind("AUTO UPDATE ON LAUNCH:", 0) == 0) {
                                clientSettings.autoUpdateOnLaunch = !clientSettings.autoUpdateOnLaunch;
                                settingsSaveFailed = !saveClientSettings(clientSettingsPath, clientSettings);
                            } else if (picked.rfind("VIDEO QUALITY:", 0) == 0) {
                                cycleVideoQuality();
                            } else if (picked.rfind("TRACKPAD AS NATIVE INPUT", 0) == 0) {
                                toggleTrackpadExperiment();
                            } else if (picked.rfind("MIRROR HOST SCREEN", 0) == 0) {
                                clientSettings.mirrorHostScreen = !clientSettings.mirrorHostScreen;
                                settingsSaveFailed = !saveClientSettings(clientSettingsPath, clientSettings);
                            } else if (picked.rfind("VIDEO CODEC", 0) == 0) {
                                toggleVideoCodec();
                            } else if (picked.rfind("DEBUG OVERLAY:", 0) == 0) {
                                clientSettings.debugOverlayEnabled = !clientSettings.debugOverlayEnabled;
                                settingsSaveFailed = !saveClientSettings(clientSettingsPath, clientSettings);
                            } else if (picked == "RUN SETUP WIZARD") {
                                setupWizardRequested = true;
                                runningInner = false;
                            } else if (picked.rfind("MICROPHONE:", 0) == 0) {
                                cycleMicDevice();
                            } else if (picked.rfind("MIC:", 0) == 0) {
                                clientSettings.micMuted = !clientSettings.micMuted;
                                settingsSaveFailed = !saveClientSettings(clientSettingsPath, clientSettings);
                            } else if (picked == "BACK") {
                                settingsActive = false;
                                menuActive = true;
                                if (reconnectRequested) runningInner = false;
                            }
                        } else if (menuActive && exitEmulationConfirm) {
                            int subCount = static_cast<int>(exitEmulationItems().size());
                            if (event.key.key == SDLK_UP) {
                                exitEmulationSelectedIndex = (exitEmulationSelectedIndex + subCount - 1) % subCount;
                            } else if (event.key.key == SDLK_DOWN) {
                                exitEmulationSelectedIndex = (exitEmulationSelectedIndex + 1) % subCount;
                            } else if (event.key.key == SDLK_RETURN) {
                                const std::string picked =
                                    exitEmulationItems()[static_cast<size_t>(exitEmulationSelectedIndex)];
                                if (picked == "CANCEL") {
                                    exitEmulationConfirm = false;
                                } else {
                                    pendingEmulatorAction = (picked == "EXIT ROM")
                                                                 ? EmulatorAction_QuitSession
                                                                 : EmulatorAction_QuitApplication;
                                    pendingEmulatorActionUntilUs =
                                        SDL_GetTicksNS() / 1000 + kPendingEmulatorActionUs;
                                    exitEmulationConfirm = false;
                                    menuActive = false;
                                }
                            }
                        } else if (menuActive && event.key.key == SDLK_UP) {
                            int count = static_cast<int>(menuItems.size());
                            menuSelectedIndex = (menuSelectedIndex + count - 1) % count;
                        } else if (menuActive && event.key.key == SDLK_DOWN) {
                            int count = static_cast<int>(menuItems.size());
                            menuSelectedIndex = (menuSelectedIndex + 1) % count;
                        } else if (menuActive && event.key.key == SDLK_RETURN) {
                            const std::string& picked = menuItems[static_cast<size_t>(menuSelectedIndex)];
                            if (picked == "RESUME") {
                                menuActive = false;
                            } else if (picked == "CHANGE HOST") {
                                changeHostRequested = true;
                                runningInner = false;
                            } else if (picked == "SETTINGS") {
                                menuActive = false;
                                settingsActive = true;
                                settingsSelectedIndex = 0;
                                refreshTrackpadExperimentStatus();
                            } else if (picked == "EXIT EMULATION") {
                                exitEmulationConfirm = true;
                                exitEmulationSelectedIndex = 0;
                            } else if (picked == "EXIT") {
                                quitApp = true;
                                runningInner = false;
                            }
                        }
                        break;
                    }
                    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                        if (settingsActive) {
                            int count = static_cast<int>(settingsMenuItems().size());
                            if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                                settingsSelectedIndex = (settingsSelectedIndex + count - 1) % count;
                            } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                                settingsSelectedIndex = (settingsSelectedIndex + 1) % count;
                            } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) {
                                const std::string picked =
                                    settingsMenuItems()[static_cast<size_t>(settingsSelectedIndex)];
                                if (picked.rfind("AUTO UPDATE ON LAUNCH:", 0) == 0) {
                                    clientSettings.autoUpdateOnLaunch = !clientSettings.autoUpdateOnLaunch;
                                    settingsSaveFailed = !saveClientSettings(clientSettingsPath, clientSettings);
                                } else if (picked.rfind("VIDEO QUALITY:", 0) == 0) {
                                    cycleVideoQuality();
                                } else if (picked.rfind("TRACKPAD AS NATIVE INPUT", 0) == 0) {
                                    toggleTrackpadExperiment();
                                } else if (picked.rfind("MIRROR HOST SCREEN", 0) == 0) {
                                    clientSettings.mirrorHostScreen = !clientSettings.mirrorHostScreen;
                                    settingsSaveFailed = !saveClientSettings(clientSettingsPath, clientSettings);
                                } else if (picked.rfind("VIDEO CODEC", 0) == 0) {
                                    toggleVideoCodec();
                                } else if (picked.rfind("DEBUG OVERLAY:", 0) == 0) {
                                    clientSettings.debugOverlayEnabled = !clientSettings.debugOverlayEnabled;
                                    settingsSaveFailed = !saveClientSettings(clientSettingsPath, clientSettings);
                                } else if (picked == "RUN SETUP WIZARD") {
                                    setupWizardRequested = true;
                                    runningInner = false;
                                } else if (picked.rfind("MICROPHONE:", 0) == 0) {
                                    cycleMicDevice();
                                } else if (picked.rfind("MIC:", 0) == 0) {
                                    clientSettings.micMuted = !clientSettings.micMuted;
                                    settingsSaveFailed = !saveClientSettings(clientSettingsPath, clientSettings);
                                } else if (picked == "BACK") {
                                    settingsActive = false;
                                    menuActive = true;
                                    if (reconnectRequested) runningInner = false;
                                }
                            } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
                                settingsActive = false;
                                menuActive = true;
                                if (reconnectRequested) runningInner = false;
                            }
                        } else if (menuActive && exitEmulationConfirm) {
                            int subCount = static_cast<int>(exitEmulationItems().size());
                            if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                                exitEmulationSelectedIndex = (exitEmulationSelectedIndex + subCount - 1) % subCount;
                            } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                                exitEmulationSelectedIndex = (exitEmulationSelectedIndex + 1) % subCount;
                            } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) {
                                const std::string picked =
                                    exitEmulationItems()[static_cast<size_t>(exitEmulationSelectedIndex)];
                                if (picked == "CANCEL") {
                                    exitEmulationConfirm = false;
                                } else {
                                    pendingEmulatorAction = (picked == "EXIT ROM")
                                                                 ? EmulatorAction_QuitSession
                                                                 : EmulatorAction_QuitApplication;
                                    pendingEmulatorActionUntilUs =
                                        SDL_GetTicksNS() / 1000 + kPendingEmulatorActionUs;
                                    exitEmulationConfirm = false;
                                    menuActive = false;
                                }
                            } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
                                exitEmulationConfirm = false; // back to the main menu, no action taken
                            }
                        } else if (menuActive) {
                            int count = static_cast<int>(menuItems.size());
                            if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                                menuSelectedIndex = (menuSelectedIndex + count - 1) % count;
                            } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                                menuSelectedIndex = (menuSelectedIndex + 1) % count;
                            } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) {
                                const std::string& picked = menuItems[static_cast<size_t>(menuSelectedIndex)];
                                if (picked == "RESUME") {
                                    menuActive = false;
                                } else if (picked == "CHANGE HOST") {
                                    changeHostRequested = true;
                                    runningInner = false;
                                } else if (picked == "SETTINGS") {
                                    menuActive = false;
                                    settingsActive = true;
                                    settingsSelectedIndex = 0;
                                    refreshTrackpadExperimentStatus();
                                } else if (picked == "EXIT EMULATION") {
                                    exitEmulationConfirm = true;
                                    exitEmulationSelectedIndex = 0;
                                } else if (picked == "EXIT") {
                                    quitApp = true;
                                    runningInner = false;
                                }
                            } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
                                menuActive = false; // back/cancel, no action taken
                            }
                        }
                        break;
                    default:
                        break;
                }
            }

            // Held L3+R3 toggles the menu -- polled once per frame
            // (not event-driven) since it's a simultaneous-hold chord, not
            // a single button press. Must be held continuously for
            // kMenuChordHoldUs before it fires (see menuChordSinceUs's
            // declaration above for why), and won't fire again until both
            // buttons are released and re-pressed.
            bool menuChordHeld = gamepad && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_STICK) &&
                                 SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
            uint64_t nowForChordUs = SDL_GetTicksNS() / 1000;
            if (menuChordHeld) {
                if (menuChordSinceUs == 0) menuChordSinceUs = nowForChordUs;
                if (!menuChordFired && nowForChordUs - menuChordSinceUs >= kMenuChordHoldUs) {
                    if (settingsActive) {
                        settingsActive = false;
                        menuActive = false;
                        if (reconnectRequested) runningInner = false;
                    } else {
                        menuActive = !menuActive;
                        menuSelectedIndex = 0;
                        exitEmulationConfirm = false;
                    }
                    menuChordFired = true;
                }
            } else {
                menuChordSinceUs = 0;
                menuChordFired = false;
            }

            // Refresh the identity line from the real HelloAck the moment
            // a (re)connect completes (GitHub issue #28) -- authoritative
            // over whatever discovery guessed beforehand, in the rare
            // case a host's active adapter changed between the discovery
            // scan and this connection. Only on the disconnected->connected
            // edge (or a mode transition, issue #4 Phase E -- a
            // ModeChanged packet carries its own fresh identity, e.g.
            // switching from a real adapter's identity to
            // kHostControlSystemIdentity/kHostControlAdapterIdentity or
            // back), not every frame, since hostSystemIdentity()/
            // hostAdapterIdentity() each take a mutex; sessionSystemName/
            // sessionAdapterName are deliberately left alone otherwise
            // (see their declaration above for why they survive a drop).
            bool nowConnected = net.isConnected();
            HostMode nowHostMode = net.hostMode();
            // Real user report, 2026-08-03: "still limited by resolution" --
            // the SDL_EVENT_MOUSE_MOTION handler above accumulates
            // event.motion.xrel/yrel into hostControlMouseDeltaX/Y, but
            // without this, xrel/yrel come from a REAL OS cursor confined to
            // this window/the Deck's own screen -- once that cursor
            // physically hits an edge, it can't move further, so no more
            // motion ever fires no matter how far the actual input device
            // keeps moving. This is an SDL feature built for exactly this
            // ("an FPS wouldn't want the player's look-motion to stop as the
            // mouse hits the edge of the window" -- SDL_mouse.h's own
            // wording), not something specific to the touchpad-vs-Steam-
            // Input investigation those other entries are chasing: relative
            // mouse mode hides the OS cursor and keeps reporting motion
            // deltas indefinitely regardless of screen edges, independent of
            // whatever underlying device is generating them (the Deck's own
            // trackpad already drives an OS cursor exactly like this even
            // without any of the SDL_EVENT_GAMEPAD_TOUCHPAD_* work). Only
            // meaningful in Host Control mode -- Emulation mode's DS-
            // touchscreen-via-mouse-drag feature (mapPointToDSCoords below)
            // needs the real absolute cursor position, which relative mode
            // would break.
            bool wantRelativeMouseMode = nowConnected && nowHostMode == HostMode::HostControl;
            if (wantRelativeMouseMode != wasRelativeMouseMode) {
                SDL_SetWindowRelativeMouseMode(window, wantRelativeMouseMode);
                wasRelativeMouseMode = wantRelativeMouseMode;
            }
            if (nowConnected && (!wasConnected || nowHostMode != lastHostMode)) {
                SystemIdentity hostSystem = net.hostSystemIdentity();
                AdapterIdentity hostAdapter = net.hostAdapterIdentity();
                if (!hostSystem.systemName.empty()) sessionSystemName = hostSystem.systemName;
                if (!hostAdapter.adapterName.empty()) sessionAdapterName = hostAdapter.adapterName;
                if (!hostSystem.systemId.empty()) sessionSystemId = hostSystem.systemId;
                if (nowConnected && wasConnected) {
                    // A mode transition mid-session, not the initial
                    // connect (which reconnectThread already logs) --
                    // worth its own line for the same "Gaming Mode has no
                    // visible terminal, stdout is what we've got" reason
                    // as every other status line in this loop.
                    logLine("[net] host mode changed to %s\n",
                                  nowHostMode == HostMode::HostControl ? "HOST CONTROL" : "EMULATION");
                }

                // Recreate the video texture at whatever native size this
                // HelloAck actually reported, if it differs from what's
                // currently allocated -- e.g. connecting to an Azahar/3DS
                // host (320x240) after a melonDS/DS one (256x192), or vice
                // versa. (A genuine mid-session resize is also handled,
                // every frame, by the identical call further down below --
                // this one just means a fresh connection doesn't have to
                // wait a frame to show correctly-sized video.)
                resizeTextureIfNeeded(net.hostNativeWidth(), net.hostNativeHeight());
            }
            wasConnected = nowConnected;
            lastHostMode = nowHostMode;

            // Microphone capture/send (GitHub issue #2): runs every frame
            // regardless of which screen is showing -- the host keeps the
            // session running while this client-local menu overlay is up,
            // so audio shouldn't pause just because the settings screen
            // does. Opens lazily once the host's HelloAck confirms
            // support; closes immediately on disconnect so a stale device
            // isn't left open across a host switch or timeout, matching
            // issue #2's "stop capture ... when the session disconnects."
            if (net.isConnected() && net.hostMicSupported() && !micCapture.isOpen()) {
                if (!micCapture.open(clientSettings.micDeviceName)) {
                    logLine("[mic] failed to open capture device \"%s\": %s\n",
                                clientSettings.micDeviceName.c_str(), SDL_GetError());
                }
            }
            if (!net.isConnected() && micCapture.isOpen()) {
                micCapture.close();
                micPendingSamples.clear();
                micLevel = 0.0f;
            }
            if (micCapture.isOpen()) {
                micLevel = micCapture.pollSamples(micPendingSamples);
                while (micPendingSamples.size() >= kMicAudioSamplesPerPacket) {
                    // Muting still polls/levels above (the meter should
                    // reflect real input while muted, not read as "no
                    // signal") -- only the actual send to the host is
                    // skipped, per issue #2's "muting prevents microphone
                    // samples from reaching melonDS."
                    if (!clientSettings.micMuted) {
                        MicAudioFramePayload micFrame;
                        micFrame.sequence = micSequence++;
                        micFrame.clientTimestampUs = wallClockNowUs();
                        micFrame.numSamples = kMicAudioSamplesPerPacket;
                        micFrame.samples.assign(micPendingSamples.begin(),
                                                 micPendingSamples.begin() + kMicAudioSamplesPerPacket);
                        net.sendMicAudioFrame(micFrame);
                    }
                    micPendingSamples.erase(micPendingSamples.begin(),
                                             micPendingSamples.begin() + kMicAudioSamplesPerPacket);
                }
            }

            if (settingsActive) {
                renderPauseMenu(renderer, settingsMenuItems(), settingsSelectedIndex, "SETTINGS",
                                settingsSaveFailed ? "COULD NOT SAVE SETTINGS" : "",
                                net.hostMicSupported() ? micLevel : -1.0f);
                continue;
            }

            if (menuActive) {
                if (exitEmulationConfirm) {
                    renderPauseMenu(renderer, exitEmulationItems(), exitEmulationSelectedIndex, "EXIT EMULATION");
                } else {
                    renderPauseMenu(renderer, menuItems, menuSelectedIndex, "MENU", "", -1.0f, identityLine());
                }
                continue;
            }

            uint64_t nowUs = SDL_GetTicksNS() / 1000;
            if (nowUs - lastInputSendUs >= inputIntervalUs) {
                ControllerState state;
                state.sequence = sequence++;
                state.clientTimestampUs = wallClockNowUs();
                // Stick-as-alternate-D-pad (spec 7.3) only makes sense for
                // a system with no analog stick of its own -- DS is the
                // only one of those. Every other system's stick is sent
                // as real analog data below instead (never both at once
                // for one physical motion -- see buildButtonsFromGamepad's
                // comment); an explicit allow-list rather than excluding
                // "3ds" specifically, so a future system (e.g. Wii U,
                // whose GamePad has two real analog sticks already)
                // doesn't inherit DS's convenience by accident.
                state.dsButtons = buildButtonsFromGamepad(gamepad, sessionSystemId == "nds");

                // Real analog stick data (protocol.h's leftStickX/Y,
                // rightStickX/Y) -- always sent regardless of session
                // system; host/adapter_bridge.cpp forwards it straight
                // into GenericInputState for whichever adapter is
                // connected, and an adapter with no analog input (DS)
                // simply never reads it. SDL's gamepad Y axis is positive
                // = down; negated here to match the positive = up
                // convention src/core/hle/service/hid/hid.cpp's
                // GetStickDirectionState() uses for the 3DS circle pad
                // (confirmed by reading it, not assumed) -- X needs no
                // flip since positive = right agrees on both sides.
                if (gamepad) {
                    state.leftStickX = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
                    state.leftStickY = negateStickAxis(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY));
                    state.rightStickX = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
                    state.rightStickY = negateStickAxis(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY));

                    // Host-control-mode-only (protocol v12): analog
                    // triggers and thumbstick clicks -- no DS/3DS/Wii U
                    // game reads these (see protocol.h's ExtraButton/
                    // leftTrigger/rightTrigger comments), but
                    // host::HostControlAdapter's virtual gamepad wants
                    // them. Real user report, 2026-08-03 (Steam Controller
                    // Tester): "bumpers register, but Triggers do not
                    // work. same with stick clicking." SDL's trigger axes
                    // are 0..32767 (unsigned, unlike the sticks' signed
                    // range) -- scaled down to the wire's 0..255 range.
                    state.leftTrigger = static_cast<uint8_t>(
                        std::clamp(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER), int16_t{0},
                                   int16_t{32767}) / 128);
                    state.rightTrigger = static_cast<uint8_t>(
                        std::clamp(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), int16_t{0},
                                   int16_t{32767}) / 128);
                    // Individual clicks only -- the L3+R3 *combo* is still
                    // reserved client-side for the menu chord (see
                    // kMenuChordHoldUs's comment); a lone click of either
                    // stick is never part of that chord and is safe to
                    // forward every tick.
                    uint8_t extraButtons = 0;
                    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_STICK)) {
                        extraButtons |= ExtraButton_ThumbLeft;
                    }
                    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) {
                        extraButtons |= ExtraButton_ThumbRight;
                    }
                    state.extraButtons = extraButtons;
                }
                // See pendingEmulatorAction's declaration above for why this
                // resends for a window instead of just the one packet that
                // set it.
                if (pendingEmulatorActionUntilUs != 0 && nowUs < pendingEmulatorActionUntilUs) {
                    state.emulatorActions = pendingEmulatorAction;
                } else {
                    state.emulatorActions = 0;
                    pendingEmulatorAction = 0;
                    pendingEmulatorActionUntilUs = 0;
                }
                state.touchActive = touchActive ? 1 : 0;
                state.touchX = touchX;
                state.touchY = touchY;
                // Host-control mouse (see hostControlMouseDeltaX/Y's
                // declaration above): clamped rather than silently
                // truncated by the int16_t cast, so an unusually large
                // single-tick delta (e.g. a fast trackpad flick during a
                // frame hitch) saturates instead of wrapping around to a
                // motion in the wrong direction. Deltas reset to 0 after
                // every send regardless of whether this packet actually
                // reaches the host -- see ControllerState::mouseDeltaX's
                // own comment on why a dropped packet's motion is simply
                // lost, not retried.
                state.mouseDeltaX = static_cast<int16_t>(
                    std::clamp(hostControlMouseDeltaX, -32768, 32767));
                state.mouseDeltaY = static_cast<int16_t>(
                    std::clamp(hostControlMouseDeltaY, -32768, 32767));
                {
                    // Polled (matching buildButtonsFromGamepad()/the stick
                    // reads above), not event-driven like the mouse-button
                    // path that sets hostControlMouseButtons itself --
                    // composed here rather than folded into that
                    // accumulator so a real desktop mouse's left click
                    // (SDL_EVENT_MOUSE_BUTTON_DOWN/UP) keeps working
                    // independently of whatever this reads. SDL_GAMEPAD_
                    // BUTTON_TOUCHPAD is the same "touchpad was pressed
                    // down" button PS4/PS5 controllers report, which Steam
                    // Input passes through the same way it does the
                    // SDL_EVENT_GAMEPAD_TOUCHPAD_* motion events above.
                    uint8_t polledMouseButtons = hostControlMouseButtons;
                    if (gamepad && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_TOUCHPAD)) {
                        polledMouseButtons |= MouseButton_Left;
                    }
                    state.mouseButtons = polledMouseButtons;
                }
                hostControlMouseDeltaX = 0;
                hostControlMouseDeltaY = 0;
                net.sendControllerState(state);
                lastInputSendUs = nowUs;
            }

            // GitHub issue #4 Phase E: while the host is in HostControl
            // mode there is normally no video to show (see
            // renderHostControlScreen()'s comment) -- ControllerState was
            // just sent above unconditionally, same as in Emulation mode,
            // so a real host's HostControlAdapter is already receiving
            // input; only the on-screen presentation differs. Falls
            // through to the normal texture path the instant nowHostMode
            // flips back to Emulation (e.g. an adapter connects).
            //
            // Real user request, 2026-08-03: "add an option to the
            // client's host control to mirror the screen, as I cannot
            // access the TV I am testing on currently." When
            // clientSettings.mirrorHostScreen is on, skip the
            // placeholder and fall through to the exact same video
            // decode/render path Emulation mode already uses below --
            // no new logic needed there at all, since a host with
            // DUALDECK_HOSTCONTROL_MIRROR_SCREEN set sends real
            // VideoFrame packets over the same wire path regardless of
            // mode (see host_control_adapter.cpp's getLatestFrame()).
            // If the host isn't actually mirroring (env var unset, or
            // no usable X11 display there), net.getLatestFrame() below
            // just keeps returning false and the built-in test-pattern
            // texture shows instead -- harmless, if not especially
            // informative; good enough for this experiment's first cut.
            if (nowConnected && nowHostMode == HostMode::HostControl && !clientSettings.mirrorHostScreen) {
                renderHostControlScreen(renderer, identityLine());
                continue;
            }

            // Real bug this fixes: a genuine mid-session resize (see
            // resizeTextureIfNeeded()'s declaration comment) only becomes
            // visible once a new-sized frame has actually been decoded --
            // checking every frame here, not just on the connect edge
            // above, means it's picked up the moment it happens instead of
            // silently falling back to the test pattern below forever
            // (frame.size() would never again match testPattern.size()
            // otherwise).
            if (nowConnected) {
                resizeTextureIfNeeded(net.hostNativeWidth(), net.hostNativeHeight());
            }

            // Only touch the texture (copy the decoded frame out of
            // NetClient and upload it to the GPU) when something that
            // would actually change its pixels has happened since the
            // last iteration: a new decoded frame, a connect/disconnect
            // edge, or a resize -- see videoTextureNeverUpdated's own
            // comment above. Every other iteration (the common case: this
            // uncapped loop running far faster than new video frames
            // arrive) reuses whatever the texture already holds.
            // latestFrameGeneration() is a plain atomic load -- cheap
            // enough to read unconditionally every iteration just to check
            // it, unlike getLatestFrame()'s full-vector copy below, which
            // is exactly the cost this whole block exists to avoid paying
            // redundantly.
            bool nowVideoTextureConnected = nowConnected;
            uint64_t nowVideoTextureGeneration = net.latestFrameGeneration();
            bool videoTextureNeedsUpdate =
                videoTextureNeverUpdated || nowVideoTextureConnected != lastVideoTextureConnected ||
                nowVideoTextureGeneration != lastVideoTextureGeneration ||
                textureWidth != lastVideoTextureWidth || textureHeight != lastVideoTextureHeight;
            if (videoTextureNeedsUpdate) {
                videoTextureNeverUpdated = false;
                lastVideoTextureConnected = nowVideoTextureConnected;
                lastVideoTextureGeneration = nowVideoTextureGeneration;
                lastVideoTextureWidth = textureWidth;
                lastVideoTextureHeight = textureHeight;

                const uint8_t* pixels = testPattern.data();
                if (nowVideoTextureConnected && net.getLatestFrame(frame) &&
                    frame.size() == testPattern.size()) {
                    pixels = frame.data();
                }
                SDL_UpdateTexture(texture, nullptr, pixels, textureWidth * 4);
            }

            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);

            SDL_FRect dst{static_cast<float>(dsRect.x), static_cast<float>(dsRect.y),
                          static_cast<float>(dsRect.width), static_cast<float>(dsRect.height)};
            SDL_RenderTexture(renderer, texture, nullptr, &dst);

            // Only while genuinely connected -- see renderDebugOverlay()'s
            // own top comment for why a stale stat stack during a
            // reconnect would be actively misleading rather than just
            // uninformative.
            if (net.isConnected() && clientSettings.debugOverlayEnabled) {
                renderDebugOverlay(renderer, net, clientSettings.videoQuality);
            }

            // Otherwise a failed/retrying connection just looks like a stuck
            // dark test-pattern screen with no indication anything is even
            // trying -- the actual retry attempts only show up in stdout,
            // which Gaming Mode has no visible terminal for (same reasoning
            // as the discovery screen using the bitmap font). Distinguishing
            // ApprovalRequired/AppVersionMismatch specifically tell the user
            // where to look/what to do -- there's nothing to do for
            // ApprovalRequired but wait (a human needs to approve on the
            // host), while AppVersionMismatch means retrying forever won't
            // help until one side is updated to match the other.
            if (!net.isConnected()) {
                std::string status;
                switch (net.lastRejectReason()) {
                    case HelloRejectReason::ApprovalRequired:
                        status = "WAITING FOR APPROVAL ON HOST " + netConfig.hostAddress + "...";
                        break;
                    case HelloRejectReason::AppVersionMismatch:
                        status = "VERSION MISMATCH WITH " + netConfig.hostAddress + " (HOST IS " +
                                  net.hostAppVersion() + ") - UPDATE TO MATCH";
                        break;
                    case HelloRejectReason::AppVersionMismatchUpdateTriggered:
                        // See wizardConnectAndApprove()'s identical case's
                        // own comment.
                        status = "HOST " + netConfig.hostAddress + " (" + net.hostAppVersion() +
                                  ") IS UPDATING ITSELF - RETRYING...";
                        break;
                    case HelloRejectReason::ProtocolVersionMismatch:
                        // See wizardConnectAndApprove()'s identical case for
                        // why net.hostAppVersion() is safe to show here even
                        // though the handshake was rejected.
                        status = "PROTOCOL VERSION MISMATCH WITH " + netConfig.hostAddress +
                                  " (HOST IS " + net.hostAppVersion() + ") - UPDATE THE APP OR HOST";
                        break;
                    // The host requires a shared secret for this mode (3DS/
                    // host-control) and this client's --auth-token doesn't
                    // match it (or wasn't given one at all) -- retrying
                    // forever won't help, unlike a genuine network hang,
                    // which this would otherwise look identical to (GitHub
                    // issue: "stuck on connecting" turned out to be this).
                    case HelloRejectReason::AuthenticationFailed:
                        status = "AUTHENTICATION FAILED WITH " + netConfig.hostAddress +
                                  " - CHECK YOUR --auth-token";
                        break;
                    case HelloRejectReason::HostBusy:
                        status = "HOST " + netConfig.hostAddress + " IS BUSY - RETRYING...";
                        break;
                    default:
                        status = "CONNECTING TO " + netConfig.hostAddress + "...";
                        break;
                }
                renderCenteredBitmapText(renderer, status, 24.0f, 2, SDL_Color{220, 200, 80, 255});
                // Last known emulated system/adapter (GitHub issue #28),
                // if any is known yet -- lets the user confirm which
                // session they're waiting to get back to while
                // reconnecting, without waiting for a fresh handshake.
                std::string lastIdentity = identityLine();
                if (!lastIdentity.empty()) {
                    renderCenteredBitmapText(renderer, lastIdentity, 44.0f, 2, SDL_Color{150, 170, 210, 255});
                }
                renderCenteredBitmapText(renderer, kMenuComboHint, 64.0f, 2, SDL_Color{140, 140, 140, 255});
            }

            SDL_RenderPresent(renderer);
        }

        shuttingDown = true;
        reconnectThread.join();
        net.disconnect();

        if (changeHostRequested) {
            logLine("[menu] changing host -- returning to discovery\n");
        }

        if (setupWizardRequested) {
            logLine("[menu] launching setup wizard\n");
            bool wizardCompleted = runSetupWizard(window, renderer, texture, gamepad, discoveryPort, netConfig,
                                                   discoveryStorePath);
            // Unlike the automatic first-run case, a cancelled re-invocation
            // from this menu should not quit the whole app -- just fall
            // through to the normal discovery screen on the next iteration,
            // same as "CHANGE HOST".
            if (wizardCompleted) markSetupComplete(wizardStatePath);
        }
    }

    if (gamepad) SDL_CloseGamepad(gamepad);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
