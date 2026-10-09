#include "pyrowave_decoder.h"

#ifdef DUALDECK_HAVE_PYROWAVE

#include <vulkan/vulkan.h>
// See host/remote-server/src/pyrowave_encoder.cpp's identical alias for
// why this exists (pre-1.4 system Vulkan headers).
#ifndef VK_VERSION_1_4
typedef VkQueueGlobalPriorityKHR VkQueueGlobalPriority;
#endif
#include <pyrowave.h>

#include <cstdio>
#include <mutex>

#include "yuv_conversion.h"

namespace melonds_remote::client {

namespace {

struct SequenceHeader {
    int width = 0;
    int height = 0;
    bool chroma444 = false;
    uint32_t totalBlocks = 0;
};

uint32_t readU32Le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// Parses the BitstreamSequenceHeader that host::PyroWaveEncoder always
// puts first in every frame (pyrowave_encoder_packetize() writes it
// before any coefficient block). Field layout per PyroWave's frozen
// bitstream v1 spec (bitstream/bitstream.md), two little-endian u32s:
//   word 0: width_minus_1:14, height_minus_1:14, sequence:3, extended:1
//   word 1: total_blocks:24, code:2, chroma_resolution:1, ...
// `extended` must be 1 and `code` must be 0 (START_OF_FRAME) -- anything
// else means this isn't a frame this project's encoder produced.
bool parseSequenceHeader(const uint8_t* data, size_t size, SequenceHeader& out) {
    if (size < 8) {
        return false;
    }
    const uint32_t w0 = readU32Le(data);
    const uint32_t w1 = readU32Le(data + 4);
    const bool extended = (w0 >> 31) & 1u;
    const uint32_t code = (w1 >> 24) & 0x3u;
    if (!extended || code != 0) {
        return false;
    }
    out.width = static_cast<int>(w0 & 0x3FFFu) + 1;
    out.height = static_cast<int>((w0 >> 14) & 0x3FFFu) + 1;
    out.chroma444 = ((w1 >> 26) & 1u) != 0;
    out.totalBlocks = w1 & 0xFFFFFFu;
    return true;
}

} // namespace

struct PyroWaveDecoder::Impl {
    pyrowave_device device = nullptr;
    pyrowave_decoder decoder = nullptr;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> yPlane, uPlane, vPlane;

    void destroyDecoder() {
        if (decoder) {
            pyrowave_decoder_destroy(decoder);
            decoder = nullptr;
        }
        width = height = 0;
    }

    ~Impl() {
        destroyDecoder();
        if (device) {
            pyrowave_device_destroy(device);
        }
    }
};

PyroWaveDecoder::PyroWaveDecoder() : impl_(std::make_unique<Impl>()) {}
PyroWaveDecoder::~PyroWaveDecoder() = default;

bool PyroWaveDecoder::isAvailable() {
    static std::once_flag probed;
    static bool available = false;
    std::call_once(probed, [] {
        pyrowave_device device = nullptr;
        if (pyrowave_create_default_device(&device) != PYROWAVE_SUCCESS || !device) {
            std::fprintf(stderr, "PyroWaveDecoder: no usable Vulkan device, PyroWave decoding unavailable\n");
            return;
        }
        pyrowave_decoder_create_info info{};
        info.device = device;
        info.width = 64;
        info.height = 64;
        info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
        pyrowave_decoder decoder = nullptr;
        if (pyrowave_decoder_create(&info, &decoder) == PYROWAVE_SUCCESS && decoder) {
            available = true;
            pyrowave_decoder_destroy(decoder);
        } else {
            std::fprintf(stderr, "PyroWaveDecoder: Vulkan device can't run PyroWave's decoder, "
                                 "PyroWave decoding unavailable\n");
        }
        pyrowave_device_destroy(device);
    });
    return available;
}

bool PyroWaveDecoder::decodeFrame(const uint8_t* data, size_t size, std::vector<uint8_t>& outBgra, int& outWidth,
                                   int& outHeight, bool& outHasFrame) {
    outHasFrame = false;
    SequenceHeader header;
    // 4:4:4 is valid PyroWave but never produced by host::PyroWaveEncoder
    // (4:2:0 only), and 4:2:0 requires even dimensions -- reject both
    // rather than handing the library something it would refuse anyway.
    if (!parseSequenceHeader(data, size, header) || header.chroma444 || (header.width & 1) || (header.height & 1)) {
        return false;
    }

    const size_t lumaStride = static_cast<size_t>(header.width);
    const size_t chromaStride = lumaStride / 2;
    const size_t chromaRows = static_cast<size_t>(header.height) / 2;

    // A frame declaring zero coefficient blocks (a perfectly flat
    // mid-level image can quantize to this) has an output the bitstream
    // spec fully defines: every coefficient is 0, so after the DC shift
    // every sample is mid-scale. Produced directly rather than through
    // the GPU because real drivers disagreed on it -- real report,
    // 2026-10-09: lavapipe decoded such a frame to flat mid-gray, but on
    // a Radeon GPU the result wasn't gray (most likely the readback
    // planes were never written when there was nothing to decode, though
    // that wasn't confirmed), which would show stale/garbage pixels.
    if (header.totalBlocks == 0) {
        impl_->yPlane.assign(lumaStride * static_cast<size_t>(header.height), 128);
        impl_->uPlane.assign(chromaStride * chromaRows, 128);
        impl_->vPlane.assign(chromaStride * chromaRows, 128);
        i420ToBgra(impl_->yPlane.data(), static_cast<int>(lumaStride), impl_->uPlane.data(), impl_->vPlane.data(),
                   static_cast<int>(chromaStride), header.width, header.height, outBgra);
        outWidth = header.width;
        outHeight = header.height;
        outHasFrame = true;
        return true;
    }

    if (!impl_->device && pyrowave_create_default_device(&impl_->device) != PYROWAVE_SUCCESS) {
        impl_->device = nullptr;
        return false;
    }
    if (!impl_->decoder || header.width != impl_->width || header.height != impl_->height) {
        impl_->destroyDecoder();
        pyrowave_decoder_create_info info{};
        info.device = impl_->device;
        info.width = header.width;
        info.height = header.height;
        info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
        if (pyrowave_decoder_create(&info, &impl_->decoder) != PYROWAVE_SUCCESS || !impl_->decoder) {
            impl_->decoder = nullptr;
            return false;
        }
        impl_->width = header.width;
        impl_->height = header.height;
    }

    // One push for the whole frame: pyrowave_decoder_push_packet() walks
    // every block header in the buffer on its own (each one carries its
    // own length), so the host's single-packet framing needs no
    // splitting back apart here.
    if (pyrowave_decoder_push_packet(impl_->decoder, data, size) != PYROWAVE_SUCCESS) {
        return false;
    }
    if (!pyrowave_decoder_decode_is_ready(impl_->decoder, false)) {
        return true;
    }

    impl_->yPlane.resize(lumaStride * static_cast<size_t>(impl_->height));
    impl_->uPlane.resize(chromaStride * chromaRows);
    impl_->vPlane.resize(chromaStride * chromaRows);

    pyrowave_cpu_buffer buffer{};
    buffer.data[0] = impl_->yPlane.data();
    buffer.data[1] = impl_->uPlane.data();
    buffer.data[2] = impl_->vPlane.data();
    buffer.row_stride_in_bytes[0] = lumaStride;
    buffer.row_stride_in_bytes[1] = buffer.row_stride_in_bytes[2] = chromaStride;
    buffer.plane_size_in_bytes[0] = impl_->yPlane.size();
    buffer.plane_size_in_bytes[1] = buffer.plane_size_in_bytes[2] = impl_->uPlane.size();
    buffer.width = impl_->width;
    buffer.height = impl_->height;
    buffer.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    if (pyrowave_decoder_decode_cpu_buffer_synchronous(impl_->decoder, &buffer) != PYROWAVE_SUCCESS) {
        return false;
    }

    i420ToBgra(impl_->yPlane.data(), static_cast<int>(lumaStride), impl_->uPlane.data(), impl_->vPlane.data(),
               static_cast<int>(chromaStride), impl_->width, impl_->height, outBgra);
    outWidth = impl_->width;
    outHeight = impl_->height;
    outHasFrame = true;
    return true;
}

} // namespace melonds_remote::client

#else // !DUALDECK_HAVE_PYROWAVE

namespace melonds_remote::client {

// Built without PyroWave -- same "always compiled, real work stubbed
// out" pattern as h264_decoder.cpp's own no-OpenH264 build.
struct PyroWaveDecoder::Impl {};

PyroWaveDecoder::PyroWaveDecoder() : impl_(std::make_unique<Impl>()) {}
PyroWaveDecoder::~PyroWaveDecoder() = default;

bool PyroWaveDecoder::isAvailable() { return false; }
bool PyroWaveDecoder::decodeFrame(const uint8_t*, size_t, std::vector<uint8_t>&, int&, int&, bool& outHasFrame) {
    outHasFrame = false;
    return false;
}

} // namespace melonds_remote::client

#endif // DUALDECK_HAVE_PYROWAVE
