add_library(dualdeck_host STATIC
    src/logging_input_sink.cpp
    src/synthetic_frame_source.cpp
    src/net_server.cpp
    src/device_approval_manager.cpp
    src/logging_mic_audio_sink.cpp
    src/adapter_bridge.cpp
    src/host_control_adapter.cpp
    src/mode_coordinator.cpp
    src/kdialog_approval_prompt.cpp
)

target_include_directories(dualdeck_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(dualdeck_host PUBLIC dualdeck_protocol)
# adapter_bridge.h/.cpp (GitHub issue #28 Phase 2) depend on the
# adapter-sdk contract; adapter_ipc.h's AdapterIpcServer is what
# main.cpp's opt-in --adapter-socket mode actually constructs.
target_link_libraries(dualdeck_host PUBLIC dualdeck_adapter_sdk dualdeck_adapter_sdk_ipc)
target_compile_features(dualdeck_host PUBLIC cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(dualdeck_host PUBLIC Threads::Threads)

# JPEG-compresses video frames before they go out over the wire (protocol
# v8) -- see net_server.cpp's videoLoop() and top-level CMakeLists.txt for
# where TurboJPEG::TurboJPEG is found.
target_link_libraries(dualdeck_host PRIVATE TurboJPEG::TurboJPEG)

if(DUALDECK_ENABLE_WARNINGS)
    target_compile_options(dualdeck_host PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_executable(dualdeck-host-service src/main.cpp)
target_link_libraries(dualdeck-host-service PRIVATE dualdeck_host)

if(DUALDECK_ENABLE_WARNINGS)
    target_compile_options(dualdeck-host-service PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

if(DUALDECK_BUILD_TESTS)
    add_subdirectory(tests)
endif()
