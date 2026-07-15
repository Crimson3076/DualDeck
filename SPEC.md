# Project Scope: melonDS Remote Dual-Screen Client for Steam Deck

## 1. Project Summary
Develop a Linux-focused system that allows Nintendo DS games to run through melonDS on a Bazzite HTPC while using a Steam Deck as the complete handheld controller and lower display.
The HTPC will:
* Run the melonDS emulator
* Display the Nintendo DS top screen on the connected television
* Output game audio through the HTPC
* Stream the Nintendo DS bottom screen to the Steam Deck
* Receive controller, touchscreen, and emulator-control input from the Steam Deck
The Steam Deck will:
* Display the Nintendo DS bottom screen
* Provide D-pad, face button, shoulder button, Start, and Select input
* Provide touchscreen input mapped directly to the DS touchscreen
* Provide optional analog-stick mappings
* Provide configurable emulator shortcuts
* Connect and reconnect with minimal user interaction
The finished experience should resemble using a Wii U GamePad, with the television acting as the top DS screen and the Steam Deck acting as the bottom screen and primary controller.
---
# 2. Primary Goal
Create a reliable proof of concept that provides:
1. The melonDS top screen on the HTPC television
2. The melonDS bottom screen on the Steam Deck
3. Full Nintendo DS controls from the Steam Deck
4. Direct Steam Deck touchscreen support
5. Low-latency communication over a local network
6. Automatic button release and safe cleanup when disconnected
7. A simple launch process suitable for Steam Gaming Mode
The initial implementation should prioritize correctness, low latency, and maintainability over visual polish.
---
# 3. Target Platforms
## Host
Primary target:
* Bazzite
* Fedora-based Linux
* KDE Plasma
* Wayland
* AMD GPU preferred
* HTPC connected to a television
Secondary compatibility:
* Other modern Linux distributions
* Intel and Nvidia GPUs where practical
* X11 support is optional and should not block the initial release
## Client
Primary target:
* Steam Deck LCD
* Steam Deck OLED
* SteamOS Gaming Mode
* SteamOS Desktop Mode
Future compatibility:
* Other Linux handhelds
* Windows handhelds
* Android tablets
* General Linux desktop clients
Do not design the protocol in a way that permanently limits it to the Steam Deck.
---
# 4. Repository Strategy
Use a monorepo unless melonDS licensing or upstream-development requirements make separate repositories more practical.
Suggested structure:
```text
melonds-remote/
├── README.md
├── SPEC.md
├── LICENSE
├── docs/
│   ├── architecture.md
│   ├── protocol.md
│   ├── building.md
│   └── testing.md
├── host/
│   ├── melonds-patches/
│   ├── remote-server/
│   └── CMakeLists.txt
├── client/
│   ├── src/
│   ├── assets/
│   ├── flatpak/
│   └── CMakeLists.txt
├── protocol/
│   ├── include/
│   ├── src/
│   └── tests/
├── scripts/
│   ├── run-host.sh
│   ├── run-client.sh
│   └── install-dev.sh
└── tests/
```
Before implementation, inspect the current melonDS repository, build system, licensing, graphics pipeline, input system, frontend architecture, and existing networking features.
Do not assume internal class names or APIs. Base all emulator modifications on the current repository.
---
# 5. High-Level Architecture
```text
┌──────────────────────────────────────────┐
│ Bazzite HTPC                             │
│                                          │
│  melonDS emulation core                  │
│   ├── Top screen → TV                    │
│   ├── Bottom framebuffer                 │
│   ├── Audio → HTPC audio output          │
│   └── Input state                        │
│                                          │
│  Remote server                           │
│   ├── Bottom-screen frame transport      │
│   ├── Controller input receiver          │
│   ├── Touch input receiver               │
│   ├── Connection management              │
│   └── Pairing/authentication              │
└───────────────────┬──────────────────────┘
                    │ Local network
┌───────────────────▼──────────────────────┐
│ Steam Deck client                        │
│                                          │
│  Bottom-screen renderer                  │
│  SDL controller input                    │
│  Touchscreen input                       │
│  Steam Input-compatible actions          │
│  Connection and pairing UI               │
│  Emulator shortcut actions               │
└──────────────────────────────────────────┘
```
The system must not depend on:
* Sunshine
* Moonlight
* A dummy HDMI display
* A virtual desktop display
* Remote mouse emulation
* Desktop-window coordinate mapping
* Capturing a second melonDS desktop window
Those may be used for early comparison testing, but they are not the target architecture.
---
# 6. Core Technical Principles
## 6.1 Direct framebuffer access
Obtain the Nintendo DS bottom-screen image directly from melonDS rather than capturing the desktop.
The DS bottom screen has a native resolution of:
```text
256 × 192
```
The host should expose completed bottom-screen frames after rendering or compositing, using the most stable point available in the melonDS graphics pipeline.
The implementation should support both software and hardware renderer paths where practical.
## 6.2 Direct emulator input injection
Steam Deck controls must be written into melonDS through its native input system.
Do not emulate:
* Keyboard input
* Mouse clicks
* A virtual Xbox controller
* Desktop focus-based input
Remote input should use the same logical button state consumed by the emulator core or frontend input layer.
## 6.3 Full-state controller updates
The client should send the complete current controller state at a fixed interval rather than relying only on button-down and button-up events.
This prevents stuck inputs when packets are lost.
Recommended initial controller update frequency:
```text
120 Hz
```
Each packet should include:
* Sequence number
* Client timestamp
* Button bitmask
* Touch active state
* Touch coordinates
* Optional analog values
* Optional emulator-action bitmask
The host must discard old or out-of-order packets where appropriate.
## 6.4 Fail-safe disconnection
When a client disconnects or times out, the host must immediately:
* Release all DS buttons
* Release the touchscreen
* Clear analog input
* Stop any active microphone input
* Clear temporary emulator actions
No button may remain held after a network interruption.
---
# 7. Functional Requirements
## 7.1 HTPC host requirements
The host application or melonDS integration must:
* Run on Bazzite
* Start alongside melonDS or be built into melonDS
* Read the bottom-screen framebuffer
* Send bottom-screen frames to one connected client
* Receive Steam Deck controller input
* Receive DS touchscreen coordinates
* Receive emulator shortcut actions
* Detect client disconnection
* Release all input after a configurable timeout
* Display the top screen locally
* Keep game audio on the HTPC by default
* Expose connection status
* Log useful diagnostic information
* Shut down cleanly
Initial scope may support only one client at a time.
## 7.2 Steam Deck client requirements
The Steam Deck client must:
* Run in SteamOS Gaming Mode
* Run in SteamOS Desktop Mode
* Render the bottom DS screen with correct aspect ratio
* Support fullscreen operation
* Read the built-in Steam Deck controller
* Read the Steam Deck touchscreen
* Send full controller state to the host
* Convert screen touch coordinates into normalized coordinates
* Display connection state
* Detect host loss
* Reconnect automatically where safe
* Allow the user to exit through a controller-accessible interface
* Avoid requiring a keyboard or mouse during normal use
## 7.3 Nintendo DS input mapping
Required controls:
| Steam Deck control      | Nintendo DS action |
| ----------------------- | ------------------ |
| D-pad                   | DS D-pad           |
| A                       | DS A               |
| B                       | DS B               |
| X                       | DS X               |
| Y                       | DS Y               |
| L1                      | DS L               |
| R1                      | DS R               |
| Menu or assigned button | DS Start           |
| View or assigned button | DS Select          |
| Touchscreen             | DS touchscreen     |
Optional default mappings:
| Steam Deck control | Suggested action        |
| ------------------ | ----------------------- |
| Left analog stick  | Alternate DS D-pad      |
| Right analog stick | Unassigned              |
| L4                 | Load state              |
| R4                 | Save state              |
| L5                 | Screen-layout menu      |
| R5                 | Fast-forward            |
| Left trackpad      | Optional D-pad          |
| Right trackpad     | Optional stylus pointer |
All mappings should eventually be configurable through Steam Input.
The application should expose recognizable controller actions rather than requiring users to bind arbitrary keyboard keys.
## 7.4 Touchscreen mapping
The client must map Steam Deck touchscreen input directly to DS touchscreen coordinates.
The rendered DS content must preserve a 4:3 aspect ratio.
The client will normally render the 256×192 DS screen inside the Deck's 1280×800 display. This produces unused vertical space.
Touch handling must:
1. Determine the actual rendered DS image rectangle
2. Ignore touches outside that rectangle by default
3. Convert the touch position into normalized coordinates
4. Clamp valid coordinates to the DS screen
5. Send touch state and coordinates to the host
6. Convert normalized values to DS coordinates on the host
Expected DS range:
```text
X: 0–255
Y: 0–191
```
Do not map touch using the entire 1280×800 panel unless stretch mode is deliberately selected.
Support the following display modes:
* Integer-scaled or nearest-neighbor 4:3
* Aspect-correct fit
* Optional stretched fullscreen
Aspect-correct fit should be the default.
## 7.5 Emulator actions
Support a separate set of emulator commands that are not part of the DS controller state.
Initial emulator actions:
* Pause or resume
* Fast-forward hold
* Save state
* Load state
* Swap local and remote screens
* Open client menu
* Disconnect
* Quit session
Potential later actions:
* Change save-state slot
* Rewind
* Reset
* Close lid
* Toggle microphone
* Toggle FPS display
* Change scaling mode
Emulator commands must be handled separately from normal DS buttons.
---
# 8. Networking Requirements
## 8.1 General
The system is intended primarily for trusted local networks.
Initial implementation should support:
* IPv4
* Direct IP connection
* Configurable host port
* One active client
* Automatic reconnect
* Heartbeat or keepalive
* Session version negotiation
Future versions may add:
* IPv6
* mDNS discovery
* QR-code pairing
* Multiple stored hosts
## 8.2 Control channel
Use a reliable channel for:
* Pairing
* Session setup
* Capability negotiation
* Emulator commands
* Connection state
* Error messages
* Configuration changes
Acceptable options:
* TCP
* QUIC
* WebSocket over a local connection
For the first prototype, TCP is acceptable.
## 8.3 Input channel
Input should prioritize low latency.
Acceptable initial options:
* UDP input packets with sequence numbers
* QUIC datagrams
* A low-latency TCP connection if measurements show no meaningful issues
Recommended prototype:
* TCP for control
* UDP for controller and touch state
* UDP or a separate transport for video
The protocol must be documented independently from the implementation.
## 8.4 Video transport
Implement video in stages.
### Stage 1: development transport
Use a simple transport that minimizes implementation complexity.
Possible formats:
* Raw RGB or RGBA
* RGB565
* Lightweight lossless compression
* Zstandard-compressed frame data
* LZ4-compressed frame data
The native frame is small enough that raw or lightly compressed transport is acceptable for initial LAN testing.
Example uncompressed RGBA bandwidth at 60 FPS:
```text
256 × 192 × 4 × 60 = approximately 11.8 MB/s
```
This is acceptable for a prototype over a modern local network.
### Stage 2: optimized transport
After the direct-frame prototype works, evaluate:
* H.264
* H.265
* AV1
* MJPEG
* Custom tile or delta encoding
Do not begin with hardware-accelerated encoding unless it is necessary to meet latency or bandwidth goals.
Hardware encoding can create unnecessary complexity around:
* VAAPI
* Vulkan
* DMA-BUF
* GPU vendor differences
* Frame synchronization
* Packaging
Measure first, then optimize.
## 8.5 Latency target
Target total bottom-screen presentation latency:
```text
Under 50 ms on a healthy local Wi-Fi network
```
Preferred target:
```text
Under 30 ms
```
Target controller-network latency:
```text
Under 10 ms on a healthy LAN
```
The project should include timestamp-based latency instrumentation.
---
# 9. Protocol Design
Create a versioned protocol document.
Suggested session handshake:
```text
Client → Host:
- Protocol version
- Client name
- Client platform
- Display resolution
- Supported pixel formats
- Supported codecs
- Controller capabilities
- Touch capabilities
- Microphone capability
Host → Client:
- Accepted protocol version
- Session ID
- Native bottom-screen resolution
- Frame rate
- Selected pixel format or codec
- Input update rate
- Emulator capabilities
```
Suggested controller state:
```cpp
struct ControllerState {
    uint32_t magic;
    uint16_t protocolVersion;
    uint16_t packetType;
    uint32_t sequence;
    uint64_t clientTimestampUs;
    uint16_t dsButtons;
    uint16_t emulatorActions;
    int16_t leftStickX;
    int16_t leftStickY;
    int16_t rightStickX;
    int16_t rightStickY;
    uint8_t touchActive;
    uint16_t touchX;
    uint16_t touchY;
};
```
The exact binary layout may change, but it must:
* Use explicit integer sizes
* Define byte order
* Include protocol versioning
* Include packet type
* Include sequence numbers
* Avoid sending compiler-dependent raw structs without serialization
* Validate every received field
* Reject malformed packets safely
Suggested DS button bitmask:
```text
Bit 0: A
Bit 1: B
Bit 2: X
Bit 3: Y
Bit 4: Up
Bit 5: Down
Bit 6: Left
Bit 7: Right
Bit 8: L
Bit 9: R
Bit 10: Start
Bit 11: Select
```
---
# 10. Client Technology
Use SDL3 unless repository inspection identifies a strong reason not to.
SDL3 should handle:
* Steam Deck controller input
* Touchscreen input
* Window creation
* Rendering
* Controller hotplugging
* Cross-platform abstraction
* Audio capture in a later phase
Preferred language:
* C++20 for easiest integration with melonDS and SDL
* Rust is acceptable for the client if it does not complicate protocol sharing or packaging
Recommended default:
```text
Host integration: C++20
Protocol library: C++20
Steam Deck client: C++20 with SDL3
Build system: CMake
```
Use sanitizers and strict compiler warnings in development builds.
---
# 11. Host Integration Options
Investigate both options before committing.
## Option A: Native melonDS feature
Add the remote-server functionality directly to the melonDS frontend.
Advantages:
* Direct framebuffer access
* Direct input integration
* Easier lifecycle management
* Better synchronization
* Cleaner long-term experience
Disadvantages:
* Larger upstream patch
* Tighter coupling
* More difficult rebasing
* Must follow melonDS architecture and contribution standards
## Option B: Shared-library or plugin-style integration
Expose framebuffer and input hooks through a limited interface.
Advantages:
* Cleaner separation
* Easier independent development
* Potentially easier upstream discussion
Disadvantages:
* melonDS may not have a plugin architecture
* Could require invasive API changes anyway
* More complex packaging
## Option C: Maintained melonDS fork
Use a fork during proof-of-concept development.
Advantages:
* Fastest initial development
* Full control
* No dependency on immediate upstream acceptance
Disadvantages:
* Long-term maintenance burden
* Must regularly merge upstream changes
Recommended path:
1. Start with a small maintained fork
2. Keep modifications isolated
3. Document every changed melonDS subsystem
4. Avoid unrelated emulator modifications
5. Structure the changes so they can later be proposed upstream
---
# 12. User Experience
## 12.1 Desired normal launch flow
The eventual user flow should be:
1. User launches the Steam Deck client from Gaming Mode
2. Client finds or connects to the configured HTPC
3. Host starts or exposes the melonDS session
4. The TV displays the top screen
5. The Steam Deck displays the bottom screen
6. Steam Deck controls become active
7. The game is playable without desktop interaction
For the initial proof of concept, starting melonDS manually on the HTPC is acceptable.
## 12.2 Client interface
Minimum client UI:
* Host address
* Connect button
* Connection status
* Disconnect button
* Exit button
* Fullscreen bottom-screen view
* Optional on-screen latency and FPS debug overlay
All controls must be usable with the Steam Deck controller.
## 12.3 Host interface
Minimum host settings:
* Enable remote screen server
* Port
* Pairing code or authentication token
* Input timeout
* Allow remote emulator commands
* Video transport selection
* Debug logging
* Connected-client status
A command-line configuration file is acceptable for the prototype.
---
# 13. Security Requirements
Even though the target is a local network, do not accept arbitrary unauthenticated input by default in the polished release.
Prototype requirements:
* Bind to localhost or LAN interface explicitly
* Display a warning when authentication is disabled
* Validate packet sizes and values
* Reject protocol mismatches
* Rate-limit connection attempts
* Do not allow arbitrary shell command execution
* Do not accept file paths from the client
* Do not expose ROM browsing initially
Later pairing options:
* Six-digit pairing code
* Pre-shared token
* QR code
* Certificate-based pairing
The client must not be able to upload or execute arbitrary content on the host.
---
# 14. Logging and Diagnostics
Provide structured, readable logging.
Host logs should include:
* Startup
* Listening address
* Client connection
* Protocol version
* Selected transport
* Frame rate
* Dropped frames
* Input packet rate
* Out-of-order packets
* Timeout events
* Client disconnection
* Input reset
* Fatal errors
Client logs should include:
* Host connection attempt
* Handshake result
* Selected format
* Controller detection
* Touch detection
* Received frame rate
* Dropped or late frames
* Reconnect attempts
* Disconnection reason
Do not log authentication secrets.
Add an optional overlay showing:
* Network latency
* Decode or upload latency
* Render FPS
* Input packet rate
* Dropped frames
* Connection quality
---
# 15. Performance Requirements
The host integration should avoid materially reducing melonDS performance.
Target host overhead:
* Less than 5 percent average CPU overhead for raw or lightweight transport
* No persistent emulator stutter caused by networking
* No blocking network operations on the emulation thread
* No frame sending directly from the emulation thread if it can stall emulation
Use:
* Bounded frame queues
* A dedicated networking thread
* Latest-frame-wins behavior
* Frame dropping rather than emulator blocking
If the network cannot keep up, drop old frames and send the newest available frame.
Never allow an unbounded frame queue.
---
# 16. Threading Requirements
Keep emulator, networking, and client rendering responsibilities separated.
Suggested host threads:
* melonDS emulation and frontend thread
* Frame transport thread
* Input receiver thread
* Control connection thread
Shared state must be:
* Thread-safe
* Bounded
* Documented
* Free from avoidable locks on the emulation hot path
Suggested frame flow:
```text
Emulator produces completed bottom frame
             ↓
Copy or reference latest frame buffer
             ↓
Replace pending frame in one-frame queue
             ↓
Network thread reads newest frame
             ↓
Encode/compress and send
```
Input should use an atomic or lock-protected latest-state structure.
---
# 17. Packaging Requirements
## Steam Deck client
Initial deliverables:
* Build instructions
* Native executable
* Desktop file
* Steam shortcut instructions
Target polished packaging:
* Flatpak
* Controller-friendly icon and artwork
* Steam Gaming Mode launch support
* No development packages required at runtime
## Bazzite host
Initial deliverables:
* Build instructions
* Patched melonDS build
* Launch script
* Configuration example
Later packaging:
* Flatpak-compatible approach if technically feasible
* RPM or Bazzite-friendly package
* Containerization only if it does not interfere with GPU or display access
Do not make Docker a requirement for normal use.
---
# 18. Development Phases
## Phase 0: Repository investigation
Tasks:
* Clone and build current melonDS
* Identify bottom-screen framebuffer ownership
* Identify renderer-independent frame access
* Identify input-state handling
* Identify save-state and fast-forward APIs
* Identify frontend lifecycle hooks
* Document relevant files and classes
* Confirm melonDS license compatibility
* Create architecture notes
Deliverable:
```text
docs/melonds-integration-analysis.md
```
No large implementation should begin until this analysis exists.
## Phase 1: Local proof of concept
Goal:
Prove that a separate client can display the melonDS bottom screen and control the emulator.
Tasks:
* Add a minimal host server
* Extract bottom-screen frames
* Send raw frames over localhost or LAN
* Build SDL3 client
* Display received bottom frames
* Read controller input
* Send DS button state
* Send touch coordinates
* Inject input into melonDS
* Release input on disconnect
Acceptance criteria:
* A DS game runs on the HTPC
* Top screen appears on TV
* Bottom screen appears on Steam Deck
* D-pad and all primary DS buttons work
* Touchscreen works accurately
* Disconnecting releases every input
* Session remains playable for at least 30 minutes
## Phase 2: Network robustness
Tasks:
* Add versioned handshake
* Add sequence numbers
* Add heartbeat
* Add reconnect
* Add malformed-packet handling
* Add bounded queues
* Add frame dropping
* Add latency metrics
* Add configurable host address and port
* Add authentication token
Acceptance criteria:
* Client can reconnect after Wi-Fi interruption
* No stuck buttons after interruption
* No emulator crash after malformed packets
* No unbounded memory growth
* No severe stutter during packet loss
## Phase 3: Gaming Mode usability
Tasks:
* Add controller-navigable client menu
* Add fullscreen behavior
* Add persistent host configuration
* Add Steam shortcut
* Add proper icons
* Add Steam Input action names
* Add exit and disconnect actions
* Add automatic reconnect
* Add debug overlay toggle
Acceptance criteria:
* Entire client can be launched, connected, used, and closed without a keyboard or mouse
* Steam Deck controls appear correctly through Steam Input
* Touch mapping remains correct at all supported scaling modes
## Phase 4: Video optimization
Tasks:
* Measure raw transport bandwidth and latency
* Add RGB565 or lightweight compression
* Compare LZ4, Zstandard, MJPEG, and video codecs
* Select the simplest transport meeting performance goals
* Add capability negotiation
* Maintain a raw fallback mode
Acceptance criteria:
* Smooth 60 FPS bottom-screen output
* No visible queue buildup
* Preferred sub-50-ms presentation latency
* Stable performance on normal Wi-Fi
## Phase 5: Polish and upstream preparation
Tasks:
* Isolate melonDS changes
* Add settings UI
* Add documentation
* Add unit and integration tests
* Add Flatpak packaging
* Review licensing
* Prepare upstream-friendly commits
* Open relevant melonDS design discussion before a large pull request
Acceptance criteria:
* New user can follow documentation successfully
* Build is reproducible
* No known input-sticking issues
* No major emulator regressions
* Host feature can be disabled completely
---
# 19. Testing Requirements
## Unit tests
Test:
* Packet serialization
* Packet parsing
* Byte order
* Protocol version validation
* Sequence handling
* Touch coordinate mapping
* Button bitmasks
* Timeout behavior
* Frame-header parsing
* Malformed packet rejection
## Integration tests
Test:
* Host and client connection
* Controller round trip
* Touch round trip
* Client disconnect
* Host disconnect
* Reconnection
* Protocol mismatch
* Packet loss simulation
* Delayed packets
* Out-of-order packets
* Long-running session
* Emulator pause and resume
## Manual Steam Deck tests
Test on:
* Steam Deck LCD
* Steam Deck OLED, when available
* Desktop Mode
* Gaming Mode
* Wi-Fi 5
* Wi-Fi 6
* Docked and undocked Deck
* Client suspend and resume
* HTPC session restart
## Game testing
Use games that exercise different DS features:
* Touch-heavy title
* Traditional D-pad and buttons
* Fast action game
* Game using microphone
* Game with frequent screen updates
* Game with mostly static bottom screen
Do not include commercial ROMs in the repository or test artifacts.
---
# 20. Acceptance Criteria for Version 0.1
Version 0.1 is complete when all of the following are true:
1. melonDS runs a Nintendo DS game on the Bazzite HTPC.
2. The top DS screen is displayed locally on the television.
3. The bottom DS screen is displayed on the Steam Deck.
4. The Deck D-pad controls the DS D-pad.
5. Deck A, B, X, and Y control the equivalent DS buttons.
6. Deck shoulder buttons control DS L and R.
7. Start and Select are functional.
8. The Steam Deck touchscreen accurately controls the DS touchscreen.
9. Touches outside the rendered DS screen are ignored.
10. The host releases all inputs when the client disconnects.
11. The bottom screen runs at or near 60 FPS.
12. The emulator remains stable during a 30-minute session.
13. The client can run fullscreen in Gaming Mode.
14. The project contains complete local build instructions.
15. The protocol is documented.
16. No Sunshine, Moonlight, dummy display, or virtual monitor is required.
---
# 21. Explicit Non-Goals for Version 0.1
Do not implement the following unless they are required for the core proof of concept:
* ROM transfer
* Cloud saves
* Internet play
* Multiple simultaneous clients
* Public matchmaking
* User accounts
* Remote desktop
* Remote filesystem browsing
* Android client
* Windows client
* iOS client
* Microphone streaming
* Camera emulation
* Rumble emulation
* Voice chat
* Spectator mode
* Library artwork scraping
* Automatic ROM downloading
* Cheat database integration
* A custom emulator core
* Replacing melonDS rendering
* Supporting every Linux desktop environment
Keep Version 0.1 narrow.
---
# 22. Code Quality Requirements
The implementation must:
* Use clear module boundaries
* Avoid unexplained global state
* Avoid blocking emulator threads
* Use RAII for sockets and resources
* Handle errors explicitly
* Validate all network input
* Include comments for non-obvious synchronization
* Include documentation for public interfaces
* Use consistent formatting
* Compile with strict warnings
* Avoid introducing unrelated melonDS changes
* Include tests for protocol and coordinate code
Preferred compiler flags for development:
```text
-Wall
-Wextra
-Wpedantic
-Wconversion
-Wshadow
```
Use AddressSanitizer and UndefinedBehaviorSanitizer in development builds where compatible.
---
# 23. Coding LLM Operating Instructions
Follow this workflow.
## Before coding
1. Inspect the full relevant repository structure.
2. Build melonDS without changes.
3. Identify the current branch and commit.
4. Read existing contributor guidance.
5. Locate framebuffer, rendering, input, and frontend lifecycle code.
6. Produce an integration analysis.
7. Propose the smallest viable implementation.
8. Avoid guessing APIs or filenames.
## During coding
1. Make small, logically separated changes.
2. Keep the build working after each meaningful step.
3. Add tests alongside protocol code.
4. Run formatting and tests before presenting changes.
5. Record commands used to build and test.
6. Do not silently disable security or validation.
7. Do not replace major melonDS subsystems unnecessarily.
8. Do not add a heavyweight dependency without justification.
## When uncertain
Prefer:
* Inspecting existing code
* Searching repository references
* Reading upstream documentation
* Implementing a small adapter
* Documenting the uncertainty
Do not fabricate classes, functions, build targets, or configuration formats.
## After each milestone
Report:
* Files changed
* Design decisions
* Build result
* Tests run
* Known limitations
* Next recommended step
---
# 24. First Implementation Task
Begin with Phase 0 and a minimal Phase 1 skeleton.
Perform the following:
1. Inspect the current melonDS source tree.
2. Identify exactly where completed top and bottom screen frames become available.
3. Identify how DS button and touchscreen states enter the emulation core.
4. Identify whether the Qt frontend, SDL frontend, or emulator core is the best integration point.
5. Document findings in `docs/melonds-integration-analysis.md`.
6. Create a minimal versioned protocol library.
7. Add unit tests for:
   * Message headers
   * Controller serialization
   * Touch coordinate serialization
   * Malformed packet rejection
8. Create an SDL3 client that:
   * Opens a 1280×800 window
   * Displays a test 256×192 frame
   * Preserves 4:3 aspect ratio
   * Reads controller state
   * Reads touchscreen state
   * Shows values in a debug overlay or logs
9. Do not modify melonDS input behavior until the integration analysis is complete.
10. Present the proposed melonDS patch boundary before implementing invasive changes.
---
# 25. Deliverables
Required project deliverables:
* Working source code
* melonDS integration patch or maintained fork
* Steam Deck client
* Shared protocol implementation
* Unit tests
* Build instructions
* Bazzite host setup instructions
* Steam Deck setup instructions
* Protocol documentation
* Architecture documentation
* Troubleshooting guide
* Known-limitations document
* License and attribution notices
---
# 26. Definition of Success
The project succeeds when a user can sit at the television, launch a Nintendo DS game on the Bazzite HTPC, hold the Steam Deck as the controller and touchscreen, and play without interacting with the HTPC desktop.
The television must show the DS top screen. The Steam Deck must show the DS bottom screen and provide all normal DS controls. The experience must not depend on treating the Steam Deck as a desktop monitor or remote mouse.
