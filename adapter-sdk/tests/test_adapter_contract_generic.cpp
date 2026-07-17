// Runs the same assertions against all three fake fixtures (DS, 3DS,
// Wii U) purely through the abstract IEmulatorAdapter interface, with
// no per-adapter branches -- proving the generic contract in
// adapter_contract.h actually is generic, not just "happens to fit the
// one adapter it was designed against" (GitHub issue #28: "keep the
// new identity/adapter model reusable for future 3DS and Wii U
// adapters"). Deeper white-box checks (which need the fake fixtures'
// own test-only helpers, not part of IEmulatorAdapter) are run
// separately against FakeAdapterBase& below, still generically across
// all three concrete types.

#include <memory>
#include <vector>

#include "melonds_remote/adapter/fake/fake_3ds_adapter.h"
#include "melonds_remote/adapter/fake/fake_ds_adapter.h"
#include "melonds_remote/adapter/fake/fake_wiiu_adapter.h"
#include "test_framework.h"

using namespace melonds_remote::adapter;
using namespace melonds_remote::adapter::fake;

namespace {

// Interface-level checks: only methods on IEmulatorAdapter itself, the
// same set of calls a real Host Service would make against any adapter
// it doesn't know the concrete type of.
void checkContractBasics(IEmulatorAdapter& adapter) {
    AdapterCapabilities caps = adapter.capabilities();
    MDR_CHECK(!caps.system.systemId.empty());
    MDR_CHECK(!caps.system.systemName.empty());
    MDR_CHECK(!caps.adapter.adapterId.empty());
    MDR_CHECK(!caps.adapter.adapterName.empty());
    MDR_CHECK(!caps.surfaces.empty());
    for (const auto& surface : caps.surfaces) {
        MDR_CHECK(!surface.surfaceId.empty());
        MDR_CHECK(surface.width > 0);
        MDR_CHECK(surface.height > 0);
    }

    MDR_CHECK(adapter.currentState() == SessionState::Available);

    // Input release must be safe to call at any point, including before
    // any input was ever applied (issue #28: "Input release on
    // disconnect, timeout, adapter shutdown, or session change" --
    // "every disconnect and adapter-loss path" from the required
    // testing-strategy list includes a path that never had input in the
    // first place, e.g. a client that connects and immediately drops).
    adapter.releaseAllInputs();

    GenericInputState input;
    input.buttons = GenericButton_South;
    input.touches.push_back(TouchContact{caps.surfaces.front().surfaceId, 10, 10});
    adapter.applyGenericInput(input); // must not crash regardless of adapter type
    adapter.releaseAllInputs();       // must not crash afterward either

    // No frame pushed via IEmulatorAdapter itself (frame submission is
    // adapter-internal, driven by the emulator, not the Host Service) --
    // latestFrame() for a declared surface must simply report "none
    // yet," not crash or return stale/garbage data.
    SurfaceFrame outFrame;
    MDR_CHECK(!adapter.latestFrame(caps.surfaces.front().surfaceId, outFrame));

    // An adapter must not crash when asked about a surface it never
    // declared (issue #28: "A malformed or incompatible adapter must
    // not crash the Host Service" -- the same must hold in reverse, a
    // Host Service typo/bug querying an unknown surface must not crash
    // the adapter).
    MDR_CHECK(!adapter.latestFrame("nonexistent-surface-id", outFrame));
}

// White-box checks against the shared fixture plumbing (FakeAdapterBase),
// still run generically across all three concrete fixture types.
void checkInputLifecycleGeneric(FakeAdapterBase& adapter) {
    MDR_CHECK(adapter.isReleased());

    GenericInputState input;
    input.buttons = GenericButton_East | GenericButton_DpadLeft;
    input.sequence = 42;
    adapter.applyGenericInput(input);
    MDR_CHECK(!adapter.isReleased());
    MDR_CHECK_EQ(adapter.lastAppliedInput().sequence, 42u);

    adapter.releaseAllInputs();
    MDR_CHECK(adapter.isReleased());
}

void checkMultiSurfaceFramesGeneric(FakeAdapterBase& adapter, const AdapterCapabilities& caps) {
    // Push a distinct frame to every declared surface, then confirm
    // each surface's latestFrame() only ever reflects its own pushes --
    // covers the required "multiple video surfaces with different
    // dimensions" + "per-surface latest-frame-wins" tests generically,
    // for however many surfaces this particular adapter declares (one
    // for the fake DS, two for fake 3DS/Wii U).
    for (size_t i = 0; i < caps.surfaces.size(); ++i) {
        adapter.pushFrame(caps.surfaces[i].surfaceId, std::vector<uint8_t>(1, static_cast<uint8_t>(i)));
    }
    for (size_t i = 0; i < caps.surfaces.size(); ++i) {
        SurfaceFrame frame;
        MDR_CHECK(adapter.latestFrame(caps.surfaces[i].surfaceId, frame));
        MDR_CHECK_EQ(frame.pixels.size(), 1u);
        MDR_CHECK_EQ(frame.pixels[0], static_cast<uint8_t>(i));
    }
}

} // namespace

MDR_TEST(all_fake_adapters_satisfy_contract_basics) {
    FakeDsAdapter ds;
    FakeThreeDsAdapter ds3;
    FakeWiiUAdapter wiiu;

    IEmulatorAdapter* adapters[] = {&ds, &ds3, &wiiu};
    for (IEmulatorAdapter* a : adapters) {
        checkContractBasics(*a);
    }
}

MDR_TEST(all_fake_adapters_release_input_generically) {
    FakeDsAdapter ds;
    FakeThreeDsAdapter ds3;
    FakeWiiUAdapter wiiu;

    FakeAdapterBase* adapters[] = {&ds, &ds3, &wiiu};
    for (FakeAdapterBase* a : adapters) {
        checkInputLifecycleGeneric(*a);
    }
}

MDR_TEST(all_fake_adapters_handle_multi_surface_frames_generically) {
    FakeDsAdapter ds;
    FakeThreeDsAdapter ds3;
    FakeWiiUAdapter wiiu;

    FakeAdapterBase* adapters[] = {&ds, &ds3, &wiiu};
    for (FakeAdapterBase* a : adapters) {
        checkMultiSurfaceFramesGeneric(*a, a->capabilities());
    }
}

MDR_TEST(all_fake_adapters_report_distinct_system_identities) {
    // A sanity check that the three fixtures don't accidentally share a
    // systemId (which would silently defeat every "generic across
    // adapters" test above by testing the same thing three times).
    FakeDsAdapter ds;
    FakeThreeDsAdapter ds3;
    FakeWiiUAdapter wiiu;

    std::string ids[] = {ds.capabilities().system.systemId, ds3.capabilities().system.systemId,
                          wiiu.capabilities().system.systemId};
    MDR_CHECK(ids[0] != ids[1]);
    MDR_CHECK(ids[0] != ids[2]);
    MDR_CHECK(ids[1] != ids[2]);
}
