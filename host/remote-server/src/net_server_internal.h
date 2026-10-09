#pragma once

// Helpers shared by NetServer's translation units (net_server.cpp,
// net_server_control.cpp, net_server_video.cpp). Internal to
// dualdeck_host; not part of the public include/ API.

#include <turbojpeg.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "dualdeck/protocol.h"

namespace dualdeck::host::net_detail {

uint64_t nowMicros();

uint64_t nowMicrosEpoch();

int makeTcpListener(const std::string& bindAddress, uint16_t port);

int makeUdpSocket(const std::string& bindAddress, uint16_t port);

bool constantTimeEquals(const std::string& a, const std::string& b);

bool compressFrameBgraToJpeg(tjhandle compressor, const uint8_t* bgra, int width, int height,
                              int quality, ByteBuffer& outJpeg);

bool sendAll(int fd, const uint8_t* data, size_t size);

VideoCodec selectVideoCodec(uint8_t clientSupportedCodecs);

int h264TargetBitrateBps(int quality, uint16_t width, uint16_t height, int fps);

size_t pyrowaveMaxFrameBytes(int quality, uint16_t width, uint16_t height);

} // namespace dualdeck::host::net_detail
