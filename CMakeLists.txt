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

# libjpeg-turbo's turbojpeg API -- used by host/remote-server/src/net_server.cpp
# to compress video frames and client/src/net_client.cpp to decompress them
# (protocol v8, see protocol.h). Found once here rather than in each
# CMakeLists.txt that needs it, since both the host and client link it.
# find_path()/find_library() rather than pkg_check_modules() or
# find_package(), since neither a CMake config file nor a guaranteed .pc
# name can be assumed across Debian/Fedora/Arch.
find_path(TURBOJPEG_INCLUDE_DIR NAMES turbojpeg.h)
find_library(TURBOJPEG_LIBRARY NAMES turbojpeg)
if(NOT TURBOJPEG_INCLUDE_DIR OR NOT TURBOJPEG_LIBRARY)
    message(FATAL_ERROR "turbojpeg.h/libturbojpeg not found -- video streaming needs libjpeg-turbo's turbojpeg API. Install libturbojpeg0-dev (Debian/Ubuntu), turbojpeg-devel (Fedora), or libjpeg-turbo (Arch).")
endif()
add_library(TurboJPEG::TurboJPEG UNKNOWN IMPORTED)
set_target_properties(TurboJPEG::TurboJPEG PROPERTIES
    IMPORTED_LOCATION "${TURBOJPEG_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${TURBOJPEG_INCLUDE_DIR}"
)

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
