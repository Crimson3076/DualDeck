add_executable(melonds_remote_client_settings_tests
    main.cpp
    test_client_settings.cpp
    ../src/client_settings.cpp
)

target_include_directories(melonds_remote_client_settings_tests PRIVATE
    ../src
    ../../protocol/tests
)
target_compile_features(melonds_remote_client_settings_tests PRIVATE cxx_std_20)

if(DUALDECK_ENABLE_WARNINGS)
    target_compile_options(melonds_remote_client_settings_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME client_settings_tests COMMAND melonds_remote_client_settings_tests)

# net_client.cpp is pure sockets + protocol/, no SDL (see net_client.h's
# top-of-file comment), so it -- and a real host::NetServer to connect it
# to -- can be tested directly without any of the windowing/SDL machinery
# the rest of client/ needs. GitHub issue #4 Phase E's control-channel
# read loop is what this target exists to cover.
add_executable(melonds_remote_net_client_tests
    main.cpp
    test_net_client.cpp
    test_h264_decoder.cpp
    ../src/net_client.cpp
    ../src/client_log.cpp
    ../src/h264_decoder.cpp
)

target_include_directories(melonds_remote_net_client_tests PRIVATE
    ../src
    ../../protocol/tests
)

target_link_libraries(melonds_remote_net_client_tests PRIVATE dualdeck_host)
target_compile_features(melonds_remote_net_client_tests PRIVATE cxx_std_20)

if(DUALDECK_ENABLE_WARNINGS)
    target_compile_options(melonds_remote_net_client_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

# Same OpenH264 detection as host/remote-server/CMakeLists.txt's own --
# see h264_decoder.h's top comment. dualdeck_host already links OpenH264
# privately (for H264Encoder) when it's found, but that definition
# doesn't propagate here, so it's detected and linked again directly,
# same precedent as TurboJPEG::TurboJPEG/dualdeck_host's own copy
# elsewhere in this file. test_h264_decoder.cpp uses this project's own
# real H264Encoder (linked in via dualdeck_host) to produce genuine
# Annex-B bytes and feeds them into H264Decoder -- a real round trip
# through both halves of this project's own codec pipeline, not just
# each one independently against OpenH264's raw API (host/remote-server/
# tests/test_h264_encoder.cpp already covers that).
#
# find_package(PkgConfig) called directly here (not assumed already
# done) -- the top-level CMakeLists.txt adds this directory *before*
# host/remote-server, whose own CMakeLists.txt is where that call would
# otherwise first happen, so PkgConfig_FOUND can't be relied on yet at
# this point in the configure run.
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(OPENH264 openh264)
endif()
if(OPENH264_FOUND)
    target_compile_definitions(melonds_remote_net_client_tests PRIVATE DUALDECK_HAVE_OPENH264)
    target_link_libraries(melonds_remote_net_client_tests PRIVATE ${OPENH264_LIBRARIES})
    target_include_directories(melonds_remote_net_client_tests SYSTEM PRIVATE ${OPENH264_INCLUDE_DIRS})
    target_link_directories(melonds_remote_net_client_tests PRIVATE ${OPENH264_LIBRARY_DIRS})
endif()

# SIMD I420->BGRA conversion for h264_decoder.cpp's i420ToBgra() -- see
# top-level CMakeLists.txt's LibYuv::LibYuv detection and that function's
# own comment. This target compiles h264_decoder.cpp directly (see the
# source list above), so it needs both the compile definition and the
# link, same as client/CMakeLists.txt's dualdeck-client target.
if(OPENH264_FOUND AND DUALDECK_LIBYUV_FOUND)
    target_compile_definitions(melonds_remote_net_client_tests PRIVATE DUALDECK_HAVE_LIBYUV)
    target_link_libraries(melonds_remote_net_client_tests PRIVATE LibYuv::LibYuv)
endif()

add_test(NAME net_client_tests COMMAND melonds_remote_net_client_tests)

# Covers the melonDS-Remote -> DualDeck config-dir migration added to
# each of these four files' defaultXPath() functions -- pure filesystem
# logic, no SDL needed, same reasoning as client_settings_tests above.
add_executable(dualdeck_config_migration_tests
    main.cpp
    test_config_migration.cpp
    ../src/device_identity.cpp
    ../src/discovery_store.cpp
    ../src/client_settings.cpp
    ../src/wizard_state.cpp
)

target_include_directories(dualdeck_config_migration_tests PRIVATE
    ../src
    ../../protocol/tests
)
target_compile_features(dualdeck_config_migration_tests PRIVATE cxx_std_20)

if(DUALDECK_ENABLE_WARNINGS)
    target_compile_options(dualdeck_config_migration_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME config_migration_tests COMMAND dualdeck_config_migration_tests)
