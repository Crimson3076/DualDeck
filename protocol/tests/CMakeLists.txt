add_executable(dualdeck_protocol_tests
    main.cpp
    test_header.cpp
    test_controller_state.cpp
    test_touch_mapping.cpp
    test_input_state_tracker.cpp
    test_handshake.cpp
    test_rate_limiter.cpp
    test_discovery.cpp
    test_mic_audio.cpp
    test_identity.cpp
    test_mode_changed.cpp
    test_client_log.cpp
    test_video_frame.cpp
)

target_link_libraries(dualdeck_protocol_tests PRIVATE dualdeck_protocol)
target_compile_features(dualdeck_protocol_tests PRIVATE cxx_std_20)

if(DUALDECK_ENABLE_WARNINGS)
    target_compile_options(dualdeck_protocol_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME protocol_tests COMMAND dualdeck_protocol_tests)
