# Wire Protocol (v2)

This document is the authoritative description of the wire format
implemented in `protocol/`. It intentionally covers only what is
implemented today; features described only as "suggested" in `SPEC.md`
section 9 but not yet built are marked as such below.

All integers are **little-endian** on the wire. All packets begin with the
same fixed-size header.

## Packet header (12 bytes)

| Offset | Size | Field            | Notes                                   |
|-------:|-----:|------------------|------------------------------------------|
| 0      | 4    | `magic`          | Always `0x444D5231` ("DMR1"). Packets with any other value are rejected before further parsing. |
| 4      | 2    | `protocolVersion`| Currently `10` (bumped from `9` when `VideoFrame`'s payload gained an 8-byte host capture timestamp prefix for video-latency instrumentation -- see "Video payload" below; `9` itself had bumped from `8` when `HelloPayload.videoQuality` was added, letting the client request its own session's JPEG quality instead of always getting the host's one configured default -- see "Hello payload"/"Video payload" below; `8` itself had bumped from `7` when `VideoFrame`'s payload changed from a raw pixel buffer to a JPEG-compressed image -- see "Video payload" below; `7` itself had bumped from `6` when `HelloAckPayload.mode` was added, GitHub issue #4 Phase E -- see "HelloAck payload" below; `6` itself had bumped from `5` when `HelloAckPayload.system`/`.adapter` and `DiscoveryResponsePayload.system`/`.adapter` were added for the emulator identity model, GitHub issue #28 -- see "Emulator identity model" below; `5` itself had bumped from `4` when `HelloAckPayload.micSupported` and `DiscoveryResponsePayload.audioPort` were added for microphone support, GitHub issue #2 -- see "MicAudioFrame payload" below; `4` itself had bumped from `3` when `HelloPayload.appVersion`/`HelloAckPayload.appVersion` were added and `HelloRejectReason::AppVersionMismatch` was introduced -- see "App version mismatch" below; `3` itself had bumped from `2` when `HelloAckPayload.pairingToken` was removed and `HelloRejectReason::PairingRequired` was renamed to `ApprovalRequired`, moving from a typed-code pairing flow to device-approval; `2` itself had bumped from `1` when those pairing-code fields were first added). A mismatch is rejected by the receiver; it is not itself a fatal error for the connection. |
| 6      | 2    | `packetType`     | See table below.                        |
| 8      | 4    | `payloadSize`    | Size of the payload that follows, in bytes. Receivers must verify this matches the number of bytes actually available before parsing the payload. |

## Packet types

| Value | Name              | Direction       | Channel | Payload                                   |
|------:|-------------------|-----------------|---------|--------------------------------------------|
| 1     | `Hello`           | client -> host  | TCP control | see "Hello payload" below |
| 2     | `HelloAck`        | host -> client  | TCP control | see "HelloAck payload" below |
| 3     | `ControllerState` | client -> host  | UDP input | see "ControllerState payload" below |
| 4     | `Heartbeat`       | either           | TCP control | none |
| 5     | `Disconnect`      | either           | TCP control | none |
| 6     | `EmulatorAction`  | client -> host  | TCP control | not yet implemented; emulator actions currently ride inside `ControllerState.emulatorActions` |
| 7     | `VideoFrame`      | host -> client  | TCP video | JPEG-compressed image, see "Video payload" below |
| 8     | `DiscoveryRequest`  | client -> host (UDP broadcast) | discovery | none |
| 9     | `DiscoveryResponse` | host -> client (UDP unicast)   | discovery | see "Discovery payload" below |
| 10    | `MicAudioFrame`   | client -> host  | UDP audio | see "MicAudioFrame payload" below |
| 11    | `ModeChanged`     | host -> client  | TCP control | see "ModeChanged payload" below |
| 12    | `ClientLog`       | client -> host  | TCP control | see "ClientLog payload" below |

## ControllerState payload (29 bytes)

Sent by the client at a fixed rate (recommended 120 Hz, spec section 6.3)
regardless of whether any input changed, so a lost packet cannot leave a
button stuck.

| Offset | Size | Field              | Notes |
|-------:|-----:|--------------------|-------|
| 0      | 4    | `sequence`         | Monotonically increasing per client session; wraps at 2^32. Used by `InputStateTracker` to discard old/out-of-order packets. |
| 4      | 8    | `clientTimestampUs`| Client-side capture time as **wall-clock (epoch) microseconds** (`std::chrono::system_clock`), not a monotonic/ticks-since-start value -- the host computes an approximate one-way latency as `hostWallClockNow - clientTimestampUs` for the periodic stats log (spec section 8.5), which only makes sense if client and host clocks are reasonably synced (e.g. NTP). The host does not reject a packet over this field's value; implausible deltas (host time before the timestamp, or a gap over 10s) are just excluded from the latency stats rather than treated as a validation failure. |
| 12     | 2    | `dsButtons`        | Bitmask, 1 = pressed. See "DS button bits" below. |
| 14     | 2    | `emulatorActions`  | Bitmask, 1 = active this packet. See "Emulator action bits" below. |
| 16     | 2    | `leftStickX`       | Signed, -32768..32767. |
| 18     | 2    | `leftStickY`       | Signed. |
| 20     | 2    | `rightStickX`      | Signed. |
| 22     | 2    | `rightStickY`      | Signed. |
| 24     | 1    | `touchActive`      | 0 or 1. Any other value makes the whole packet malformed and it is dropped. |
| 25     | 2    | `touchX`           | 0..255. Only validated when `touchActive == 1`; ignored otherwise. |
| 27     | 2    | `touchY`           | 0..191. Only validated when `touchActive == 1`. |

`kControllerStateWireSize` in `protocol/include/melonds_remote/protocol.h`
is defined as this same sum (29 bytes) and is checked by
`protocol_tests` (`test_controller_state.cpp`); treat the header as the
source of truth if this document and the code ever disagree.

### DS button bits (`dsButtons`)

This is the **wire** bit order, chosen independently of melonDS's internal
`KEYINPUT` register layout (which is active-low and ordered
A,B,Select,Start,Right,Left,Up,Down,R,L,X,Y -- see
`docs/melonds-integration-analysis.md` section 2). The host adapter is
responsible for translating between the two; that translation is a small
table, not part of the wire format.

| Bit | Button |
|----:|--------|
| 0   | A      |
| 1   | B      |
| 2   | X      |
| 3   | Y      |
| 4   | Up     |
| 5   | Down   |
| 6   | Left   |
| 7   | Right  |
| 8   | L      |
| 9   | R      |
| 10  | Start  |
| 11  | Select |

### Emulator action bits (`emulatorActions`)

| Bit | Action           |
|----:|------------------|
| 0   | Pause/Resume     |
| 1   | Fast-forward (hold) |
| 2   | Save state       |
| 3   | Load state       |
| 4   | Swap screens     |
| 5   | Open client menu |
| 6   | Disconnect       |
| 7   | Quit session (eject ROM) |
| 8   | Quit application (close melonDS entirely, GitHub issue #25) |

## Video payload

Protocol v10: the payload is an 8-byte host wall-clock capture
timestamp (`VideoFramePayload::captureTimestampUs`, little-endian
microseconds since the Unix epoch) immediately followed by the JPEG-
compressed image itself (protocol v8, described below, unchanged in
shape). Added for video-latency instrumentation: the client compares
this against its own wall-clock receipt time to estimate network +
encode + send-queue latency for the video path, the same "assumes
synced clocks" caveat `ControllerState.clientTimestampUs`'s existing
input-latency estimate already carries (see `docs/known-limitations.md`).
The timestamp is taken host-side (`net_server.cpp`'s `videoLoop()`)
immediately before this frame's JPEG encoding begins, so the client-
computed latency includes encode time, not just network transit. This
is a genuine payload-shape change (unlike `ModeChanged`/`ClientLog`
above, which are brand new packet types an older peer can just ignore)
-- it required bumping `protocolVersion` because an old client reading
a new-format frame would misinterpret the leading timestamp bytes as
JPEG data, and a new client reading an old-format frame would
misinterpret the JPEG's own leading bytes as a timestamp.

Protocol v8: the JPEG portion is a JPEG-compressed image (libjpeg-turbo's
turbojpeg API), one frame's worth of whatever pixel dimensions the
connected adapter's `frameDimensions()` reports (see the "Emulator
identity model" section below for how a non-DS-sized adapter's
dimensions reach the client). The overall `payloadSize` (timestamp plus
JPEG) varies frame to frame with scene content, unlike every fixed-size
payload elsewhere in this protocol -- receivers must not assume a fixed
expected size the way older versions of this protocol's client did.

Quality defaults to `NetServerConfig::videoJpegQuality` (80), but
protocol v9's `HelloPayload::videoQuality` (see "Hello payload" above)
lets the connecting client request a different value for its own
session -- added after a single host-wide default turned out to
over-compress DS/3DS, whose small frame sizes have bandwidth to spare
that a large, often link-constrained Cemu session would not. At a
requested quality of 90 or higher, chroma subsampling switches from
4:2:0 to 4:4:4 (`net_server.cpp`'s `compressFrameBgraToJpeg()`) so a
"maximum quality" request isn't still capped by subsampling artifacts on
sharp pixel-art edges/text -- the quality scalar alone doesn't control
that.

**Before v7 (raw pixel buffer, retained here for history)**: the payload
used to be an uncompressed pixel buffer, `width * height * 4` bytes,
**B,G,R,X bytes in memory, in that order** (byte 0 = blue, 1 = green, 2 =
red, 3 = unused/alpha) -- matching melonDS's software renderer output
directly. Fine for DS's 256x192 or 3DS's 320x240 surfaces, but
increasingly unworkable for anything larger (Cemu's 854x480 GamePad
surface needs ~788 Mbps uncompressed at 60fps) on a real, bandwidth-
constrained link -- see `docs/known-limitations.md`'s 2026-07-22 JPEG
compression entry for the full story. `NetServer::videoLoop()`
(`host/remote-server/src/net_server.cpp`) compresses every frame with
this B,G,R,X-ordered raw buffer as turbojpeg's `TJPF_BGRA` input, and
`NetClient::videoReceiveLoop()` (`client/src/net_client.cpp`) decodes
back to the exact same B,G,R,X byte order via `TJPF_BGRA` as the output
format -- so everything below this point (the SDL texture format note,
`client/src/main.cpp`'s texture upload) is unchanged from before
compression, since that layer only ever sees the decoded raw buffer.

One real side effect of JPEG having no alpha channel at all: the fourth
(X/alpha) byte of every decoded pixel always comes back `0xFF`, whatever
the original byte was. This is harmless here since alpha in this
protocol has never carried real transparency data and the client never
reads it for blending (`SDL_SetTextureBlendMode(..., SDL_BLENDMODE_NONE)`
in `client/src/main.cpp`), but would matter if that byte were ever put to
real use.

**On the client, the decoded buffer is uploaded as `SDL_PIXELFORMAT_BGRA32`,
not `SDL_PIXELFORMAT_BGRA8888`.** SDL names its 32-bit "packed" formats
(`..._8888`) after a bit layout read MSB-to-LSB of the pixel as a single
integer, not a byte order in memory -- on a little-endian machine (the
common case, including Steam Deck) `SDL_PIXELFORMAT_BGRA8888` actually
means the same in-memory byte order as `SDL_PIXELFORMAT_ARGB8888`, a
completely different order than this wire format uses. Only the `_32`
suffixed aliases (`SDL_PIXELFORMAT_BGRA32` etc.) are defined by SDL to
always mean "bytes in memory match the name," regardless of host
endianness -- see `SDL_pixels.h`. Using the packed name instead of the
`_32` alias here was a real bug (client screen colors visibly wrong --
confirmed by feeding a solid-red B,G,R,X buffer through both constants
and reading back the actually-rendered color: `BGRA8888` displayed it as
black, `BGRA32` displayed it correctly as red), now fixed in
`client/src/main.cpp`.

## Discovery payload

Implements spec section 8.1's "future versions" LAN-discovery item. A
separate UDP socket from the control/input/video channels above, bound to
port `8763` by default (`--discovery-port` on the host, matching client
flag on the client), so it can be turned off (`--no-discovery`) without
touching the real ports at all.

`DiscoveryRequest` has no payload -- the client broadcasts a bare packet
with this type to `255.255.255.255:<discoveryPort>` and collects whatever
`DiscoveryResponse` replies arrive within a short window (see
`client/src/discovery_client.h`).

`DiscoveryResponse` payload (8 fixed bytes + `hostName` plus 5 more length-prefixed strings for identity):

| Offset | Size | Field          | Notes |
|-------:|-----:|----------------|-------|
| 0      | var. | `hostName`     | length-prefixed string, same encoding as Hello's strings. Defaults to `gethostname()` if the host wasn't given `--host-name`. |
| var.   | 2    | `controlPort`  | |
| var.   | 2    | `inputPort`    | |
| var.   | 2    | `videoPort`    | |
| var.   | 2    | `audioPort`    | UDP port for `MicAudioFrame` packets (default 8765), added in protocol v5. Advertised here regardless of whether the host actually accepts audio -- see `HelloAckPayload.micSupported` below for that. |
| var.   | var. | `system.systemId`, `system.systemName`, `adapter.adapterId`, `adapter.adapterName`, `adapter.adapterVersion` | Five length-prefixed strings, added in protocol v6 (GitHub issue #28) -- see "Emulator identity model" below. |

Deliberately unauthenticated: discovery only reveals a host name and three
port numbers, never bypasses the device-approval/token check on the
actual control connection (see "Authentication and device approval"
below), so there is no security benefit to gating it. This is also why
the host's discovery listener is the one socket in this project that
binds `0.0.0.0` rather than a specific configured address -- see the doc
comment on `NetServerConfig::discoveryEnabled` in
`host/remote-server/include/host/net_server.h`.

Unlike every other packet type, `DiscoveryRequest`/`DiscoveryResponse`
deliberately do **not** enforce `header.protocolVersion == kProtocolVersion`
on either side (real-usage bug: a client and host that auto-updated at
different times ended up on different protocol versions, and the exact-
match check made a well-formed request/response from "the wrong"
version indistinguishable from noise -- the client's host list silently
showed nothing, with no error anywhere). `DiscoveryResponsePayload`'s
wire format is stable across every version bump so far, so this is safe:
a mismatched host still shows up in the list, and attempting to connect
to it still correctly surfaces `AppVersionMismatch` from the Hello/
HelloAck handshake described below -- discovery's only job is "is there
a host here to try," not compatibility enforcement.

The client shows the discovered-host list on every launch (see
`docs/architecture.md` "Client" section) -- it never auto-connects
silently, even when only one host answers, so switching to a different
HTPC is always available. The previously-picked host is pre-highlighted
as the default selection for a quick one-button reconnect, and the list
keeps rescanning live while shown so a host that finishes booting a few
seconds late still appears.

## MicAudioFrame payload

Implements GitHub issue #2 (microphone support): the client captures
audio locally and streams it to the host over its own UDP socket
(`audioPort`, see "Discovery payload" above), separate from the
`ControllerState` input socket, so a burst of larger audio payloads
never delays input packets or vice versa. Only sent once the host's
`HelloAck` reported `micSupported == 1` and the user hasn't muted --
absence of packets is the "no audio" state, the same convention
`ControllerState`'s own timeout-based release already uses.

Fixed format: mono, 16-bit signed PCM, little-endian, at a single fixed
rate (`kMicAudioSampleRate` = 48000 Hz) rather than negotiated per
client -- this matches melonDS's own default local-microphone capture
rate, so the host side can resample it into the DS's actual mic
consumption rate using melonDS's existing local-mic pipeline, not a
separate one (see `docs/known-limitations.md`'s microphone section for
why). `kMicAudioSamplesPerPacket` (480, i.e. 10ms per packet) bounds how
many samples one packet may declare -- a well-behaved client always
sends exactly this many except possibly a shorter final packet right
before muting.

| Offset | Size | Field         | Notes |
|-------:|-----:|---------------|-------|
| 0      | 4    | `sequence`    | Monotonically increasing per client session; informational (ordering isn't enforced the way `ControllerState`'s is -- losing an audio packet is an audible gap, not something to discard as "stale"). |
| 4      | 8    | `clientTimestampUs` | Same wall-clock convention as `ControllerState.clientTimestampUs`. |
| 12     | 2    | `numSamples`  | Number of `int16_t` samples that follow. Must not exceed `kMicAudioSamplesPerPacket`; the declared value must exactly match the number of sample bytes actually present in the packet, or it's rejected as malformed. |
| 14     | `numSamples * 2` | `samples` | Raw PCM, no compression, no forward-error-correction -- a lost UDP packet is an audible ~10ms gap in the DS's mic input, not a corrupted decode. |

Raw/uncompressed audio over lossy UDP with no FEC is a deliberate
Stage-1 tradeoff (same spirit as the video transport's own Stage-1
raw-pixel-buffer choice above) -- revisiting it (compression, jitter
buffering, loss concealment) is future work, not attempted here.

## ModeChanged payload

Implements GitHub issue #4 Phase B: an unsolicited, host-initiated
notification that the host switched which mode/adapter is driving the
session, sent to an already-connected, already-authenticated client on
the TCP control channel. Exists so a client can stay connected across
an emulator starting or exiting instead of having to reconnect --
`host/remote-server`'s `NetServer::setTarget()` is what triggers this
(see `docs/known-limitations.md`'s matching entry and
`docs/adr/0001-host-service-and-adapter-architecture.md` section 6).

Unlike most other packet types in this document (see also `ClientLog`
below), adding `ModeChanged` did **not** bump `protocolVersion`: it carries no negotiated-payload
compatibility concern the way a new *field* on `HelloPayload`/
`HelloAckPayload`/`DiscoveryResponsePayload` would (those are parsed
during the version-gated handshake itself). `ModeChanged` is sent later,
mid-session, unsolicited, to a client that already completed its
handshake against whatever `protocolVersion` it negotiated -- an older
client build (predating GitHub issue #4 Phase E, which is the first
client build to read anything from the control channel post-handshake)
simply never consumes these bytes; they sit harmlessly unread in its TCP
receive buffer rather than causing any parse error. A Phase-E-or-later
client's control-channel read loop (`client/src/net_client.cpp`'s
`controlReceiveLoop()`) tolerates any packet type it doesn't specifically
handle the same way, so this stays forward-compatible with future packet
types too.

| Offset | Size | Field   | Notes |
|-------:|-----:|---------|-------|
| 0      | 1    | `mode`  | `0` = `Emulation` (a real adapter, e.g. melonDS, is driving the session), `1` = `HostControl` (no emulator running -- client input drives a virtual gamepad, `HostControlAdapter`, that navigates the host's own UI instead). Any other value is rejected as malformed. |
| 1      | length-prefixed identity | `system`  | Same encoding as `HelloAckPayload.system`/`DiscoveryResponsePayload.system` below -- see "Emulator identity model". |
| ...    | length-prefixed identity | `adapter` | Same encoding as `HelloAckPayload.adapter`/`DiscoveryResponsePayload.adapter` below. |

Sending the identity alongside the mode (rather than just the bare mode
byte) means a client UI can show e.g. "Host Menu" or "Nintendo DS ·
melonDS" the instant this arrives, without a second round-trip or a
stale value left over from the last handshake.

A Phase-E-or-later client build (see `HelloAck payload` above for how it
learns the *initial* mode before any `ModeChanged` could ever arrive)
reads this packet via `NetClient::controlReceiveLoop()` and shows a
distinct "WAITING FOR EMULATOR" screen in place of the video texture
while `mode == HostControl` -- see `docs/known-limitations.md`'s
matching entry for what that UI does and doesn't do.

## ClientLog payload

Forwards one already-formatted client-side log line to the host, for
host-side debugging and app development (see `client/src/client_log.h`)
-- e.g. so a developer can see what a Steam Deck client is doing without
physical access to it or a visible terminal in Gaming Mode. Sent
client -> host on the TCP control channel, best-effort: the client
drops a line locally rather than blocking or growing an unbounded queue
if it isn't connected or its own outbound queue is already full (see
`NetClient::sendClientLog()`), so a burst of logging can never affect
real control/input traffic. Like `ModeChanged` above, this is a new
packet type an older peer simply never sends/reads -- it does not
change any existing struct's shape, so it did not require a
`protocolVersion` bump either.

| Offset | Size | Field  | Notes |
|-------:|-----:|--------|-------|
| 0      | 2    | length | Number of bytes in `line` that follow. |
| 2      | var. | `line` | Up to `kMaxClientLogLineLength` (512) bytes, UTF-8, not length-prefix-nested (a raw byte string, not a `readString()`-style field capped at the smaller `kMaxProtocolStringLength` used for identity fields elsewhere in this document). Sent exactly as formatted client-side, including its own trailing newline. |

The host (`NetServer::controlLoop()`) prints each received line to its
own stderr, prefixed with the sending client's address (e.g.
`[client 192.168.1.42] connection attempt already in progress; ignoring
duplicate request`), and additionally invokes
`NetServerConfig::onClientLogLine` if one is set, for a host-side UI (or
a test) that wants to react to it directly instead of scraping stderr.
A malformed `ClientLog` payload (declared length over the max, or
trailing garbage) is dropped with its own stderr warning; the
connection itself is not torn down for it, matching how a Heartbeat's
own occasional oddities are tolerated.

## Emulator identity model

Implements the first foundation milestone of GitHub issue #28 (evolving
DualDeck from melonDS-specific into an emulator-independent platform):
every host advertises which emulated system and which specific adapter/
emulator is actually running it, so client UI never has to guess or
hardcode assumptions -- see `docs/architecture.md`'s "Emulator identity
model" section for the full design rationale (why two separate structs,
who sets them, and what issue #28 still needs beyond this milestone).

Two small structs, `melonds_remote::SystemIdentity`/`AdapterIdentity`
(`protocol/include/melonds_remote/protocol.h`), each serialized as a
fixed sequence of length-prefixed strings (same encoding as every other
string field in this document -- a `u16` byte count followed by that many
bytes, capped at `kMaxProtocolStringLength` like everything else):

**SystemIdentity** (2 length-prefixed strings):

| Field        | Notes |
|--------------|-------|
| `systemId`   | Short, stable, machine-comparable identifier for the emulated system, e.g. `"nds"`, `"3ds"`, `"wiiu"`. Not free text -- future code may switch on this value, so it must never change meaning once shipped. |
| `systemName` | Human-readable name for UI, e.g. `"Nintendo DS"`. |

**AdapterIdentity** (3 length-prefixed strings):

| Field            | Notes |
|------------------|-------|
| `adapterId`      | Short, stable, machine-comparable identifier for the specific emulator/adapter, e.g. `"melonds"`, `"synthetic-test"`. Distinct from `systemId` -- more than one adapter could eventually target the same emulated system. |
| `adapterName`    | Human-readable emulator name for UI, e.g. `"melonDS"`. |
| `adapterVersion` | The adapter's own version string (e.g. melonDS's own release version for the melonDS adapter), distinct from `HelloPayload`/`HelloAckPayload.appVersion` (DualDeck's own release) and `protocolVersion` (wire format version) above. May be empty. |

Both structs are embedded, one after another (`system` then `adapter`),
in `DiscoveryResponsePayload` and `HelloAckPayload` -- see those payloads'
tables above for exactly where. There is no separate packet type for
identity; it always rides along with a payload a client already needs to
parse for another reason (discovery, or the handshake response), rather
than requiring a third round trip.

**Defaults**: a host that never explicitly sets these
(`host::NetServerConfig::systemIdentity`/`adapterIdentity`, e.g. the
standalone `host/remote-server` binary run with no `--system-id`/
`--adapter-id` flags, or a test harness constructing `NetServerConfig`
directly) reports `{"synthetic", "Synthetic Test System"}` /
`{"synthetic-test", "Synthetic Test Adapter", ""}` rather than empty
strings, so it's never mistaken in client UI for a real DS/melonDS
session. The melonDS integration always reports
`{"nds", "Nintendo DS"}` / `{"melonds", "melonDS", <melonDS's own
version>}` instead.

## Hello payload (variable length)

Each string field is length-prefixed: a `u16` byte count followed by that
many bytes (not null-terminated). Any declared length greater than
`kMaxProtocolStringLength` (64 bytes) is rejected as malformed, as is a
declared length that would run past the end of the received buffer.

| Field             | Type          | Notes |
|-------------------|---------------|-------|
| `clientName`      | length-prefixed string | e.g. "SteamDeck". Informational/logging only today. |
| `clientPlatform`  | length-prefixed string | e.g. "linux". Informational/logging only today. |
| `displayWidth`    | `u16`         | Client's display width in pixels. Not currently used by the host. |
| `displayHeight`   | `u16`         | Client's display height in pixels. Not currently used by the host. |
| `authToken`       | length-prefixed string | See "Authentication and device approval" below -- meaning depends on whether the host has a static `--auth-token` configured. |
| `appVersion`      | length-prefixed string | This client's release version (e.g. "v0.1.24"), from `MELONDS_REMOTE_VERSION`/the archive's `VERSION` file. Distinct from `protocolVersion` above -- see "App version mismatch" below. Empty on a from-source dev build with no wrapper script setting it. |
| `videoQuality`    | `u8`          | Added in protocol v9. JPEG quality (1-100, libjpeg-turbo's `tjCompress2` scale) this client wants its video compressed at for the session, or `0` to defer to the host's own `NetServerConfig::videoJpegQuality`. An out-of-range nonzero value is treated the same as `0`. Applied once, at handshake time -- there is no packet type for changing it without reconnecting. See "Video payload"'s JPEG-compression note for why this exists: a single host-wide default over-compressed DS/3DS, whose small frame sizes have bandwidth to spare that a Cemu-sized session over a slow link would not. |

The whole `Hello` payload is capped at 512 bytes by the host before it will
even attempt to parse it, so a hostile `payloadSize` can't be used to make
the host read/allocate an unbounded amount of data.

**Not yet implemented** (present in `SPEC.md` section 9 as a suggestion,
not a current requirement): supported pixel formats/codecs, controller/
touch capability flags. `clientName`/`clientPlatform`/display size exist
on the wire today but the host does not yet act on them beyond logging.
Microphone capability negotiation *is* now implemented, but travels the
other direction -- `HelloAckPayload.micSupported` below, not a client-side
Hello flag, since it's the host's own capability being negotiated.
Emulated-system/adapter identity *is* now negotiated too (GitHub issue
#28, see "Emulator identity model" below), also host->client only, for
the same reason -- there is nothing for the client to negotiate here, it
only ever consumes what the host reports.

## HelloAck payload (10 fixed bytes, a trailing length-prefixed string, 1 more fixed byte, 5 more length-prefixed strings, then 1 final fixed byte)

| Offset | Size | Field          | Notes |
|-------:|-----:|----------------|-------|
| 0      | 1    | `accepted`     | 0 or 1. Any other value is malformed. |
| 1      | 1    | `rejectReason` | Meaningful only if `accepted == 0`. See `HelloRejectReason` below. |
| 2      | 4    | `sessionId`    | Non-zero, host-chosen, only when `accepted == 1`. Informational today (logging/future reconnect correlation); not yet validated on subsequent packets. |
| 6      | 2    | `nativeWidth`  | Always 256. |
| 8      | 2    | `nativeHeight` | Always 192. |
| 10     | length-prefixed string | `appVersion` | The host's own release version, sent regardless of `accepted`/`rejectReason` -- lets the client show e.g. "host is on vX, you're on vY" even on a rejection. Empty if the host doesn't know its own version. |
| var.   | 1    | `micSupported` | 0 or 1, added in protocol v5. Whether this host build/config can accept and inject microphone audio (GitHub issue #2) -- the client should not bother opening a capture device or sending `MicAudioFrame` packets unless this is 1. Sent regardless of `accepted`/`rejectReason`, same as `appVersion`. |
| var.   | var. | `system.systemId`, `system.systemName`, `adapter.adapterId`, `adapter.adapterName`, `adapter.adapterVersion` | Five length-prefixed strings, added in protocol v6 (GitHub issue #28) -- see "Emulator identity model" below. Sent regardless of `accepted`/`rejectReason`, same as `appVersion`/`micSupported`. |
| var.   | 1    | `mode`         | 0 = `Emulation`, 1 = `HostControl` (`HostMode`, see "ModeChanged payload" below). Added in protocol v7, GitHub issue #4 Phase E. Sent regardless of `accepted`/`rejectReason`, same as everything above. This is what tells a client connecting (or reconnecting) *while the host is already in `HostControl` mode* that fact immediately -- `ModeChanged` is only ever sent to a client that is already connected and authenticated (see "ModeChanged payload" below), so a fresh handshake would otherwise never learn it. |

(Protocol v2 added a trailing length-prefixed `pairingToken` string here,
for the 6-digit-pairing-code flow described below under "History: the
6-digit pairing code". Protocol v3 removed it again along with that flow,
making `HelloAckPayload` a fixed 10 bytes for one version. Protocol v4
added the trailing `appVersion` string described above, making it
variable-length again. Protocol v5 appended the single `micSupported`
byte described above, after `appVersion`. Protocol v6 appended the five
identity strings described above, after `micSupported`. Protocol v7
appended the single `mode` byte described above, after the identity
strings.)

`HelloRejectReason`: `0` = none (accepted), `1` = protocol version
mismatch, `2` = authentication failed, `3` = host busy (reserved, not
currently sent since the host doesn't yet reject on "busy" -- an extra
control connection while one is active is simply closed without a
handshake attempt), `4` = approval required (see "Authentication and
device approval" below; renamed from `PairingRequired` in protocol v3),
`5` = app version mismatch (see "App version mismatch" below; added in
protocol v4).

## App version mismatch

`kProtocolVersion` (currently 7) only guards *wire-format* compatibility
-- two builds can share a wire format while being different releases
with different features/fixes. `appVersion` (both payloads above) is a
separate, exact-string check for that: if the host was started with a
known `appVersion` (`NetServerConfig::appVersion`, wired from
`MELONDS_REMOTE_VERSION`/the archive's `VERSION` file by
`run-host.sh`/`install-host-distrobox.sh`) and a connecting client's
`Hello.appVersion` is also non-empty and doesn't match exactly, the
handshake is rejected with `AppVersionMismatch` -- checked before
authentication/device-approval, so a stale client never even learns
whether its stale credentials would have worked. If either side's
`appVersion` is empty (a from-source dev build with no wrapper script
setting it), this check is skipped entirely and the handshake proceeds
as before -- this is opt-in hardening for packaged releases, not a
requirement for development.

## Authentication and device approval

Adapts spec section 13's "later pairing options" (originally: six-digit
pairing code, pre-shared token, QR code, certificate-based pairing).
Which mode is active is a single host-side choice
(`NetServerConfig::authToken` empty or not), not negotiated:

**Static token mode** (`--auth-token TOKEN` given to the host): the
`Hello` payload's `authToken` must equal that value exactly (compared in
constant time), or the handshake is rejected with `AuthenticationFailed`.
Device-approval mode (below) never runs in this mode. Intended for
scripting/CI (`tests/smoke_test.py` uses this) or anyone who'd rather
manage a shared secret themselves.

**Device-approval mode** (no `--auth-token`, the recommended default):
the client generates a random, persistent device identity once (32 hex
characters, `client/src/device_identity.h`) and sends the *same* value in
`Hello.authToken` on every connection attempt, to every host, forever --
there is no code typed on the client, ever. The host maintains a set of
approved device identities plus a queue of pending (not yet approved)
ones (`melonds_remote::host::DeviceApprovalManager`,
`host/remote-server/include/host/device_approval_manager.h`). For each
`Hello`:

1. If `authToken` (the device identity) is in the approved set: accept
   silently -- this is what makes reconnects (including the client's
   auto-reconnect-on-drop) not require re-approval.
2. Else: record or refresh a pending-request entry for this identity
   (deduped by identity, so a client's automatic retries don't create
   duplicate entries), surface it to a human at the host (console log
   with `approve`/`deny` commands on the standalone host; a `QMessageBox`
   Approve/Deny dialog on the melonDS-integrated host), and reject this
   attempt with `ApprovalRequired`. The client has nothing to do here but
   keep retrying automatically (its normal reconnect/backoff loop already
   does this) -- approval happens entirely on the host side.
3. A pending request not refreshed by a retry within `pendingRequestTtl`
   (default 60s) is silently evicted, so a client that gave up (powered
   off, pointed elsewhere) doesn't leave a permanent stale entry in the
   approval queue.

A human approves or denies a pending request by device identity (or an
unambiguous prefix, for convenience typing it by hand):
`DeviceApprovalManager::approve()`/`deny()`, exposed as `approve <id>`/
`deny <id>` console commands on the standalone host and Approve/Deny
buttons on the melonDS-integrated host's dialog. Approval persists the
identity to disk; denial just drops the pending entry (not a permanent
block -- if the same client retries, it becomes pending again, though the
melonDS-integrated host's dialog won't re-prompt for an already-denied
identity again within the same process run).

Approved device identities are persisted to `--state-dir PATH`
(standalone host) or `$HOME/.config/dualdeck/approved_devices.txt`
by default, or `$MELONDS_REMOTE_STATE_DIR` if set (melonDS-integrated
host), so an approved client stays approved across host restarts. There
is currently no UI to list or revoke individual approved devices -- only
deleting the state file entirely (forgetting everyone) is possible.

The SDL3 client persists its own device identity to
`$HOME/.config/dualdeck-client/device_id.txt` and reuses it for
every host, forever (regenerated only if that file is missing).

### History: the 6-digit pairing code

An earlier version of this project implemented spec section 13's
"six-digit pairing code" option literally: the client would show a
6-digit code-entry screen (driven by `SDL_StartTextInput()`, intended to
bring up Steam's on-screen keyboard in Gaming Mode) and the user typed
the code shown in the host's log. This was replaced with device-approval
authentication above because Steam Input doesn't reliably bring up a
virtual keyboard in Gaming Mode in practice, making the client-side
typing step unworkable -- device-approval moves all interaction to the
host side instead (which normally has a real keyboard/mouse), and
requires no typing on either side. See git history for the removed
`PairingManager`/`pairing_store.h` code if it's ever needed for
reference.

## Handshake (as currently implemented)

1. Client opens a TCP connection to the control port and sends a `Hello`
   packet with the payload described above.
2. Host checks, in order: source-IP connection-attempt rate limit (see
   "Rate limiting" below) -- applied before any handshake bytes are even
   read; `magic`/`protocolVersion`/`payloadSize` bound; successful parse of
   the Hello payload; and finally the `authToken` check described in
   "Authentication and device approval" above (either an exact
   static-token match, or the device-approval logic). On success, replies
   with a `HelloAck` (`accepted=1`, a fresh `sessionId`) and keeps the
   connection open. On any failure, replies with `HelloAck` (`accepted=0`,
   the relevant `rejectReason`) and then closes the connection.
3. Only once a client is accepted does the host start acting on
   `ControllerState` packets on the UDP input port, and only accepts a
   video connection on the video port -- both are matched against the
   authenticated client's source IP address. An unauthenticated sender on
   either port is silently ignored (input) or has its connection closed
   immediately (video). This closes a gap that existed before
   authentication was added: previously the UDP input port accepted any
   well-formed packet regardless of whether a control-channel client had
   completed a handshake.
4. The client should send `Heartbeat` packets on the control connection
   periodically (the SDL3 client sends one every second while otherwise
   idle). The host enforces a control-channel silence timeout
   (`controlHeartbeatTimeoutUs`, default 5s, via `SO_RCVTIMEO` on the
   accepted socket) independent of the UDP input timeout, so a
   TCP-alive-but-silent connection (e.g. a firewall dropping UDP only)
   still gets cleaned up.
5. Either side may send `Disconnect` to request a graceful close; the host
   also treats TCP EOF/error, a malformed control packet, or the
   heartbeat timeout as an implicit disconnect.
6. On disconnect (graceful, malformed-packet, or timeout), the host resets
   its `InputStateTracker`, clears the authenticated-client state, and
   calls `IEmulatorInputSink::releaseAll()` unconditionally (spec section
   6.4).

**Not yet implemented** (present in `SPEC.md` section 9 as a suggestion,
not a current requirement): supported pixel formats/codecs, controller/
touch capability negotiation (microphone capability *is* now negotiated,
see `HelloAckPayload.micSupported` above; emulated-system/adapter identity
*is* now negotiated too, see "Emulator identity model" above -- GitHub
issue #28), session IDs being carried on
any packet after `HelloAck` (so a stale/replayed session can't yet be
distinguished from a current one at the protocol level -- today this is
handled at the transport level via one-active-client-plus-source-IP-match
instead).

## Rate limiting

The host tracks control-connection attempts per source IP address in a
sliding window (`ConnectionRateLimiter`, `protocol/include/melonds_remote/rate_limiter.h`):
by default at most 5 attempts per 10 seconds per address. An attempt over
the limit is closed immediately, before any handshake bytes are read, and
still counts against the client's budget (so hammering the endpoint
doesn't reset it faster). This satisfies spec section 13's "rate-limit
connection attempts" as a bound on handshake attempts specifically; it
does not currently rate-limit already-established UDP input traffic.

## Validation rules enforced today

- Any packet whose `magic` doesn't match is rejected without further
  parsing.
- `ControllerState` packets are rejected if: the declared `payloadSize`
  doesn't match the number of bytes actually received, `touchActive` is
  any value other than 0 or 1, or `touchActive == 1` and the touch
  coordinates are outside the native DS range (0-255, 0-191).
- `ControllerState` packets with a `sequence` that is not "newer" than the
  last accepted packet (wraparound-aware comparison) are silently
  discarded (spec section 6.3).
- `ControllerState` packets are ignored entirely unless they come from
  the same source address as the currently-authenticated control-channel
  client.
- `Hello`/`HelloAck` string fields longer than 64 bytes, or a declared
  length that overruns the buffer, are rejected; a `Hello` payload larger
  than 512 bytes overall is rejected before parsing.
- The host accepts exactly one control connection and one video
  connection at a time; extra connection attempts while one is active are
  closed immediately. A video connection is additionally refused unless
  it comes from the address of the currently-authenticated control
  client.
