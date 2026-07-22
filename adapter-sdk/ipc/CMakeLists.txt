# The local Host-Service<->adapter IPC channel (GitHub issue #28 Phase 2
# / the ADR's IPC decision) -- Unix domain socket transport, not yet
# wired into host/remote-server's production NetServer.
add_library(dualdeck_adapter_sdk_ipc STATIC
    src/ipc_protocol.cpp
    src/socket_path.cpp
    src/adapter_ipc_server.cpp
    src/adapter_ipc_client.cpp
)

target_include_directories(dualdeck_adapter_sdk_ipc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(dualdeck_adapter_sdk_ipc PUBLIC dualdeck_adapter_sdk)
target_compile_features(dualdeck_adapter_sdk_ipc PUBLIC cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(dualdeck_adapter_sdk_ipc PUBLIC Threads::Threads)

if(DUALDECK_ENABLE_WARNINGS)
    target_compile_options(dualdeck_adapter_sdk_ipc PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()
