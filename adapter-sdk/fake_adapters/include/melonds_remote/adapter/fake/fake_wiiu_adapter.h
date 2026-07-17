#pragma once

// A fake Wii U adapter capability fixture (GitHub issue #28 Phase 1):
// two video surfaces (a non-touch TV output, a touch-capable GamePad
// screen), matching the shape issue #28's "Nintendo Wii U" section
// describes -- not a real Wii U emulator integration (no target
// emulator has been selected, see issue #28's "Wii U adapter
// feasibility phase" section), just a fixture proving the generic
// contract handles a TV+GamePad pair distinct from DS/3DS's top+bottom
// pair (different SurfaceRole values, much larger TV resolution).

#include "melonds_remote/adapter/fake/fake_adapter_base.h"

namespace melonds_remote::adapter::fake {

class FakeWiiUAdapter : public FakeAdapterBase {
public:
    FakeWiiUAdapter();
};

} // namespace melonds_remote::adapter::fake
