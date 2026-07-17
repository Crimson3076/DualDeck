#pragma once

// A fake 3DS adapter capability fixture (GitHub issue #28 Phase 1):
// two video surfaces (a non-touch top screen, a touch-capable bottom
// screen), matching the shape issue #28's "Nintendo 3DS" section
// describes as required capabilities -- not a real 3DS emulator
// integration (no target emulator has been selected, see issue #28's
// "3DS adapter feasibility phase" section), just a fixture proving the
// generic contract handles a real multi-surface case beyond the DS's
// single bottom surface.

#include "melonds_remote/adapter/fake/fake_adapter_base.h"

namespace melonds_remote::adapter::fake {

class FakeThreeDsAdapter : public FakeAdapterBase {
public:
    FakeThreeDsAdapter();
};

} // namespace melonds_remote::adapter::fake
