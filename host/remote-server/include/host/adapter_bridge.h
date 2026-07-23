#pragma once

// The "DS compatibility adapter" decided in
// docs/adr/0001-host-service-and-adapter-architecture.md section 4:
// translates between NetServer's existing DS-specific
// IEmulatorInputSink/IFrameSource interfaces and a connected
// melonds_remote::adapter::IEmulatorAdapter (in-process, or
// out-of-process via adapter-sdk/ipc's AdapterIpcServer -- this class
// doesn't know or care which). This is what lets NetServer, and the
// live client<->host wire protocol it speaks (kProtocolVersion, unchanged
// by this class), be driven by any adapter implementing the
// generic contract instead of only the built-in
// LoggingInputSink/SyntheticFrameSource stand-ins -- without the wire
// protocol itself needing to become surface/generic-input-aware yet.
//
// Only one primary video surface at a time: the target surface (the
// first one flagged remotelyDisplayed, or the first surface at all if
// none is) is re-resolved from adapter.capabilities() on every call --
// never cached at construction -- there is no mechanism here for
// surface selection or multiple simultaneous surfaces. The wire
// protocol itself is no longer fixed at DS's native 256x192, though:
// frameDimensions() reports the target surface's own declared
// width/height (e.g. 320x240 for AzaharAdapter's 3DS bottom screen),
// which net_server.cpp relays to the client in HelloAck so it can size
// its receive buffer/texture to match instead of assuming DS. A true
// multi-surface Host Service is later work (see the ADR's "What this
// ADR does not decide yet").
//
// Re-resolving on every call (rather than caching once) is not an
// optimization -- it's required for correctness. This class is
// constructed once at process startup in --adapter-ipc mode (GitHub
// issue #4), long before any real adapter has connected over the IPC
// channel: `adapter.capabilities()` at that point has an empty
// `surfaces` list, so caching the picked surface ID/dimensions in the
// constructor permanently locks them to "" / DS's 256x192 default --
// silently breaking getLatestFrame() forever (empty-string surface ID
// never matches the real one, e.g. "bottom", once the adapter actually
// connects) while leaving input/touch mostly working, since neither
// depends on the frame actually arriving. This was a real, shipped bug:
// confirmed via AzaharAdapter's own capture-loop diagnostics showing
// 100% successful captures on the Azahar side while NetServer's video
// stats stayed at zero sent frames the entire session -- see
// docs/known-limitations.md.
//
// Microphone audio is deliberately NOT bridged here: the generic
// contract's GenericInputState only carries a micActive flag, not raw
// PCM samples (see generic_input.h) -- actually forwarding audio
// through IEmulatorAdapter would require a contract change this class
// doesn't make. A caller wiring this up alongside real mic support
// needs a separate IMicAudioSink, same as today's LoggingMicAudioSink.

#include <string>

#include "host/emulator_input_sink.h"
#include "host/frame_source.h"
#include "melonds_remote/adapter/adapter_contract.h"

namespace melonds_remote::host {

class AdapterBridge : public IEmulatorInputSink, public IFrameSource {
public:
    // `adapter` must outlive this object. Unlike an earlier version of
    // this class, `adapter` does NOT need to already be connected/have
    // valid capabilities() by the time this constructor runs -- see the
    // class comment above for why that used to be a real, silently
    // broken precondition.
    explicit AdapterBridge(melonds_remote::adapter::IEmulatorAdapter& adapter) : adapter_(adapter) {}

    // IEmulatorInputSink
    void applyControllerState(const ControllerState& state) override;
    void releaseAll() override;

    // IFrameSource
    bool getLatestFrame(std::vector<uint8_t>& outFrame, uint64_t& outFrameIndex,
                        uint16_t& outWidth, uint16_t& outHeight) override;
    // Overrides IFrameSource's DS-sized default with the target surface's
    // actual declared width/height (from the adapter's own capabilities()
    // -- e.g. AzaharAdapter reports 320x240, not DS's 256x192).
    void frameDimensions(uint16_t& outWidth, uint16_t& outHeight) const override;

    // The surface ID this bridge currently reads frames from / attaches
    // touch contacts to -- re-resolved from adapter_.capabilities() on
    // every call (see class comment), so this reflects live state, not
    // a value fixed at construction. Exposed for logging/diagnostics,
    // not required for normal use.
    std::string targetSurfaceId() const;

private:
    melonds_remote::adapter::IEmulatorAdapter& adapter_;
};

} // namespace melonds_remote::host
