find_package(SDL3 REQUIRED CONFIG)

add_executable(melonds-remote-client
    src/main.cpp
    src/net_client.cpp
    src/pairing_store.cpp
)

target_link_libraries(melonds-remote-client PRIVATE
    melonds_remote_protocol
    SDL3::SDL3
)

target_compile_features(melonds-remote-client PRIVATE cxx_std_20)

if(MELONDS_REMOTE_ENABLE_WARNINGS)
    target_compile_options(melonds-remote-client PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()
