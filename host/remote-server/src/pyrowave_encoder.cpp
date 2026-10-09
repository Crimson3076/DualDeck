#include "host/pyrowave_encoder.h"

#ifdef DUALDECK_HAVE_PYROWAVE

#include <vulkan/vulkan.h>
// pyrowave.h names the Vulkan 1.4 core type VkQueueGlobalPriority, which
// older system headers (Ubuntu 24.04's 1.3.275, for one) only have as the
// ABI-identical VkQueueGlobalPriorityKHR -- alias it rather than requiring
// newer Vulkan headers than the distro ships just for a parameter type
// this file never even passes (pyrowave_create_default_device() picks its
// own priority).
#ifndef VK_VERSION_1_4
typedef VkQueueGlobalPriorityKHR VkQueueGlobalPriority;
#endif
#include <pyrowave.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "host/yuv_conversion.h"

namespace dualdeck::host {

namespace {

// Fills `dst` (dstWidth x dstHeight, dstWidth >= srcWidth, dstHeight >=
// srcHeight) from `src` (srcWidth x srcHeight, tightly packed),
// edge-replicating the last column/row into any extra space -- the same
// clamp-to-edge extension PyroWave's own bitstream spec already applies
// internally past an image's edge, so this adds no new artifact of its
// own beyond the one extra duplicated pixel.
void padPlane(const std::vector<uint8_t>& src, int srcWidth, int srcHeight, std::vector<uint8_t>& dst,
              int dstWidth, int dstHeight) {
    dst.resize(static_cast<size_t>(dstWidth) * static_cast<size_t>(dstHeight));
    for (int y = 0; y < dstHeight; ++y) {
        const uint8_t* srcRow = src.data() + static_cast<size_t>(std::min(y, srcHeight - 1)) * static_cast<size_t>(srcWidth);
        uint8_t* dstRow = dst.data() + static_cast<size_t>(y) * static_cast<size_t>(dstWidth);
        std::memcpy(dstRow, srcRow, static_cast<size_t>(srcWidth));
        std::fill(dstRow + srcWidth, dstRow + dstWidth, srcRow[srcWidth - 1]);
    }
}

} // namespace

struct PyroWaveEncoder::Impl {
    pyrowave_device device = nullptr;
    pyrowave_encoder encoder = nullptr;
    // Source (pre-padding) size, what encodeFrame() callers pass.
    int width = 0;
    int height = 0;
    // Even-rounded size the PyroWave encoder itself was created at.
    int encodeWidth = 0;
    int encodeHeight = 0;
    size_t maxFrameBytes = 0;
    std::vector<uint8_t> yPlane, uPlane, vPlane, paddedY;
    std::vector<pyrowave_packet> packets;

    void destroyEncoder() {
        if (encoder) {
            pyrowave_encoder_destroy(encoder);
            encoder = nullptr;
        }
        width = height = encodeWidth = encodeHeight = 0;
    }

    ~Impl() {
        destroyEncoder();
        if (device) {
            pyrowave_device_destroy(device);
        }
    }
};

PyroWaveEncoder::PyroWaveEncoder() : impl_(std::make_unique<Impl>()) {}
PyroWaveEncoder::~PyroWaveEncoder() = default;

bool PyroWaveEncoder::isAvailable() {
    static std::once_flag probed;
    static bool available = false;
    std::call_once(probed, [] {
        pyrowave_device device = nullptr;
        if (pyrowave_create_default_device(&device) != PYROWAVE_SUCCESS || !device) {
            std::fprintf(stderr, "PyroWaveEncoder: no usable Vulkan device, PyroWave encoding unavailable\n");
            return;
        }
        pyrowave_encoder_create_info info{};
        info.device = device;
        info.width = 64;
        info.height = 64;
        info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
        pyrowave_encoder encoder = nullptr;
        if (pyrowave_encoder_create(&info, &encoder) == PYROWAVE_SUCCESS && encoder) {
            available = true;
            pyrowave_encoder_destroy(encoder);
        } else {
            // The real-world failure here is a GPU (or software
            // rasterizer, e.g. lavapipe) that can't force the subgroup
            // sizes PyroWave's encoder shaders need -- see this class's
            // header comment.
            std::fprintf(stderr, "PyroWaveEncoder: Vulkan device can't run PyroWave's encoder, "
                                 "PyroWave encoding unavailable\n");
        }
        pyrowave_device_destroy(device);
    });
    return available;
}

bool PyroWaveEncoder::initialize(int width, int height, size_t maxFrameBytes) {
    impl_->destroyEncoder();
    if (width <= 0 || height <= 0 || maxFrameBytes == 0) {
        return false;
    }
    if (!impl_->device && pyrowave_create_default_device(&impl_->device) != PYROWAVE_SUCCESS) {
        impl_->device = nullptr;
        return false;
    }

    const int encodeWidth = (width + 1) & ~1;
    const int encodeHeight = (height + 1) & ~1;
    pyrowave_encoder_create_info info{};
    info.device = impl_->device;
    info.width = encodeWidth;
    info.height = encodeHeight;
    info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
    if (pyrowave_encoder_create(&info, &impl_->encoder) != PYROWAVE_SUCCESS || !impl_->encoder) {
        impl_->encoder = nullptr;
        return false;
    }

    impl_->width = width;
    impl_->height = height;
    impl_->encodeWidth = encodeWidth;
    impl_->encodeHeight = encodeHeight;
    impl_->maxFrameBytes = maxFrameBytes;
    return true;
}

bool PyroWaveEncoder::encodeFrame(const uint8_t* bgra, int width, int height, ByteBuffer& outBitstream) {
    if (!impl_->encoder || width != impl_->width || height != impl_->height) {
        // Same safety net as H264Encoder::encodeFrame()'s identical
        // check -- a mismatched size would read past the source buffer.
        return false;
    }

    bgraToI420(bgra, width, height, impl_->yPlane, impl_->uPlane, impl_->vPlane);
    // Chroma planes are already ((w+1)/2) x ((h+1)/2) -- exactly the
    // padded size's chroma dimensions -- so only an odd-sized luma plane
    // needs the extra edge-replicated column/row.
    const uint8_t* lumaData = impl_->yPlane.data();
    if (impl_->encodeWidth != width || impl_->encodeHeight != height) {
        padPlane(impl_->yPlane, width, height, impl_->paddedY, impl_->encodeWidth, impl_->encodeHeight);
        lumaData = impl_->paddedY.data();
    }

    const size_t lumaStride = static_cast<size_t>(impl_->encodeWidth);
    const size_t chromaStride = static_cast<size_t>(impl_->encodeWidth / 2);
    const size_t chromaRows = static_cast<size_t>(impl_->encodeHeight / 2);
    pyrowave_cpu_buffer buffer{};
    buffer.data[0] = const_cast<uint8_t*>(lumaData);
    buffer.data[1] = impl_->uPlane.data();
    buffer.data[2] = impl_->vPlane.data();
    buffer.row_stride_in_bytes[0] = lumaStride;
    buffer.row_stride_in_bytes[1] = buffer.row_stride_in_bytes[2] = chromaStride;
    buffer.plane_size_in_bytes[0] = lumaStride * static_cast<size_t>(impl_->encodeHeight);
    buffer.plane_size_in_bytes[1] = buffer.plane_size_in_bytes[2] = chromaStride * chromaRows;
    buffer.width = impl_->encodeWidth;
    buffer.height = impl_->encodeHeight;
    buffer.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;

    pyrowave_rate_control rateControl{};
    rateControl.maximum_bitstream_size = impl_->maxFrameBytes;
    if (pyrowave_encoder_encode_cpu(impl_->encoder, &buffer, &rateControl) != PYROWAVE_SUCCESS) {
        return false;
    }

    // PyroWave's packetizer is built for splitting a frame across
    // MTU-sized UDP datagrams; this project's video channel is a TCP
    // stream that already carries one whole frame per VideoFrame packet,
    // so a packet boundary nothing can exceed yields exactly one packet
    // holding the entire frame. Its output buffer isn't bounds-checked
    // in release builds of PyroWave, so it's sized from the encoder's
    // own raw bitstream buffer (an upper bound on every block it could
    // copy out) plus the 8-byte start-of-frame header packetize()
    // prepends, rather than trusting the rate-control target alone.
    const void* rawBitstream = nullptr;
    const void* rawMeta = nullptr;
    size_t rawBitstreamSize = 0;
    size_t rawMetaSize = 0;
    if (pyrowave_encoder_get_mapped_raw_bitstream(impl_->encoder, &rawBitstream, &rawBitstreamSize, &rawMeta,
                                                  &rawMetaSize) != PYROWAVE_SUCCESS) {
        return false;
    }
    constexpr size_t kSinglePacketBoundary = SIZE_MAX / 2;
    size_t numPackets = 0;
    if (pyrowave_encoder_compute_num_packets(impl_->encoder, kSinglePacketBoundary, &numPackets) != PYROWAVE_SUCCESS) {
        return false;
    }
    impl_->packets.resize(std::max<size_t>(numPackets, 1));

    const size_t startOffset = outBitstream.size();
    outBitstream.resize(startOffset + rawBitstreamSize + 64);
    size_t writtenPackets = 0;
    if (pyrowave_encoder_packetize(impl_->encoder, impl_->packets.data(), kSinglePacketBoundary, &writtenPackets,
                                   outBitstream.data() + startOffset, rawBitstreamSize + 64) != PYROWAVE_SUCCESS ||
        writtenPackets == 0) {
        outBitstream.resize(startOffset);
        return false;
    }
    // Packets are laid out back to back starting at offset 0, so the
    // last one's end is the frame's total size.
    const pyrowave_packet& last = impl_->packets[writtenPackets - 1];
    outBitstream.resize(startOffset + last.offset + last.size);
    return true;
}

} // namespace dualdeck::host

#else // !DUALDECK_HAVE_PYROWAVE

namespace dualdeck::host {

// Built without PyroWave (see host/remote-server/CMakeLists.txt's
// optional detection) -- same "always compiled, real work stubbed out"
// pattern as h264_encoder.cpp's own no-OpenH264 build.
struct PyroWaveEncoder::Impl {};

PyroWaveEncoder::PyroWaveEncoder() : impl_(std::make_unique<Impl>()) {}
PyroWaveEncoder::~PyroWaveEncoder() = default;

bool PyroWaveEncoder::isAvailable() { return false; }
bool PyroWaveEncoder::initialize(int, int, size_t) { return false; }
bool PyroWaveEncoder::encodeFrame(const uint8_t*, int, int, ByteBuffer&) { return false; }

} // namespace dualdeck::host

#endif // DUALDECK_HAVE_PYROWAVE
