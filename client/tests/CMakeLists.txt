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

# net_client.cpp is pure sockets + protocol/, no SDL (see net_client.h's
# top-of-file comment), so it -- and a real host::NetServer to connect it
# to -- can be tested directly without any of the windowing/SDL machinery
# the rest of client/ needs. GitHub issue #4 Phase E's control-channel
# read loop is what this target exists to cover.
add_executable(melonds_remote_net_client_tests
    main.cpp
    test_net_client.cpp
    ../src/net_client.cpp
)

target_include_directories(melonds_remote_net_client_tests PRIVATE
    ../src
    ../../protocol/tests
)

target_link_libraries(melonds_remote_net_client_tests PRIVATE melonds_remote_host)
target_compile_features(melonds_remote_net_client_tests PRIVATE cxx_std_20)

if(MELONDS_REMOTE_ENABLE_WARNINGS)
    target_compile_options(melonds_remote_net_client_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME net_client_tests COMMAND melonds_remote_net_client_tests)
