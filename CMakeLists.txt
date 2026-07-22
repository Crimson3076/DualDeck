cmake_minimum_required(VERSION 3.20)
project(dualdeck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DUALDECK_BUILD_TESTS "Build unit tests" ON)
option(DUALDECK_BUILD_CLIENT "Build the SDL3 Steam Deck client (requires SDL3)" OFF)
option(DUALDECK_BUILD_HOST "Build the standalone host remote-server prototype" ON)
option(DUALDECK_ENABLE_WARNINGS "Enable strict warnings (-Wall -Wextra -Wpedantic -Wconversion -Wshadow)" ON)

enable_testing()

add_subdirectory(protocol)
add_subdirectory(adapter-sdk)

if(DUALDECK_BUILD_TESTS)
    add_subdirectory(client/tests)
endif()

if(DUALDECK_BUILD_HOST)
    add_subdirectory(host/remote-server)
endif()

if(DUALDECK_BUILD_CLIENT)
    add_subdirectory(client)
endif()
