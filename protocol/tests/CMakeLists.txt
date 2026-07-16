add_executable(melonds_remote_protocol_tests
    main.cpp
    test_header.cpp
    test_controller_state.cpp
    test_touch_mapping.cpp
    test_input_state_tracker.cpp
    test_handshake.cpp
    test_rate_limiter.cpp
    test_discovery.cpp
)

target_link_libraries(melonds_remote_protocol_tests PRIVATE melonds_remote_protocol)
target_compile_features(melonds_remote_protocol_tests PRIVATE cxx_std_20)

if(MELONDS_REMOTE_ENABLE_WARNINGS)
    target_compile_options(melonds_remote_protocol_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME protocol_tests COMMAND melonds_remote_protocol_tests)
