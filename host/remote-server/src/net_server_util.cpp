#include "net_server_internal.h"

#include "host/net_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <turbojpeg.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>

#include "host/h264_encoder.h"
#include "host/pyrowave_encoder.h"
#include "melonds_remote/protocol.h"

namespace melonds_remote::host::net_detail {


// Monotonic clock: used for timeouts and sequence-number bookkeeping,
// where immunity to wall-clock jumps (NTP corrections, DST, manual clock
// changes) matters more than comparability with the client's clock.
uint64_t nowMicros() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// Wall-clock (epoch) time: the only clock comparable with the client's
// ControllerState.clientTimestampUs (see wallClockNowUs() in
// client/src/main.cpp), so this is used only for the latency estimate in
// inputLoop()'s stats, never for timeout/ordering decisions.
uint64_t nowMicrosEpoch() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
}

// Binds a TCP listening socket to config.bindAddress:port. Returns -1 on
// failure (logged); never falls back to binding all interfaces implicitly.
int makeTcpListener(const std::string& bindAddress, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket (tcp)");
        return -1;
    }

    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, bindAddress.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "invalid bind address: %s\n", bindAddress.c_str());
        ::close(fd);
        return -1;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind (tcp)");
        // Real user report, 2026-08-03: melonDS's own in-process
        // NetServer (this file, vendored into the melonDS patch) and the
        // separate persistent Host Control daemon (dualdeck-host-
        // control.service) are two independent processes that both bind
        // these exact same default client-facing ports -- melonDS isn't
        // wired into the shared-daemon/adapter-IPC model Azahar/Cemu use
        // (a known, deliberately-deferred larger rework, see
        // docs/history.md), so nothing today stops both from
        // trying to claim the same port at once. When that happens here,
        // this melonDS instance silently never starts a server at all
        // while the client, upon connecting, reaches whichever one *did*
        // win the bind -- typically the daemon, since it's usually
        // already running by the time melonDS launches -- landing the
        // client in Host Control mode instead of the melonDS session the
        // user actually wanted, with no obvious explanation from the
        // client side alone. EADDRINUSE specifically (not just any bind
        // failure) is exactly this scenario; call it out by name so
        // whoever reads this line (interactively, or via journalctl if
        // launched under systemd) doesn't have to guess.
        if (errno == EADDRINUSE) {
            std::fprintf(stderr,
                          "NetServer: port %u is already in use -- if the persistent Host Control "
                          "daemon (dualdeck-host-control.service) is running, stop it first "
                          "(systemctl --user stop dualdeck-host-control.service), then relaunch.\n",
                          port);
        }
        ::close(fd);
        return -1;
    }

    if (::listen(fd, 1) < 0) {
        std::perror("listen");
        ::close(fd);
        return -1;
    }

    return fd;
}

int makeUdpSocket(const std::string& bindAddress, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::perror("socket (udp)");
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, bindAddress.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "invalid bind address: %s\n", bindAddress.c_str());
        ::close(fd);
        return -1;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind (udp)");
        // See makeTcpListener()'s identical EADDRINUSE handling above --
        // same melonDS-vs-persistent-daemon port conflict, just on the
        // UDP input port instead of the TCP control/video ones.
        if (errno == EADDRINUSE) {
            std::fprintf(stderr,
                          "NetServer: port %u is already in use -- if the persistent Host Control "
                          "daemon (dualdeck-host-control.service) is running, stop it first "
                          "(systemctl --user stop dualdeck-host-control.service), then relaunch.\n",
                          port);
        }
        ::close(fd);
        return -1;
    }

    return fd;
}

// Constant-time-ish string comparison: always compares the same number of
// bytes (padding the shorter string's "missing" bytes into the mismatch
// accumulator) so a client can't use response timing to learn how many
// leading bytes of the auth token it guessed correctly.
bool constantTimeEquals(const std::string& a, const std::string& b) {
    size_t maxLen = std::max(a.size(), b.size());
    unsigned char diff = static_cast<unsigned char>(a.size() != b.size());
    for (size_t i = 0; i < maxLen; ++i) {
        unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff = static_cast<unsigned char>(diff | (ca ^ cb));
    }
    return diff == 0;
}

// JPEG-compresses one raw BGRA8888 frame (protocol v8, see protocol.h's
// kProtocolVersion comment). `compressor` is reused across calls -- see
// videoLoop()'s comment on why -- so this function is only ever called
// from that one thread. TJPF_BGRA as both input format and (implicitly,
// via JCS_EXT_BGRA under the hood) how libjpeg-turbo reads it means no
// manual channel reordering is needed, matching this project's existing
// BGRA8888-everywhere convention. Returns false (leaving outJpeg
// untouched) on a compression failure, which should only happen for a
// malformed width/height.
bool compressFrameBgraToJpeg(tjhandle compressor, const uint8_t* bgra, int width, int height,
                              int quality, ByteBuffer& outJpeg) {
    // 4:2:0 chroma subsampling (half resolution in both chroma axes) is
    // invisible at typical photo/game content and quality settings, but
    // real-world feedback on DS/3DS -- small enough surfaces that
    // bandwidth was never the constraint compression exists for -- found
    // it visibly softens sharp pixel-art edges and text even at quality
    // 100, since subsampling is a structural choice independent of the
    // quality scalar. At a high requested quality (client is explicitly
    // asking for close-to-lossless, protocol v9's HelloPayload::
    // videoQuality), use full-resolution 4:4:4 chroma instead -- still
    // cheap at these frame sizes -- so "quality" actually reaches
    // full fidelity rather than being capped by subsampling artifacts.
    const TJSAMP subsampling = quality >= 90 ? TJSAMP_444 : TJSAMP_420;
    unsigned char* jpegBuf = nullptr;
    unsigned long jpegSize = 0;
    int result = tjCompress2(compressor, bgra, width, /*pitch=*/0, height, TJPF_BGRA,
                              &jpegBuf, &jpegSize, subsampling, quality, TJFLAG_FASTDCT);
    if (result != 0) {
        std::fprintf(stderr, "NetServer: tjCompress2 failed: %s\n", tjGetErrorStr2(compressor));
        return false;
    }
    outJpeg.assign(jpegBuf, jpegBuf + jpegSize);
    tjFree(jpegBuf);
    return true;
}

bool sendAll(int fd, const uint8_t* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        ssize_t n = ::send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Intersects the client's advertised HelloPayload::supportedVideoCodecs
// against what this host build can actually encode, preferring the most
// bandwidth-efficient codec both sides agree on. VideoCodecBit_H264 is
// only ever in hostSupportedCodecs when this build actually has OpenH264
// (see H264Encoder::isAvailable()) -- a build without it (or a client
// that, like every client shipped so far, only ever advertises JPEG
// support) still runs exactly the same JPEG path this project always
// has. videoLoop() is what actually branches on the result -- see its
// own H264Encoder usage.
//
// PyroWave wins over H.264 when both are agreed: the client only ever
// advertises it when its user explicitly picked it (see NetClientConfig::
// preferPyroWave), i.e. chose lowest latency over lowest bandwidth.
// PyroWaveEncoder::isAvailable() is checked only once the client has
// actually asked for it -- its first call probes for a capable Vulkan
// device (see its own comment), which no other session should pay for.
VideoCodec selectVideoCodec(uint8_t clientSupportedCodecs) {
    if ((clientSupportedCodecs & kVideoCodecBit_PyroWave) && PyroWaveEncoder::isAvailable()) {
        return VideoCodec::PyroWave;
    }
    const uint8_t hostSupportedCodecs =
        kVideoCodecBit_Jpeg | (H264Encoder::isAvailable() ? kVideoCodecBit_H264 : 0);
    const uint8_t agreed = clientSupportedCodecs & hostSupportedCodecs;
    if (agreed & kVideoCodecBit_H264) {
        return VideoCodec::H264;
    }
    // Jpeg is the permanent fallback: always in hostSupportedCodecs, so
    // this is only ever reached by "agreed" being 0 (a client that
    // somehow advertised no codecs at all, or a not-yet-mutually-
    // supported future codec) or Jpeg itself being the actual agreement.
    return VideoCodec::Jpeg;
}

// A first-pass H.264 bitrate default, deliberately reusing the existing
// currentVideoQuality_ (1-100, originally a JPEG-quality scale -- see
// its own comment) rather than inventing a wholly separate H.264-only
// control axis before any real hardware testing exists to say whether
// that's actually needed. Maps the quality scale onto roughly
// 0.05-0.30 bits/pixel/frame (a plausible middle-ground range for H.264
// at real-time/low-latency tuning, not empirically measured against
// this project's actual video content yet -- see docs/history.md's
// 2026-08-25 H.264 entries) and scales by resolution and frame rate to
// get a target bits-per-second the encoder's RC_BITRATE_MODE rate
// control aims for.
int h264TargetBitrateBps(int quality, uint16_t width, uint16_t height, int fps) {
    const double clampedQuality = std::clamp(quality, 1, 100) / 100.0;
    const double bitsPerPixelPerFrame = 0.05 + clampedQuality * 0.25;
    const double bitrate =
        static_cast<double>(width) * static_cast<double>(height) * bitsPerPixelPerFrame * fps;
    return static_cast<int>(std::clamp(bitrate, 250'000.0, 20'000'000.0));
}

// PyroWave's rate control is an exact per-frame byte ceiling rather than
// a bitrate target (intra-only: every frame stands alone, so there's no
// GOP to average over), derived from the same 1-100 quality scale as
// h264TargetBitrateBps() above. PyroWave's trivial entropy coding needs
// far more bits than H.264 for the same quality -- its own README
// targets ~200 Mbit/s at 1080p60, about 1.6 bits/pixel/frame -- so this
// maps quality onto 0.5-3.0 bits/pixel/frame. At Cemu's 854x480 GamePad
// and the large-surface default quality of 60 (see
// defaultVideoQualityForFrameSize()) that's ~100 KB/frame, ~25 Mbit/s at
// 30fps; DS's 256x192 at the default 80 is ~15 KB/frame. A first-pass
// mapping, not tuned against real game content yet, same caveat as
// h264TargetBitrateBps(). Floored so a tiny test-fixture frame still
// leaves room for PyroWave's per-block headers.
size_t pyrowaveMaxFrameBytes(int quality, uint16_t width, uint16_t height) {
    const double clampedQuality = std::clamp(quality, 1, 100) / 100.0;
    const double bitsPerPixelPerFrame = 0.5 + clampedQuality * 2.5;
    const double bytes = static_cast<double>(width) * static_cast<double>(height) * bitsPerPixelPerFrame / 8.0;
    return std::max<size_t>(static_cast<size_t>(bytes), 4096);
}


} // namespace melonds_remote::host::net_detail
