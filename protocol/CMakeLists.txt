add_library(dualdeck_protocol STATIC
    src/protocol.cpp
    src/touch_mapping.cpp
    src/input_state_tracker.cpp
    src/rate_limiter.cpp
)

target_include_directories(dualdeck_protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(dualdeck_protocol PUBLIC cxx_std_20)

if(DUALDECK_ENABLE_WARNINGS)
    target_compile_options(dualdeck_protocol PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

if(DUALDECK_BUILD_TESTS)
    add_subdirectory(tests)
endif()
