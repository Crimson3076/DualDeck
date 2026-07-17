add_executable(melonds_remote_adapter_sdk_tests
    main.cpp
    test_session_state.cpp
    test_fake_ds_adapter.cpp
    test_fake_3ds_adapter.cpp
    test_fake_wiiu_adapter.cpp
    test_adapter_contract_generic.cpp
)

target_include_directories(melonds_remote_adapter_sdk_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../protocol/tests
)

target_link_libraries(melonds_remote_adapter_sdk_tests PRIVATE
    melonds_remote_adapter_sdk_fakes
    melonds_remote_adapter_sdk
)

target_compile_features(melonds_remote_adapter_sdk_tests PRIVATE cxx_std_20)

if(MELONDS_REMOTE_ENABLE_WARNINGS)
    target_compile_options(melonds_remote_adapter_sdk_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME adapter_sdk_tests COMMAND melonds_remote_adapter_sdk_tests)
