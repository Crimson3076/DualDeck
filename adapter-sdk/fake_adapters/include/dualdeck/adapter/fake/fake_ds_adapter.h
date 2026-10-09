#pragma once

// A fake DS adapter capability fixture (GitHub issue #28 Phase 1): one
// 256x192 touch-capable bottom surface, matching the real melonDS
// adapter's shape today (see host/melonds-patches/'s RemoteServerBridge
// and protocol.h's kTouchMaxX/kTouchMaxY) -- not a real emulator, just
// a fixture proving the generic contract can describe DS's existing
// single-surface case, for comparison against the multi-surface 3DS/
// Wii U fixtures alongside it.

#include "dualdeck/adapter/fake/fake_adapter_base.h"

namespace dualdeck::adapter::fake {

class FakeDsAdapter : public FakeAdapterBase {
public:
    FakeDsAdapter();
};

} // namespace dualdeck::adapter::fake
