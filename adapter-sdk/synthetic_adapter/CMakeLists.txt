# A real IEmulatorAdapter implementation (self-contained animated test
# pattern) plus a standalone executable that connects it to a Host
# Service over the adapter IPC channel (GitHub issue #28 Phase 2).
add_library(dualdeck_adapter_sdk_synthetic STATIC
    src/synthetic_emulator_adapter.cpp
)

target_include_directories(dualdeck_adapter_sdk_synthetic PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(dualdeck_adapter_sdk_synthetic PUBLIC dualdeck_adapter_sdk)
target_compile_features(dualdeck_adapter_sdk_synthetic PUBLIC cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(dualdeck_adapter_sdk_synthetic PUBLIC Threads::Threads)

if(DUALDECK_ENABLE_WARNINGS)
    target_compile_options(dualdeck_adapter_sdk_synthetic PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_executable(dualdeck-synthetic-adapter src/main.cpp)
target_link_libraries(dualdeck-synthetic-adapter PRIVATE
    dualdeck_adapter_sdk_synthetic
    dualdeck_adapter_sdk_ipc
)

if(DUALDECK_ENABLE_WARNINGS)
    target_compile_options(dualdeck-synthetic-adapter PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()
