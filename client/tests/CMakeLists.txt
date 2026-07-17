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

if(MELONDS_REMOTE_ENABLE_WARNINGS)
    target_compile_options(melonds_remote_client_settings_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME client_settings_tests COMMAND melonds_remote_client_settings_tests)
