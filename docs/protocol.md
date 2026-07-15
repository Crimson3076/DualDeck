# Wire Protocol (v1)

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
| 4      | 2    | `protocolVersion`| Currently `1`. A mismatch is rejected by the receiver; it is not itself a fatal error for the connection. |
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
| 7     | `VideoFrame`      | host -> client  | TCP video | raw pixel buffer, see "Video payload" below |

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
| 7   | Quit session     |

## Video payload

Stage 1 transport (spec section 8.4): the payload is a raw pixel buffer,
`256 * 192 * 4 = 196608` bytes, **BGRA8888** byte order -- matching
melonDS's software renderer output directly (see
`docs/melonds-integration-analysis.md` section 1.1), so the eventual real
integration does not need a color conversion step. `host/remote-server`'s
`SyntheticFrameSource` already produces frames in this same format so the
client-side decode path is exercised end-to-end today.

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
| `authToken`       | length-prefixed string | Must equal the host's configured `--auth-token` exactly, or the handshake is rejected. Empty if the host has authentication disabled. |

The whole `Hello` payload is capped at 512 bytes by the host before it will
even attempt to parse it, so a hostile `payloadSize` can't be used to make
the host read/allocate an unbounded amount of data.

**Not yet implemented** (present in `SPEC.md` section 9 as a suggestion,
not a current requirement): supported pixel formats/codecs, controller/
touch/microphone capability flags. `clientName`/`clientPlatform`/display
size exist on the wire today but the host does not yet act on them beyond
logging.

## HelloAck payload (10 bytes)

| Offset | Size | Field          | Notes |
|-------:|-----:|----------------|-------|
| 0      | 1    | `accepted`     | 0 or 1. Any other value is malformed. |
| 1      | 1    | `rejectReason` | Meaningful only if `accepted == 0`. See `HelloRejectReason` below. |
| 2      | 4    | `sessionId`    | Non-zero, host-chosen, only when `accepted == 1`. Informational today (logging/future reconnect correlation); not yet validated on subsequent packets. |
| 6      | 2    | `nativeWidth`  | Always 256. |
| 8      | 2    | `nativeHeight` | Always 192. |

`HelloRejectReason`: `0` = none (accepted), `1` = protocol version
mismatch, `2` = authentication failed, `3` = host busy (reserved, not
currently sent since the host doesn't yet reject on "busy" -- an extra
control connection while one is active is simply closed without a
handshake attempt).

## Handshake (as currently implemented)

1. Client opens a TCP connection to the control port and sends a `Hello`
   packet with the payload described above.
2. Host checks, in order: source-IP connection-attempt rate limit (see
   "Rate limiting" below) -- applied before any handshake bytes are even
   read; `magic`/`protocolVersion`/`payloadSize` bound; successful parse of
   the Hello payload; and finally, if the host has `--auth-token` set,
   that `authToken` matches exactly. On success, replies with a
   `HelloAck` (`accepted=1`, a fresh `sessionId`) and keeps the connection
   open. On any failure, replies with `HelloAck` (`accepted=0`, the
   relevant `rejectReason`) and then closes the connection.
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
touch/microphone capability negotiation, session IDs being carried on any
packet after `HelloAck` (so a stale/replayed session can't yet be
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
