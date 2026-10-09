find_package(SDL3 REQUIRED CONFIG)

# Everything but main(), so the UI preview tool below can draw the same
# screens without its own copy of the source list.
add_library(dualdeck_client_core STATIC
    src/address_entry.cpp
    src/gamepad_input.cpp
    src/host_picker.cpp
    src/screens.cpp
    src/setup_wizard.cpp
    src/net_client.cpp
    src/device_identity.cpp
    src/discovery_client.cpp
    src/discovery_json.cpp
    src/discovery_store.cpp
    src/client_settings.cpp
    src/client_log.cpp
    src/wizard_state.cpp
    src/bitmap_font.cpp
    src/mic_capture.cpp
    src/h264_decoder.cpp
    src/pyrowave_decoder.cpp
    src/yuv_conversion.cpp
)

target_include_directories(dualdeck_client_core PUBLIC src)
target_link_libraries(dualdeck_client_core PUBLIC
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
    target_compile_definitions(dualdeck_client_core PUBLIC DUALDECK_HAVE_OPENH264)
    target_link_libraries(dualdeck_client_core PUBLIC ${OPENH264_LIBRARIES})
    target_include_directories(dualdeck_client_core SYSTEM PUBLIC ${OPENH264_INCLUDE_DIRS})
    target_link_directories(dualdeck_client_core PUBLIC ${OPENH264_LIBRARY_DIRS})
else()
    message(STATUS "dualdeck-client: openh264 not found -- H.264 video decoding will be compiled out "
                    "as unavailable in this build (JPEG, protocol v8's existing codec, is unaffected)")
endif()

# PyroWave decode -- see top-level CMakeLists.txt's PyroWave::PyroWave
# detection and pyrowave_decoder.h's own comment. A client built without
# it never advertises VideoCodecBit_PyroWave (see net_client.cpp's
# connect()).
if(DUALDECK_PYROWAVE_FOUND)
    target_link_libraries(dualdeck_client_core PUBLIC PyroWave::PyroWave)
endif()

# SIMD I420->BGRA conversion for yuv_conversion.cpp's i420ToBgra() -- see
# top-level CMakeLists.txt's LibYuv::LibYuv detection and that function's
# own comment. Only meaningful alongside a planar-YUV codec (OpenH264 or
# PyroWave), same gating as host/remote-server/CMakeLists.txt's.
if((OPENH264_FOUND OR DUALDECK_PYROWAVE_FOUND) AND DUALDECK_LIBYUV_FOUND)
    target_compile_definitions(dualdeck_client_core PUBLIC DUALDECK_HAVE_LIBYUV)
    target_link_libraries(dualdeck_client_core PUBLIC LibYuv::LibYuv)
endif()

target_compile_features(dualdeck_client_core PUBLIC cxx_std_20)

add_executable(dualdeck-client src/main.cpp)
target_link_libraries(dualdeck-client PRIVATE dualdeck_client_core)

# Dev tool, not shipped: renders every client screen to a BMP so UI
# changes can be reviewed without a Steam Deck. See
# tools/ui-preview/main.cpp.
add_executable(dualdeck-ui-preview ${CMAKE_CURRENT_SOURCE_DIR}/../tools/ui-preview/main.cpp)
target_link_libraries(dualdeck-ui-preview PRIVATE dualdeck_client_core)

if(DUALDECK_ENABLE_WARNINGS)
    foreach(target dualdeck_client_core dualdeck-client dualdeck-ui-preview)
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        )
    endforeach()
endif()
