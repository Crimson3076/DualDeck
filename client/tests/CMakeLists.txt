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
    ../src/net_client.cpp
    ../src/client_log.cpp
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
