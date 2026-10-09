#include "host/h264_encoder.h"

#ifdef DUALDECK_HAVE_OPENH264

#include <wels/codec_api.h>

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
    // bEnableFrameSkip = true: real, OpenH264-reported requirement, not
    // a style choice -- first shipped as `false` on the (wrong) reasoning
    // that skipping frames would hurt latency, which drew OpenH264's own
    // runtime warning ("bitrate can't be controlled for ... RC_BITRATE_MODE
    // ... without enabling skip frame"). Frame skip is unrelated to
    // B-frame lookahead: it lets the encoder drop an occasional frame to
    // stay within iTargetBitrate under sustained pressure, which is
    // exactly the "bounded, not unbounded, added latency under a slow
    // link" outcome this project already chose for JPEG (see net_server.cpp's
    // SO_SNDBUF sizing) -- disabled, RC_BITRATE_MODE can't actually
    // enforce the target bitrate at all, letting a congested link's queue
    // grow the same unbounded way the pre-SO_SNDBUF JPEG path did.
    param.iTemporalLayerNum = 1;
    param.iSpatialLayerNum = 1;
    param.bEnableFrameSkip = true;
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
