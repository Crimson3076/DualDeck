find_package(SDL3 REQUIRED CONFIG)

add_executable(dualdeck-client
    src/main.cpp
    src/net_client.cpp
    src/device_identity.cpp
    src/discovery_client.cpp
    src/discovery_store.cpp
    src/client_settings.cpp
    src/wizard_state.cpp
    src/bitmap_font.cpp
    src/mic_capture.cpp
)

target_link_libraries(dualdeck-client PRIVATE
    dualdeck_protocol
    SDL3::SDL3
    TurboJPEG::TurboJPEG
)

target_compile_features(dualdeck-client PRIVATE cxx_std_20)

if(DUALDECK_ENABLE_WARNINGS)
    target_compile_options(dualdeck-client PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()
