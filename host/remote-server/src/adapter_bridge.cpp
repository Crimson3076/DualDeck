#include "host/adapter_bridge.h"

namespace melonds_remote::host {

namespace {

std::string pickTargetSurface(const melonds_remote::adapter::AdapterCapabilities& caps) {
    for (const auto& s : caps.surfaces) {
        if (s.remotelyDisplayed) return s.surfaceId;
    }
    if (!caps.surfaces.empty()) return caps.surfaces.front().surfaceId;
    return {};
}

// Looks up targetId's own declared width/height among caps.surfaces,
// falling back to DS's native 256x192 only if it's genuinely not found
// (e.g. an adapter with zero surfaces) -- never silently assumes DS for
// an adapter that clearly declared something else.
void lookupTargetDimensions(const melonds_remote::adapter::AdapterCapabilities& caps,
                            const std::string& targetId, uint16_t& outWidth, uint16_t& outHeight) {
    for (const auto& s : caps.surfaces) {
        if (s.surfaceId == targetId) {
            outWidth = s.width;
            outHeight = s.height;
            return;
        }
    }
    outWidth = 256;
    outHeight = 192;
}

// Explicit bit-by-bit table rather than a raw bitmask cast: DSButton's
// and GenericButton's low bits happen to agree for A/B/X/Y/D-pad/L/R
// (both were designed with the same physical-button ordering) but
// diverge from Start/Select onward, since GenericButton reserves bits
// 10-13 for L2/R2/L3/R3 (concepts the DS has no equivalent of) before
// its own Start/Select at bits 14/15 -- see generic_input.h's comment
// on GenericButton for the full layout. Getting this table wrong would
// silently swap which physical button does what, so it's written out
// explicitly rather than "optimized" into a shift/mask trick.
uint32_t dsButtonsToGenericButtons(uint16_t dsButtons) {
    uint32_t generic = 0;
    if (dsButtons & melonds_remote::DSButton_A) generic |= melonds_remote::adapter::GenericButton_South;
    if (dsButtons & melonds_remote::DSButton_B) generic |= melonds_remote::adapter::GenericButton_East;
    if (dsButtons & melonds_remote::DSButton_X) generic |= melonds_remote::adapter::GenericButton_West;
    if (dsButtons & melonds_remote::DSButton_Y) generic |= melonds_remote::adapter::GenericButton_North;
    if (dsButtons & melonds_remote::DSButton_Up) generic |= melonds_remote::adapter::GenericButton_DpadUp;
    if (dsButtons & melonds_remote::DSButton_Down) generic |= melonds_remote::adapter::GenericButton_DpadDown;
    if (dsButtons & melonds_remote::DSButton_Left) generic |= melonds_remote::adapter::GenericButton_DpadLeft;
    if (dsButtons & melonds_remote::DSButton_Right) generic |= melonds_remote::adapter::GenericButton_DpadRight;
    if (dsButtons & melonds_remote::DSButton_L) generic |= melonds_remote::adapter::GenericButton_L1;
    if (dsButtons & melonds_remote::DSButton_R) generic |= melonds_remote::adapter::GenericButton_R1;
    if (dsButtons & melonds_remote::DSButton_Start) generic |= melonds_remote::adapter::GenericButton_Start;
    if (dsButtons & melonds_remote::DSButton_Select) generic |= melonds_remote::adapter::GenericButton_Select;
    return generic;
}

// Real Wii U GamePad hardware has clickable analog sticks (L3/R3) --
// unlike DSButton, which has no bits for these at all (the DS/3DS have
// no such concept), protocol v12's ExtraButton bitmask carries them
// (see protocol.h's own comment). host/cemu-patches/'s RemoteController.cpp
// already maps GenericButton_L3/R3 to the Wii U's real kButton6/kButton7
// stick-click bits -- this table is what actually populates them from
// the wire, closing a real gap where they were always 0 regardless of
// whether the wire carried real data (it didn't, before v12).
uint32_t extraButtonsToGenericButtons(uint8_t extraButtons) {
    uint32_t generic = 0;
    if (extraButtons & melonds_remote::ExtraButton_ThumbLeft) generic |= melonds_remote::adapter::GenericButton_L3;
    if (extraButtons & melonds_remote::ExtraButton_ThumbRight) generic |= melonds_remote::adapter::GenericButton_R3;
    return generic;
}

} // namespace

std::string AdapterBridge::targetSurfaceId() const {
    return pickTargetSurface(adapter_.capabilities());
}

void AdapterBridge::applyControllerState(const ControllerState& state) {
    melonds_remote::adapter::GenericInputState generic;
    generic.sequence = state.sequence;
    generic.clientTimestampUs = state.clientTimestampUs;
    generic.buttons = dsButtonsToGenericButtons(state.dsButtons) | extraButtonsToGenericButtons(state.extraButtons);
    generic.leftStickX = state.leftStickX;
    generic.leftStickY = state.leftStickY;
    generic.rightStickX = state.rightStickX;
    generic.rightStickY = state.rightStickY;
    // Real Wii U GamePad hardware has analog ZL/ZR -- see protocol.h's
    // leftTrigger/rightTrigger comment for why these aren't DS/3DS-only
    // despite living next to the other host-control-era additions.
    generic.leftTrigger = state.leftTrigger;
    generic.rightTrigger = state.rightTrigger;
    if (state.touchActive) {
        generic.touches.push_back(
            melonds_remote::adapter::TouchContact{targetSurfaceId(), state.touchX, state.touchY});
    }
    // Bit layout is intentionally identical between EmulatorAction
    // (protocol.h) and GenericEmulatorAction (generic_input.h) -- see
    // the latter's comment -- so this is a direct pass-through, not a
    // lookup table like the buttons above.
    generic.emulatorActions = state.emulatorActions;
    adapter_.applyGenericInput(generic);
}

void AdapterBridge::releaseAll() {
    adapter_.releaseAllInputs();
}

bool AdapterBridge::getLatestFrame(std::vector<uint8_t>& outFrame, uint64_t& outFrameIndex,
                                   uint16_t& outWidth, uint16_t& outHeight) {
    melonds_remote::adapter::SurfaceFrame frame;
    if (!adapter_.latestFrame(targetSurfaceId(), frame)) {
        return false;
    }
    outFrame = std::move(frame.pixels);
    outFrameIndex = frame.frameIndex;
    // Real, shipped bug this fixes: frameDimensions() below is a coarse
    // value negotiated once per connection (see its own comment) --
    // CemuAdapter's actual captured GamePad resolution depends on the
    // specific title running and isn't known at Hello time (that
    // connection is rebuilt fresh per title-launch, and the IPC
    // handshake all but always completes before the title has rendered
    // even one real frame), so a value sampled once at connection start
    // could never reliably reflect the truth for the rest of that
    // session. SurfaceFrame::width/height (adapter contract v2) is this
    // specific frame's own real size, straight from whatever the adapter
    // actually captured -- use it if the adapter populated it, falling
    // back to frameDimensions()'s declared value only for an adapter that
    // hasn't been updated to set it (it would otherwise be 0x0, which
    // would corrupt or crash the JPEG encoder in net_server.cpp).
    if (frame.width != 0 && frame.height != 0) {
        outWidth = frame.width;
        outHeight = frame.height;
    } else {
        frameDimensions(outWidth, outHeight);
    }
    return true;
}

void AdapterBridge::frameDimensions(uint16_t& outWidth, uint16_t& outHeight) const {
    const auto caps = adapter_.capabilities();
    lookupTargetDimensions(caps, pickTargetSurface(caps), outWidth, outHeight);
}

} // namespace melonds_remote::host
