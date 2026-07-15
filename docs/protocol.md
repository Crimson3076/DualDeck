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
| 1     | `Hello`           | client -> host  | TCP control | none in the current prototype (see "Handshake" below) |
| 2     | `HelloAck`        | host -> client  | TCP control | none in the current prototype |
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
| 4      | 8    | `clientTimestampUs`| Client-side capture time, for future latency instrumentation. Not currently validated by the host. |
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

## Handshake (as currently implemented)

1. Client opens a TCP connection to the control port and sends a `Hello`
   packet with an empty payload.
2. Host validates `magic` and `protocolVersion`. On success, replies with
   a `HelloAck` packet (empty payload) and keeps the connection open.
   On failure, the host does not reply and the connection is expected to
   be closed by the client.
3. Either side may send `Heartbeat` packets on the control connection;
   the host does not currently enforce a heartbeat interval itself,
   because client liveness is already tracked via the UDP input stream's
   timeout (`InputStateTracker::isTimedOut`, default 500ms). A future
   revision may add an explicit heartbeat timeout for clients that are
   connected but not sending input (e.g. a paused game).
4. Either side may send `Disconnect` to request a graceful close; the host
   also treats TCP EOF/error on the control socket as an implicit
   disconnect.
5. On disconnect (graceful or timeout), the host resets its
   `InputStateTracker` and calls `IEmulatorInputSink::releaseAll()`
   unconditionally (spec section 6.4).

**Not yet implemented** (present in `SPEC.md` section 9 as a suggestion,
not a current requirement): client capability negotiation (display
resolution, supported pixel formats/codecs, controller/touch/microphone
capabilities), session IDs, native-resolution/frame-rate advertisement in
`HelloAck`, and a pairing token. These belong to Phase 2 (spec section
"Development Phases" / Phase 2: Network robustness) and are called out
here so the gap is explicit rather than silently assumed.

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
- The host accepts exactly one control connection and one video
  connection at a time; extra connection attempts while one is active are
  closed immediately.
