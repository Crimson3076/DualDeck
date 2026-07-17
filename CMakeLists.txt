cmake_minimum_required(VERSION 3.20)
project(melonds_remote LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MELONDS_REMOTE_BUILD_TESTS "Build unit tests" ON)
option(MELONDS_REMOTE_BUILD_CLIENT "Build the SDL3 Steam Deck client (requires SDL3)" OFF)
option(MELONDS_REMOTE_BUILD_HOST "Build the standalone host remote-server prototype" ON)
option(MELONDS_REMOTE_ENABLE_WARNINGS "Enable strict warnings (-Wall -Wextra -Wpedantic -Wconversion -Wshadow)" ON)

enable_testing()

add_subdirectory(protocol)

if(MELONDS_REMOTE_BUILD_TESTS)
    add_subdirectory(client/tests)
endif()

if(MELONDS_REMOTE_BUILD_HOST)
    add_subdirectory(host/remote-server)
endif()

if(MELONDS_REMOTE_BUILD_CLIENT)
    add_subdirectory(client)
endif()
