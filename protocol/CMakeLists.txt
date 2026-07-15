add_library(melonds_remote_protocol STATIC
    src/protocol.cpp
    src/touch_mapping.cpp
    src/input_state_tracker.cpp
    src/rate_limiter.cpp
)

target_include_directories(melonds_remote_protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(melonds_remote_protocol PUBLIC cxx_std_20)

if(MELONDS_REMOTE_ENABLE_WARNINGS)
    target_compile_options(melonds_remote_protocol PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

if(MELONDS_REMOTE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
