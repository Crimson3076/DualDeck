// Focused tests for SystemIdentity/AdapterIdentity (GitHub issue #28's
// architecture foundation milestone), independent of which payload they
// end up embedded in -- HelloAckPayload and DiscoveryResponsePayload
// each get their own round-trip coverage in test_handshake.cpp and
// test_discovery.cpp respectively; this file exercises the shared
// appendSystemIdentity/readSystemIdentity/appendAdapterIdentity/
// readAdapterIdentity helpers directly.

#include "melonds_remote/protocol.h"
#include "test_framework.h"

using namespace melonds_remote;

MDR_TEST(system_identity_round_trip) {
    SystemIdentity id{"nds", "Nintendo DS"};
    ByteBuffer buf;
    appendSystemIdentity(buf, id);

    size_t offset = 0;
    auto parsed = readSystemIdentity(buf.data(), buf.size(), offset);
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->systemId == "nds");
    MDR_CHECK(parsed->systemName == "Nintendo DS");
    MDR_CHECK_EQ(offset, buf.size());
}

MDR_TEST(system_identity_empty_round_trip) {
    SystemIdentity id; // default: both fields empty
    ByteBuffer buf;
    appendSystemIdentity(buf, id);

    size_t offset = 0;
    auto parsed = readSystemIdentity(buf.data(), buf.size(), offset);
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->systemId.empty());
    MDR_CHECK(parsed->systemName.empty());
}

MDR_TEST(system_identity_rejects_truncated_buffer) {
    SystemIdentity id{"nds", "Nintendo DS"};
    ByteBuffer buf;
    appendSystemIdentity(buf, id);
    buf.resize(buf.size() - 1);

    size_t offset = 0;
    MDR_CHECK(!readSystemIdentity(buf.data(), buf.size(), offset).has_value());
}

MDR_TEST(system_identity_rejects_oversized_field) {
    ByteBuffer buf;
    appendU16(buf, static_cast<uint16_t>(kMaxProtocolStringLength + 1));
    for (size_t i = 0; i < kMaxProtocolStringLength + 1; ++i) buf.push_back('a');

    size_t offset = 0;
    MDR_CHECK(!readSystemIdentity(buf.data(), buf.size(), offset).has_value());
}

MDR_TEST(adapter_identity_round_trip) {
    AdapterIdentity id{"melonds", "melonDS", "0.9.5"};
    ByteBuffer buf;
    appendAdapterIdentity(buf, id);

    size_t offset = 0;
    auto parsed = readAdapterIdentity(buf.data(), buf.size(), offset);
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->adapterId == "melonds");
    MDR_CHECK(parsed->adapterName == "melonDS");
    MDR_CHECK(parsed->adapterVersion == "0.9.5");
    MDR_CHECK_EQ(offset, buf.size());
}

MDR_TEST(adapter_identity_empty_version_round_trip) {
    AdapterIdentity id{"synthetic-test", "Synthetic Test Adapter", ""};
    ByteBuffer buf;
    appendAdapterIdentity(buf, id);

    size_t offset = 0;
    auto parsed = readAdapterIdentity(buf.data(), buf.size(), offset);
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->adapterVersion.empty());
}

MDR_TEST(adapter_identity_rejects_truncated_buffer) {
    AdapterIdentity id{"melonds", "melonDS", "0.9.5"};
    ByteBuffer buf;
    appendAdapterIdentity(buf, id);
    buf.resize(buf.size() - 1);

    size_t offset = 0;
    MDR_CHECK(!readAdapterIdentity(buf.data(), buf.size(), offset).has_value());
}

// Reads two identities back-to-back from the same buffer, mirroring how
// HelloAckPayload/DiscoveryResponsePayload embed both one after another
// -- confirms `offset` is correctly threaded through both helpers rather
// than each assuming it starts at 0.
MDR_TEST(system_then_adapter_identity_share_offset_correctly) {
    SystemIdentity sys{"3ds", "Nintendo 3DS"};
    AdapterIdentity adapter{"fake-3ds", "Fake 3DS Adapter (test fixture)", "0.0.1"};

    ByteBuffer buf;
    appendSystemIdentity(buf, sys);
    appendAdapterIdentity(buf, adapter);

    size_t offset = 0;
    auto parsedSys = readSystemIdentity(buf.data(), buf.size(), offset);
    MDR_CHECK(parsedSys.has_value());
    MDR_CHECK(parsedSys->systemId == "3ds");

    auto parsedAdapter = readAdapterIdentity(buf.data(), buf.size(), offset);
    MDR_CHECK(parsedAdapter.has_value());
    MDR_CHECK(parsedAdapter->adapterId == "fake-3ds");
    MDR_CHECK(parsedAdapter->adapterName == "Fake 3DS Adapter (test fixture)");
    MDR_CHECK_EQ(offset, buf.size());
}

// GitHub issue #28's protocol version compatibility decision: adding
// these identity fields to HelloAckPayload/DiscoveryResponsePayload
// changed their wire layout, so kProtocolVersion had to move to a new
// value (see protocol.h's version-history comment on kProtocolVersion
// and docs/protocol.md's "Emulator identity model" section) -- a v5
// host/client pair without this field is rejected outright by the
// existing protocolVersion check elsewhere (net_server.cpp/
// discovery_client.cpp), not partially parsed. This is a guard against a
// silent accidental revert of that bump, not a behavioral test of the
// rejection itself (already covered by the existing whole-packet
// version-mismatch handling that predates this change).
MDR_TEST(protocol_version_was_bumped_for_identity_fields) {
    MDR_CHECK_EQ(kProtocolVersion, 6);
}
