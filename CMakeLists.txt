cmake_minimum_required(VERSION 3.20)
project(dualdeck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DUALDECK_BUILD_TESTS "Build unit tests" ON)
option(DUALDECK_BUILD_CLIENT "Build the SDL3 Steam Deck client (requires SDL3)" OFF)
option(DUALDECK_BUILD_HOST "Build the standalone host remote-server prototype" ON)
option(DUALDECK_ENABLE_WARNINGS "Enable strict warnings (-Wall -Wextra -Wpedantic -Wconversion -Wshadow)" ON)

enable_testing()

# libjpeg-turbo's turbojpeg API -- used by host/remote-server/src/net_server.cpp
# to compress video frames and client/src/net_client.cpp to decompress them
# (protocol v8, see protocol.h). Found once here rather than in each
# CMakeLists.txt that needs it, since both the host and client link it.
# find_path()/find_library() rather than pkg_check_modules() or
# find_package(), since neither a CMake config file nor a guaranteed .pc
# name can be assumed across Debian/Fedora/Arch.
find_path(TURBOJPEG_INCLUDE_DIR NAMES turbojpeg.h)
find_library(TURBOJPEG_LIBRARY NAMES turbojpeg)
if(NOT TURBOJPEG_INCLUDE_DIR OR NOT TURBOJPEG_LIBRARY)
    message(FATAL_ERROR "turbojpeg.h/libturbojpeg not found -- video streaming needs libjpeg-turbo's turbojpeg API. Install libturbojpeg0-dev (Debian/Ubuntu), turbojpeg-devel (Fedora), or libjpeg-turbo (Arch).")
endif()
add_library(TurboJPEG::TurboJPEG UNKNOWN IMPORTED)
set_target_properties(TurboJPEG::TurboJPEG PROPERTIES
    IMPORTED_LOCATION "${TURBOJPEG_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${TURBOJPEG_INCLUDE_DIR}"
)

# libyuv (BSD-licensed, Google): SIMD-accelerated BGRA<->I420 conversion
# for the H.264 path only (host/remote-server/src/h264_encoder.cpp's
# bgraToI420()/client/src/h264_decoder.cpp's i420ToBgra() -- see each
# one's own comment), itself only ever reachable when OpenH264 is also
# available. Unlike TurboJPEG above, this is optional at configure time
# (no FATAL_ERROR) -- a build without it just keeps the existing hand-
# rolled scalar conversion, same graceful-degradation treatment as every
# other optional dependency in this project (X11/Wayland/OpenH264
# itself). No .pc file is shipped for it on any distro checked so far
# (Debian/Ubuntu's libyuv-dev included), so find_path()/find_library()
# rather than pkg_check_modules(), same reasoning TurboJPEG's own comment
# gives. Found once here and exposed as an imported target, same pattern
# as TurboJPEG::TurboJPEG -- each target that actually compiles
# h264_encoder.cpp/h264_decoder.cpp (or links something that does) still
# opts in and links it individually, matching how OpenH264 itself is
# handled per-target in this project rather than centrally.
find_path(LIBYUV_INCLUDE_DIR NAMES libyuv.h)
find_library(LIBYUV_LIBRARY NAMES yuv)
if(LIBYUV_INCLUDE_DIR AND LIBYUV_LIBRARY)
    set(DUALDECK_LIBYUV_FOUND TRUE)
    add_library(LibYuv::LibYuv UNKNOWN IMPORTED)
    set_target_properties(LibYuv::LibYuv PROPERTIES
        IMPORTED_LOCATION "${LIBYUV_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBYUV_INCLUDE_DIR}"
    )
else()
    set(DUALDECK_LIBYUV_FOUND FALSE)
    message(STATUS "libyuv not found -- H.264 BGRA<->I420 conversion will use the existing scalar "
                    "fallback (install libyuv-dev (Debian/Ubuntu), libyuv-devel (Fedora), or libyuv "
                    "(Arch) for SIMD-accelerated conversion)")
endif()

add_subdirectory(protocol)
add_subdirectory(adapter-sdk)

if(DUALDECK_BUILD_TESTS)
    add_subdirectory(client/tests)
endif()

if(DUALDECK_BUILD_HOST)
    add_subdirectory(host/remote-server)
    # Standalone dev tool, not shipped -- needs dualdeck_host (H264Encoder)
    # so it's gated the same as the host build itself. See
    # tools/codec-benchmark/main.cpp's own top comment.
    add_subdirectory(tools/codec-benchmark)
endif()

if(DUALDECK_BUILD_CLIENT)
    add_subdirectory(client)
endif()
