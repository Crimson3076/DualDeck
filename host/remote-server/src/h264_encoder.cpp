#include "host/h264_encoder.h"

#ifdef DUALDECK_HAVE_OPENH264

#include <wels/codec_api.h>

#include <chrono>
#include <cstring>
#include <vector>

#include "host/yuv_conversion.h"

namespace dualdeck::host {

struct H264Encoder::Impl {
    ISVCEncoder* encoder = nullptr;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> yPlane, uPlane, vPlane;

    void destroy() {
        if (encoder) {
            encoder->Uninitialize();
            WelsDestroySVCEncoder(encoder);
            encoder = nullptr;
        }
    }

    ~Impl() { destroy(); }
};

H264Encoder::H264Encoder() : impl_(std::make_unique<Impl>()) {}
H264Encoder::~H264Encoder() = default;

bool H264Encoder::isAvailable() { return true; }

bool H264Encoder::initialize(int width, int height, int fps, int targetBitrateBps) {
    impl_->destroy();
    impl_->width = 0;
    impl_->height = 0;
    if (width <= 0 || height <= 0 || fps <= 0) {
        return false;
    }

    if (WelsCreateSVCEncoder(&impl_->encoder) != 0 || !impl_->encoder) {
        impl_->encoder = nullptr;
        return false;
    }

    SEncParamExt param;
    std::memset(&param, 0, sizeof(param));
    impl_->encoder->GetDefaultParams(&param);
    param.iUsageType = CAMERA_VIDEO_REAL_TIME;
    param.iPicWidth = width;
    param.iPicHeight = height;
    param.iTargetBitrate = targetBitrateBps;
    param.fMaxFrameRate = static_cast<float>(fps);
    param.iRCMode = RC_BITRATE_MODE;
    // Real-time streaming tuning, not offline max-compression encoding:
    // a single temporal/spatial layer (no scalable-video-coding
    // complexity this point-to-point link has any use for -- OpenH264's
    // encoder never uses B-frames at all regardless of this setting, so
    // there's no separate B-frame knob to disable here), and a keyframe
    // roughly every 2 seconds so a client that (re)joins mid-session
    // isn't stuck waiting long for a decodable frame -- requestKeyframe()
    // below covers the "right now" case explicitly.
    //
    // bEnableFrameSkip = false. It was true to silence OpenH264's
    // "bitrate can't be controlled ... without enabling skip frame"
    // warning, but with it on, any scene busier than the bitrate target
    // makes rate control drop frames outright: measured with OpenH264
    // 2.4 on synthetic 854x480 moving content at quality 60's target, 208-267 of
    // 300 frames came out, i.e. a 30fps Cemu GamePad stream arrived at
    // ~21-27fps -- the Wii U "24-25fps" report. The video loop then
    // re-encoded the skipped frame on its next tick, so the link carried
    // above-target bitrate anyway, just with fewer frames and more
    // encoder time. With skip off, rate control holds the bitrate by
    // raising QP instead (measured: same bytes on the wire, every frame
    // delivered). A slow link is still bounded by the video socket's
    // small SO_SNDBUF (see net_server_video.cpp), not by dropped frames.
    param.iTemporalLayerNum = 1;
    param.iSpatialLayerNum = 1;
    param.bEnableFrameSkip = false;
    param.uiIntraPeriod = static_cast<unsigned int>(fps * 2);
    param.eSpsPpsIdStrategy = CONSTANT_ID;

    if (impl_->encoder->InitializeExt(&param) != 0) {
        impl_->destroy();
        return false;
    }

    int videoFormat = videoFormatI420;
    impl_->encoder->SetOption(ENCODER_OPTION_DATAFORMAT, &videoFormat);

    impl_->width = width;
    impl_->height = height;
    return true;
}

void H264Encoder::requestKeyframe() {
    if (impl_->encoder) {
        impl_->encoder->ForceIntraFrame(true);
    }
}

bool H264Encoder::encodeFrame(const uint8_t* bgra, int width, int height, ByteBuffer& outAnnexB,
                               bool& outIsKeyframe) {
    if (!impl_->encoder || width != impl_->width || height != impl_->height) {
        // A resolution mismatch here means the caller forgot to call
        // initialize() again on a real resolution change (see that
        // method's own comment) -- this is a safety net, not the
        // intended way to handle it: feeding a mismatched-size frame
        // into an encoder still configured for the old size would
        // otherwise read past the source buffer.
        return false;
    }

    bgraToI420(bgra, width, height, impl_->yPlane, impl_->uPlane, impl_->vPlane);

    SSourcePicture pic;
    std::memset(&pic, 0, sizeof(pic));
    pic.iPicWidth = width;
    pic.iPicHeight = height;
    pic.iColorFormat = videoFormatI420;
    pic.iStride[0] = width;
    pic.iStride[1] = pic.iStride[2] = (width + 1) / 2;
    pic.pData[0] = impl_->yPlane.data();
    pic.pData[1] = impl_->uPlane.data();
    pic.pData[2] = impl_->vPlane.data();
    // Rate control paces itself off these (milliseconds); left at 0 it
    // has no idea how much time passed between frames.
    pic.uiTimeStamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();

    SFrameBSInfo info;
    std::memset(&info, 0, sizeof(info));
    if (impl_->encoder->EncodeFrame(&pic, &info) != cmResultSuccess) {
        return false;
    }
    if (info.eFrameType == videoFrameTypeSkip) {
        // Rate control decided to skip this tick entirely -- nothing to
        // send, not a failure.
        outIsKeyframe = false;
        return true;
    }

    outIsKeyframe = (info.eFrameType == videoFrameTypeIDR);
    for (int layer = 0; layer < info.iLayerNum; ++layer) {
        const SLayerBSInfo& layerInfo = info.sLayerInfo[layer];
        size_t layerSize = 0;
        for (int nal = 0; nal < layerInfo.iNalCount; ++nal) {
            layerSize += static_cast<size_t>(layerInfo.pNalLengthInByte[nal]);
        }
        outAnnexB.insert(outAnnexB.end(), layerInfo.pBsBuf, layerInfo.pBsBuf + layerSize);
    }
    return true;
}

} // namespace dualdeck::host

#else // !DUALDECK_HAVE_OPENH264

namespace dualdeck::host {

// This build was configured without OpenH264 (see host/remote-server/
// CMakeLists.txt's optional detection) -- every method is a no-op that
// reports unavailable, matching x11_screen_capture.cpp's/
// wayland_screen_capture.cpp's identical "always compiled, real work
// stubbed out" pattern for their own optional dependencies.
struct H264Encoder::Impl {};

H264Encoder::H264Encoder() : impl_(std::make_unique<Impl>()) {}
H264Encoder::~H264Encoder() = default;

bool H264Encoder::isAvailable() { return false; }
bool H264Encoder::initialize(int, int, int, int) { return false; }
void H264Encoder::requestKeyframe() {}
bool H264Encoder::encodeFrame(const uint8_t*, int, int, ByteBuffer&, bool&) { return false; }

} // namespace dualdeck::host

#endif // DUALDECK_HAVE_OPENH264
