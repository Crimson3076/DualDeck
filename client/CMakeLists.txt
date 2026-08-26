find_package(SDL3 REQUIRED CONFIG)

add_executable(dualdeck-client
    src/main.cpp
    src/net_client.cpp
    src/device_identity.cpp
    src/discovery_client.cpp
    src/discovery_store.cpp
    src/client_settings.cpp
    src/client_log.cpp
    src/wizard_state.cpp
    src/bitmap_font.cpp
    src/mic_capture.cpp
    src/h264_decoder.cpp
)

target_link_libraries(dualdeck-client PRIVATE
    dualdeck_protocol
    SDL3::SDL3
    TurboJPEG::TurboJPEG
)

# H.264 decode (protocol v13's optional codec alongside JPEG -- see
# h264_decoder.h's top comment and host/remote-server/CMakeLists.txt's
# identical detection for the encoder side). Optional at configure time,
# same as every other X11/Wayland/OpenH264 detection in this project: a
# client built without it simply never advertises VideoCodecBit_H264 in
# HelloPayload (see net_client.cpp's connect()), so every session still
# negotiates JPEG exactly as before.
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(OPENH264 openh264)
endif()
if(OPENH264_FOUND)
    target_compile_definitions(dualdeck-client PRIVATE DUALDECK_HAVE_OPENH264)
    target_link_libraries(dualdeck-client PRIVATE ${OPENH264_LIBRARIES})
    target_include_directories(dualdeck-client SYSTEM PRIVATE ${OPENH264_INCLUDE_DIRS})
    target_link_directories(dualdeck-client PRIVATE ${OPENH264_LIBRARY_DIRS})
else()
    message(STATUS "dualdeck-client: openh264 not found -- H.264 video decoding will be compiled out "
                    "as unavailable in this build (JPEG, protocol v8's existing codec, is unaffected)")
endif()

# SIMD I420->BGRA conversion for h264_decoder.cpp's i420ToBgra() -- see
# top-level CMakeLists.txt's LibYuv::LibYuv detection and that function's
# own comment. Only meaningful alongside OpenH264 itself -- gated on
# OPENH264_FOUND too so a build with libyuv but not OpenH264 doesn't
# define a macro nothing reads.
if(OPENH264_FOUND AND DUALDECK_LIBYUV_FOUND)
    target_compile_definitions(dualdeck-client PRIVATE DUALDECK_HAVE_LIBYUV)
    target_link_libraries(dualdeck-client PRIVATE LibYuv::LibYuv)
endif()

target_compile_features(dualdeck-client PRIVATE cxx_std_20)

if(DUALDECK_ENABLE_WARNINGS)
    target_compile_options(dualdeck-client PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()
