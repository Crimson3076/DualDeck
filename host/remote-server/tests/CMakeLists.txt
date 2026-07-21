add_executable(melonds_remote_host_tests
    main.cpp
    test_adapter_bridge.cpp
    test_net_server_mode_switch.cpp
    test_host_control_adapter.cpp
    test_mode_coordinator.cpp
    test_kdialog_approval_prompt.cpp
)

target_include_directories(melonds_remote_host_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../protocol/tests
)

target_link_libraries(melonds_remote_host_tests PRIVATE
    melonds_remote_host
    melonds_remote_adapter_sdk_fakes
    melonds_remote_adapter_sdk_synthetic
)

target_compile_features(melonds_remote_host_tests PRIVATE cxx_std_20)

if(MELONDS_REMOTE_ENABLE_WARNINGS)
    target_compile_options(melonds_remote_host_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME host_tests COMMAND melonds_remote_host_tests)
