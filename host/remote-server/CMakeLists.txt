add_library(dualdeck_host STATIC
    src/logging_input_sink.cpp
    src/synthetic_frame_source.cpp
    src/net_server.cpp
    src/device_approval_manager.cpp
    src/logging_mic_audio_sink.cpp
    src/adapter_bridge.cpp
    src/host_control_adapter.cpp
    src/x11_screen_capture.cpp
    src/mode_coordinator.cpp
    src/host_session_state.cpp
    src/capability_bridge.cpp
    src/kdialog_approval_prompt.cpp
)

# Host Control screen-mirror experiment (DUALDECK_HOSTCONTROL_MIRROR_SCREEN,
# see host_control_adapter.cpp's own comment) -- optional at configure
# time, unlike TurboJPEG above: X11 dev headers are already a hard
# requirement of this project's melonDS/Azahar Qt builds (see
# scripts/build-release.sh's "build" ensure_packages() list), so this CI
# environment already has them, but a from-source build of just the host
# (DUALDECK_BUILD_HOST=ON without ever touching melonDS/Qt at all --
# exactly how this target is built and tested standalone) shouldn't be
# forced to install X11 dev headers just to compile a narrow, opt-in
# feature it may never use. x11_screen_capture.cpp itself always
# compiles either way (see its own DUALDECK_HAVE_X11_SCREEN_CAPTURE
# guard) -- this only decides which half of it: a real implementation,
# or an always-not-ready stub.
find_package(X11)
if(X11_FOUND AND X11_Xext_FOUND)
    target_compile_definitions(dualdeck_host PRIVATE DUALDECK_HAVE_X11_SCREEN_CAPTURE)
    target_link_libraries(dualdeck_host PRIVATE ${X11_LIBRARIES} ${X11_Xext_LIB})
    target_include_directories(dualdeck_host PRIVATE ${X11_INCLUDE_DIR})
else()
    message(STATUS "dualdeck_host: X11/Xext not found -- Host Control screen-mirroring "
                    "(DUALDECK_HOSTCONTROL_MIRROR_SCREEN) will be compiled out as a no-op in this build")
endif()

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
