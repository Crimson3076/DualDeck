add_library(melonds_remote_host STATIC
    src/logging_input_sink.cpp
    src/synthetic_frame_source.cpp
    src/net_server.cpp
    src/device_approval_manager.cpp
    src/logging_mic_audio_sink.cpp
    src/adapter_bridge.cpp
    src/host_control_adapter.cpp
    src/mode_coordinator.cpp
)

target_include_directories(melonds_remote_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(melonds_remote_host PUBLIC melonds_remote_protocol)
# adapter_bridge.h/.cpp (GitHub issue #28 Phase 2) depend on the
# adapter-sdk contract; adapter_ipc.h's AdapterIpcServer is what
# main.cpp's opt-in --adapter-socket mode actually constructs.
target_link_libraries(melonds_remote_host PUBLIC melonds_remote_adapter_sdk melonds_remote_adapter_sdk_ipc)
target_compile_features(melonds_remote_host PUBLIC cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(melonds_remote_host PUBLIC Threads::Threads)

if(MELONDS_REMOTE_ENABLE_WARNINGS)
    target_compile_options(melonds_remote_host PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_executable(melonds-remote-server src/main.cpp)
target_link_libraries(melonds-remote-server PRIVATE melonds_remote_host)

if(MELONDS_REMOTE_ENABLE_WARNINGS)
    target_compile_options(melonds-remote-server PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

if(MELONDS_REMOTE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
