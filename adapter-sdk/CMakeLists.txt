# The versioned Emulator Adapter Contract (GitHub issue #28 Phase 1) --
# see docs/adr/0001-host-service-and-adapter-architecture.md. Header-only
# in spirit, but session_state.cpp needs a translation unit for
# isValidTransition()/toString(), so this is a small static library like
# protocol/ rather than an INTERFACE target.
add_library(melonds_remote_adapter_sdk STATIC
    src/session_state.cpp
)

target_include_directories(melonds_remote_adapter_sdk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(melonds_remote_adapter_sdk PUBLIC melonds_remote_protocol)
target_compile_features(melonds_remote_adapter_sdk PUBLIC cxx_std_20)

if(MELONDS_REMOTE_ENABLE_WARNINGS)
    target_compile_options(melonds_remote_adapter_sdk PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_subdirectory(fake_adapters)

if(MELONDS_REMOTE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
