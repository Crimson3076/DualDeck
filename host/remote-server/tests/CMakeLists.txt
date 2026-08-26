add_executable(melonds_remote_host_tests
    main.cpp
    test_adapter_bridge.cpp
    test_net_server_mode_switch.cpp
    test_self_update_trigger.cpp
    test_host_control_adapter.cpp
    test_mode_coordinator.cpp
    test_host_session_state.cpp
    test_capability_bridge.cpp
    test_kdialog_approval_prompt.cpp
    test_h264_encoder.cpp
    test_h264_sps.cpp
)

target_include_directories(melonds_remote_host_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../protocol/tests
)

target_link_libraries(melonds_remote_host_tests PRIVATE
    dualdeck_host
    dualdeck_adapter_sdk_fakes
    dualdeck_adapter_sdk_synthetic
    TurboJPEG::TurboJPEG
)

# Same OpenH264 detection as host/remote-server/CMakeLists.txt's own
# (that copy's DUALDECK_HAVE_OPENH264 compile definition is PRIVATE to
# dualdeck_host and so isn't visible here) -- linked directly into this
# test executable too, same precedent as TurboJPEG::TurboJPEG right
# above, so test_h264_encoder.cpp can do a real encode+decode round trip
# using OpenH264's own decoder when this build has it, and gracefully
# skip that coverage (still asserting H264Encoder::isAvailable() is
# false, and initialize() fails cleanly) when it doesn't.
if(PkgConfig_FOUND)
    pkg_check_modules(OPENH264 openh264)
endif()
if(OPENH264_FOUND)
    target_compile_definitions(melonds_remote_host_tests PRIVATE DUALDECK_HAVE_OPENH264)
    target_link_libraries(melonds_remote_host_tests PRIVATE ${OPENH264_LIBRARIES})
    target_include_directories(melonds_remote_host_tests SYSTEM PRIVATE ${OPENH264_INCLUDE_DIRS})
    target_link_directories(melonds_remote_host_tests PRIVATE ${OPENH264_LIBRARY_DIRS})
endif()

target_compile_features(melonds_remote_host_tests PRIVATE cxx_std_20)

if(DUALDECK_ENABLE_WARNINGS)
    target_compile_options(melonds_remote_host_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME host_tests COMMAND melonds_remote_host_tests)
