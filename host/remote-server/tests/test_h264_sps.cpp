// Latency-audit follow-up (2026-08-26 DualDeck-vs-Vanilla audit): the
// audit flagged that nothing in this project actually inspected what SPS
// OpenH264 emits -- disabling B-frames and configuring CAMERA_VIDEO_REAL_TIME
// (see h264_encoder.cpp's own comment) is not the same thing as confirming
// the resulting bitstream actually advertises Vanilla's stated low-latency
// buffering properties (max_num_reorder_frames = 0, max_dec_frame_buffering
// = 1, a single reference frame). This file closes that gap: it parses the
// real SPS NAL unit H264Encoder produces (this project's own encoder, real
// OpenH264 underneath, not a mocked/assumed bitstream) and asserts on the
// actual field values, not on what the encoder's *configuration* implies
// they should be.
//
// The parser below implements just enough of ITU-T H.264 (08/2021) section
// 7.3.2.1.1 (seq_parameter_set_data) and Annex E.1.1 (vui_parameters) to
// reach max_num_ref_frames (a direct SPS field) and, inside the VUI's
// bitstream_restriction() sub-structure, max_num_reorder_frames and
// max_dec_frame_buffering -- every intervening field the spec places before
// them (scaling lists, HRD parameters, etc.) is still parsed (bits
// consumed in the right order and, where their own presence is
// conditional, via their own real presence flag), not skipped by byte
// offset or assumption, since a wrong guess there would silently misalign
// every field read afterward.

#include "host/h264_encoder.h"
#include "test_framework.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

#ifdef DUALDECK_HAVE_OPENH264

using namespace melonds_remote;
using namespace melonds_remote::host;

namespace {

std::vector<uint8_t> makeTestFrameBgra(int width, int height) {
    std::vector<uint8_t> frame(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t* px =
                &frame[(static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4];
            px[0] = static_cast<uint8_t>(x * 255 / std::max(width - 1, 1));
            px[1] = static_cast<uint8_t>(y * 255 / std::max(height - 1, 1));
            px[2] = static_cast<uint8_t>(((x + y) * 255) / std::max(width + height - 2, 1));
            px[3] = 0xFF;
        }
    }
    return frame;
}

// Removes H.264's "emulation prevention" 0x03 bytes (any 0x03 that
// immediately follows two 0x00 bytes, inserted purely so the RBSP never
// contains a byte sequence that could be mistaken for a start code) --
// the RBSP bit-parsing below must run on the de-escaped bytes, not the
// raw Annex-B NAL payload.
std::vector<uint8_t> stripEmulationPrevention(const uint8_t* data, size_t size) {
    std::vector<uint8_t> out;
    out.reserve(size);
    int zeroRunLength = 0;
    for (size_t i = 0; i < size; ++i) {
        if (zeroRunLength >= 2 && data[i] == 0x03 && i + 1 < size && data[i + 1] <= 0x03) {
            zeroRunLength = 0; // drop this 0x03, don't count it towards the next run
            continue;
        }
        out.push_back(data[i]);
        zeroRunLength = (data[i] == 0x00) ? zeroRunLength + 1 : 0;
    }
    return out;
}

// Minimal MSB-first bit reader plus H.264's two Exp-Golomb codes (ue(v):
// unsigned, se(v): signed, ITU-T H.264 section 9.1) -- everything the SPS
// parser below needs, nothing more.
struct BitReader {
    const uint8_t* data;
    size_t sizeBits;
    size_t pos = 0;
    bool overran = false;

    BitReader(const uint8_t* d, size_t sizeBytes) : data(d), sizeBits(sizeBytes * 8) {}

    int u1() {
        if (pos >= sizeBits) {
            overran = true;
            return 0;
        }
        int bit = (data[pos / 8] >> (7 - (pos % 8))) & 1;
        ++pos;
        return bit;
    }

    uint32_t u(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | static_cast<uint32_t>(u1());
        return v;
    }

    uint32_t ue() {
        int leadingZeroBits = 0;
        while (u1() == 0 && !overran) {
            ++leadingZeroBits;
            if (leadingZeroBits >= 32) break; // malformed input guard, not a real code length
        }
        if (leadingZeroBits == 0) return 0;
        return (1u << leadingZeroBits) - 1 + u(leadingZeroBits);
    }

    int32_t se() {
        uint32_t codeNum = ue();
        int32_t magnitude = static_cast<int32_t>((codeNum + 1) / 2);
        return (codeNum % 2 == 0) ? -magnitude : magnitude;
    }
};

// ITU-T H.264 section 7.3.2.1.1.1 scaling_list() -- only the bits need
// consuming correctly here (the actual scaling values decoded aren't
// needed by anything below), so this discards them rather than building
// the real ScalingList4x4/8x8 arrays a decoder would need.
void skipScalingList(BitReader& br, int size) {
    int lastScale = 8, nextScale = 8;
    for (int j = 0; j < size && !br.overran; ++j) {
        if (nextScale != 0) {
            int32_t deltaScale = br.se();
            nextScale = (lastScale + deltaScale + 256) % 256;
        }
        lastScale = (nextScale == 0) ? lastScale : nextScale;
    }
}

// ITU-T H.264 Annex E.1.2 hrd_parameters() -- referenced (not necessarily
// present) from within vui_parameters() below; must still be fully parsed
// when present so the bit position lands correctly on whatever VUI field
// follows it, even though nothing here reads its values.
void skipHrdParameters(BitReader& br) {
    uint32_t cpbCntMinus1 = br.ue();
    br.u(4); // bit_rate_scale
    br.u(4); // cpb_size_scale
    for (uint32_t i = 0; i <= cpbCntMinus1 && !br.overran; ++i) {
        br.ue(); // bit_rate_value_minus1[i]
        br.ue(); // cpb_size_value_minus1[i]
        br.u1(); // cbr_flag[i]
    }
    br.u(5); // initial_cpb_removal_delay_length_minus1
    br.u(5); // cpb_removal_delay_length_minus1
    br.u(5); // dpb_output_delay_length_minus1
    br.u(5); // time_offset_length
}

struct SpsInfo {
    uint32_t profileIdc = 0;
    uint32_t levelIdc = 0;
    uint32_t maxNumRefFrames = 0;
    bool vuiPresent = false;
    bool bitstreamRestrictionPresent = false;
    // Only meaningful when bitstreamRestrictionPresent is true -- callers
    // must check that first, same convention as the wire fields they came
    // from (there is no sentinel value the spec itself defines for
    // "absent").
    uint32_t maxNumReorderFrames = 0;
    uint32_t maxDecFrameBuffering = 0;
};

// ITU-T H.264 (08/2021) section 7.3.2.1.1 seq_parameter_set_data(), plus
// Annex E.1.1 vui_parameters() for the two bitstream_restriction() fields
// this project actually cares about. `rbsp` is the SPS NAL's payload
// *after* the 1-byte NAL header and *after* stripEmulationPrevention().
std::optional<SpsInfo> parseSpsData(const std::vector<uint8_t>& rbsp) {
    BitReader br(rbsp.data(), rbsp.size());
    SpsInfo info;

    info.profileIdc = br.u(8);
    br.u(6); // constraint_set0_flag .. constraint_set5_flag (1 bit each)
    br.u(2); // reserved_zero_2bits
    info.levelIdc = br.u(8);
    br.ue(); // seq_parameter_set_id

    // High-profile-and-above-only chroma/bit-depth/scaling-matrix block --
    // the exact profile_idc list ITU-T H.264 7.3.2.1.1 itself enumerates.
    // OpenH264's real-time/CAVLC configuration is expected to emit
    // Baseline (66), which skips this block entirely, but this is written
    // to parse it correctly regardless of which profile actually shows up
    // -- asserting on profile_idc is what the test below does to confirm
    // that expectation, not this parser silently assuming it.
    static constexpr uint32_t kHighProfileIdcs[] = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135};
    bool highProfile = false;
    for (uint32_t p : kHighProfileIdcs) highProfile = highProfile || (info.profileIdc == p);
    if (highProfile) {
        uint32_t chromaFormatIdc = br.ue();
        if (chromaFormatIdc == 3) br.u1(); // separate_colour_plane_flag
        br.ue(); // bit_depth_luma_minus8
        br.ue(); // bit_depth_chroma_minus8
        br.u1(); // qpprime_y_zero_transform_bypass_flag
        if (br.u1()) { // seq_scaling_matrix_present_flag
            int count = (chromaFormatIdc != 3) ? 8 : 12;
            for (int idx = 0; idx < count; ++idx) {
                if (br.u1()) skipScalingList(br, idx < 6 ? 16 : 64); // seq_scaling_list_present_flag[idx]
            }
        }
    }

    br.ue(); // log2_max_frame_num_minus4
    uint32_t picOrderCntType = br.ue();
    if (picOrderCntType == 0) {
        br.ue(); // log2_max_pic_order_cnt_lsb_minus4
    } else if (picOrderCntType == 1) {
        br.u1(); // delta_pic_order_always_zero_flag
        br.se(); // offset_for_non_ref_pic
        br.se(); // offset_for_top_to_bottom_field
        uint32_t numRefFramesInCycle = br.ue();
        for (uint32_t k = 0; k < numRefFramesInCycle && !br.overran; ++k) br.se(); // offset_for_ref_frame[k]
    }

    info.maxNumRefFrames = br.ue();
    br.u1(); // gaps_in_frame_num_value_allowed_flag
    br.ue(); // pic_width_in_mbs_minus1
    br.ue(); // pic_height_in_map_units_minus1
    bool frameMbsOnly = br.u1();
    if (!frameMbsOnly) br.u1(); // mb_adaptive_frame_field_flag
    br.u1(); // direct_8x8_inference_flag
    if (br.u1()) { // frame_cropping_flag
        br.ue(); br.ue(); br.ue(); br.ue(); // left/right/top/bottom offsets
    }

    info.vuiPresent = br.u1();
    if (info.vuiPresent) {
        if (br.u1()) { // aspect_ratio_info_present_flag
            uint32_t aspectRatioIdc = br.u(8);
            if (aspectRatioIdc == 255) { // Extended_SAR
                br.u(16); // sar_width
                br.u(16); // sar_height
            }
        }
        if (br.u1()) br.u1(); // overscan_info_present_flag -> overscan_appropriate_flag
        if (br.u1()) { // video_signal_type_present_flag
            br.u(3); // video_format
            br.u1(); // video_full_range_flag
            if (br.u1()) { // colour_description_present_flag
                br.u(8); br.u(8); br.u(8); // colour_primaries, transfer_characteristics, matrix_coefficients
            }
        }
        if (br.u1()) { // chroma_loc_info_present_flag
            br.ue(); br.ue(); // chroma_sample_loc_type_top/bottom_field
        }
        if (br.u1()) { // timing_info_present_flag
            br.u(32); br.u(32); br.u1(); // num_units_in_tick, time_scale, fixed_frame_rate_flag
        }
        bool nalHrdPresent = br.u1();
        if (nalHrdPresent) skipHrdParameters(br);
        bool vclHrdPresent = br.u1();
        if (vclHrdPresent) skipHrdParameters(br);
        if (nalHrdPresent || vclHrdPresent) br.u1(); // low_delay_hrd_flag
        br.u1(); // pic_struct_present_flag
        info.bitstreamRestrictionPresent = br.u1();
        if (info.bitstreamRestrictionPresent) {
            br.u1(); // motion_vectors_over_pic_boundaries_flag
            br.ue(); // max_bytes_per_pic_denom
            br.ue(); // max_bits_per_mb_denom
            br.ue(); // log2_max_mv_length_horizontal
            br.ue(); // log2_max_mv_length_vertical
            info.maxNumReorderFrames = br.ue();
            info.maxDecFrameBuffering = br.ue();
        }
    }

    if (br.overran) {
        // A real parse bug (or a genuinely unsupported bitstream shape) --
        // either way the fields above can't be trusted, so the caller
        // should treat this the same as "couldn't find an SPS at all"
        // rather than asserting on garbage.
        return std::nullopt;
    }
    return info;
}

// Finds the first NAL unit of `wantedType` in `annexB` (Annex-B format:
// each NAL prefixed by a 3- or 4-byte start code) and returns its RBSP
// (NAL header byte stripped, emulation prevention removed) -- exactly
// what parseSpsData() above expects as input for wantedType == 7 (SPS).
std::optional<std::vector<uint8_t>> findNalRbsp(const ByteBuffer& annexB, int wantedType) {
    size_t i = 0;
    while (i + 3 <= annexB.size()) {
        size_t scLen = 0;
        if (annexB[i] == 0 && annexB[i + 1] == 0 && annexB[i + 2] == 1) {
            scLen = 3;
        } else if (i + 4 <= annexB.size() && annexB[i] == 0 && annexB[i + 1] == 0 && annexB[i + 2] == 0 &&
                   annexB[i + 3] == 1) {
            scLen = 4;
        }
        if (scLen == 0) {
            ++i;
            continue;
        }
        size_t nalStart = i + scLen;
        if (nalStart >= annexB.size()) break;
        int nalType = annexB[nalStart] & 0x1F;

        // Next start code (3- or 4-byte) marks the end of this NAL.
        size_t next = nalStart + 1;
        while (next + 2 < annexB.size() &&
               !(annexB[next] == 0 && annexB[next + 1] == 0 && annexB[next + 2] == 1)) {
            ++next;
        }
        size_t nalEnd = (next + 2 < annexB.size()) ? next : annexB.size();

        if (nalType == wantedType) {
            return stripEmulationPrevention(annexB.data() + nalStart + 1, nalEnd - nalStart - 1);
        }
        i = nalEnd;
    }
    return std::nullopt;
}

std::optional<SpsInfo> encodeAndParseSps(int width, int height) {
    H264Encoder encoder;
    if (!encoder.initialize(width, height, 30, 2'000'000)) {
        return std::nullopt;
    }
    auto frame = makeTestFrameBgra(width, height);
    ByteBuffer annexB;
    bool isKeyframe = false;
    if (!encoder.encodeFrame(frame.data(), width, height, annexB, isKeyframe) || !isKeyframe) {
        return std::nullopt; // first frame after initialize() is always an IDR -- see encodeFrame()'s own comment
    }
    auto rbsp = findNalRbsp(annexB, /*wantedType=*/7);
    if (!rbsp) {
        return std::nullopt;
    }
    return parseSpsData(*rbsp);
}

} // namespace

// Real values observed from this project's own H264Encoder (real OpenH264
// underneath, not a mock) at Cemu's GamePad resolution (854x480), recorded
// here as of this test's writing so a future change to H264Encoder's
// configuration that alters them is a deliberate, visible decision rather
// than a silent regression:
//   profile_idc=66 (Baseline) level_idc=31 max_num_ref_frames=1
//   vui_present=1 bitstream_restriction_present=1
//   max_num_reorder_frames=0 max_dec_frame_buffering=1
// Every one of these matches Vanilla's own stated low-latency H.264
// buffering principles exactly (see the 2026-08-26 audit's "H.264 decoder
// buffering" section) -- this test is what turns that from an assumption
// implied by H264Encoder's *configuration* (CAMERA_VIDEO_REAL_TIME, no
// B-frames, a single spatial/temporal layer) into a verified property of
// the actual emitted bitstream.
MDR_TEST(h264_sps_reports_single_ref_frame_and_zero_reorder_buffering) {
    auto sps = encodeAndParseSps(854, 480); // Cemu's real GamePad surface size
    MDR_CHECK(sps.has_value());
    if (!sps) return;

    std::fprintf(stderr,
                  "  SPS: profile_idc=%u level_idc=%u max_num_ref_frames=%u vui_present=%d "
                  "bitstream_restriction_present=%d max_num_reorder_frames=%u max_dec_frame_buffering=%u\n",
                  sps->profileIdc, sps->levelIdc, sps->maxNumRefFrames, sps->vuiPresent,
                  sps->bitstreamRestrictionPresent, sps->maxNumReorderFrames, sps->maxDecFrameBuffering);

    // Baseline profile independently corroborates "no B-frames" from a
    // completely different angle than H264Encoder's own configuration: B
    // slices are not a valid part of the Baseline profile at all (ITU-T
    // H.264 Annex A.2.1), so this isn't just "the encoder chose not to use
    // them this session," it's structurally impossible for this stream to
    // contain one.
    MDR_CHECK_EQ(sps->profileIdc, 66u);

    // Vanilla's stated target: a single reference frame.
    MDR_CHECK_EQ(sps->maxNumRefFrames, 1u);

    // Vanilla's stated targets: max_num_reorder_frames = 0,
    // max_dec_frame_buffering = 1 -- both live inside VUI's
    // bitstream_restriction(), so both presence flags must actually be set
    // for these numbers to mean anything (an absent bitstream_restriction()
    // leaves a decoder to fall back to level-derived defaults instead of
    // this explicit, tighter signal).
    MDR_CHECK(sps->vuiPresent);
    MDR_CHECK(sps->bitstreamRestrictionPresent);
    if (sps->bitstreamRestrictionPresent) {
        MDR_CHECK_EQ(sps->maxNumReorderFrames, 0u);
        MDR_CHECK_EQ(sps->maxDecFrameBuffering, 1u);
    }
}

// Same assertions at a much smaller surface (DS's native 256x192) --
// confirms the properties above aren't an accident specific to Cemu's
// larger GamePad resolution, since OpenH264 could in principle pick a
// different profile/level or omit bitstream_restriction() at a different
// picture size (level_idc in particular is partly a function of picture
// size and frame rate, per ITU-T H.264 Table A-1).
MDR_TEST(h264_sps_properties_hold_at_ds_resolution_too) {
    auto sps = encodeAndParseSps(256, 192);
    MDR_CHECK(sps.has_value());
    if (!sps) return;

    MDR_CHECK_EQ(sps->profileIdc, 66u);
    MDR_CHECK_EQ(sps->maxNumRefFrames, 1u);
    MDR_CHECK(sps->vuiPresent);
    MDR_CHECK(sps->bitstreamRestrictionPresent);
    if (sps->bitstreamRestrictionPresent) {
        MDR_CHECK_EQ(sps->maxNumReorderFrames, 0u);
        MDR_CHECK_EQ(sps->maxDecFrameBuffering, 1u);
    }
}

#else // !DUALDECK_HAVE_OPENH264

MDR_TEST(h264_sps_test_skipped_without_openh264) {
    // Nothing to verify against a bitstream this build can't produce --
    // see H264Encoder::isAvailable()'s own comment. Not a real assertion,
    // just keeps this file from being an empty translation unit when
    // OpenH264 isn't available, matching every other H.264 test file's
    // convention in this project.
    MDR_CHECK(!melonds_remote::host::H264Encoder::isAvailable());
}

#endif // DUALDECK_HAVE_OPENH264
