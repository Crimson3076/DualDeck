add_executable(dualdeck_adapter_sdk_tests
    main.cpp
    test_session_state.cpp
    test_fake_ds_adapter.cpp
    test_fake_3ds_adapter.cpp
    test_fake_wiiu_adapter.cpp
    test_adapter_contract_generic.cpp
    test_ipc_protocol.cpp
    test_adapter_ipc_end_to_end.cpp
)

target_include_directories(dualdeck_adapter_sdk_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../protocol/tests
)

target_link_libraries(dualdeck_adapter_sdk_tests PRIVATE
    dualdeck_adapter_sdk_fakes
    dualdeck_adapter_sdk_ipc
    dualdeck_adapter_sdk
)

target_compile_features(dualdeck_adapter_sdk_tests PRIVATE cxx_std_20)

if(DUALDECK_ENABLE_WARNINGS)
    target_compile_options(dualdeck_adapter_sdk_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME adapter_sdk_tests COMMAND dualdeck_adapter_sdk_tests)
