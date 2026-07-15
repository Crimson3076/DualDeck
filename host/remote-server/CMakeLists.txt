add_library(melonds_remote_host STATIC
    src/logging_input_sink.cpp
    src/synthetic_frame_source.cpp
    src/net_server.cpp
)

target_include_directories(melonds_remote_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(melonds_remote_host PUBLIC melonds_remote_protocol)
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
