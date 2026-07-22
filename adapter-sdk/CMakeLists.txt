# The versioned Emulator Adapter Contract (GitHub issue #28 Phase 1) --
# see docs/adr/0001-host-service-and-adapter-architecture.md. Header-only
# in spirit, but session_state.cpp needs a translation unit for
# isValidTransition()/toString(), so this is a small static library like
# protocol/ rather than an INTERFACE target.
add_library(dualdeck_adapter_sdk STATIC
    src/session_state.cpp
)

target_include_directories(dualdeck_adapter_sdk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(dualdeck_adapter_sdk PUBLIC dualdeck_protocol)
target_compile_features(dualdeck_adapter_sdk PUBLIC cxx_std_20)

if(DUALDECK_ENABLE_WARNINGS)
    target_compile_options(dualdeck_adapter_sdk PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_subdirectory(fake_adapters)
add_subdirectory(ipc)
add_subdirectory(synthetic_adapter)

if(DUALDECK_BUILD_TESTS)
    add_subdirectory(tests)
endif()
