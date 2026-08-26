# Latency-audit follow-up: standalone JPEG-vs-H.264 benchmark (see
# main.cpp's own top comment). Links dualdeck_host directly for
# H264Encoder (and its own real quality/bitrate-selection formulas,
# duplicated in main.cpp -- see that duplication's own comment) and
# compiles client/src/h264_decoder.cpp directly for H264Decoder, the same
# "no SDL needed, compile it straight into whatever needs it" approach
# client/tests/CMakeLists.txt's net_client_tests target already uses for
# the same file.
add_executable(dualdeck-codec-benchmark
    main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../client/src/h264_decoder.cpp
)

target_include_directories(dualdeck-codec-benchmark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../client/src
)

target_link_libraries(dualdeck-codec-benchmark PRIVATE
    dualdeck_host
    dualdeck_protocol
    TurboJPEG::TurboJPEG
)

# Same OpenH264 detection as host/remote-server/CMakeLists.txt's own --
# that target's DUALDECK_HAVE_OPENH264 is PRIVATE to dualdeck_host, so it
# isn't visible here and must be redetected/relinked directly, same
# precedent as client/tests/CMakeLists.txt's net_client_tests target.
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(OPENH264 openh264)
endif()
if(OPENH264_FOUND)
    target_compile_definitions(dualdeck-codec-benchmark PRIVATE DUALDECK_HAVE_OPENH264)
    target_link_libraries(dualdeck-codec-benchmark PRIVATE ${OPENH264_LIBRARIES})
    target_include_directories(dualdeck-codec-benchmark SYSTEM PRIVATE ${OPENH264_INCLUDE_DIRS})
    target_link_directories(dualdeck-codec-benchmark PRIVATE ${OPENH264_LIBRARY_DIRS})
else()
    message(STATUS "dualdeck-codec-benchmark: openh264 not found -- H.264 side of the benchmark will be skipped")
endif()

target_compile_features(dualdeck-codec-benchmark PRIVATE cxx_std_20)

if(DUALDECK_ENABLE_WARNINGS)
    target_compile_options(dualdeck-codec-benchmark PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()
