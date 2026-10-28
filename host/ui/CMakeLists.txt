# dualdeck-host-ui: the full-screen DualDeck Host window that
# packaging/host/dualdeck-host.sh drives (see src/protocol.h). Needs
# SDL3, like the client; the release bundles libSDL3 next to it.

find_package(SDL3 REQUIRED CONFIG)

# The fonts are compiled in so the binary has nothing to find at runtime.
set(DUALDECK_HOST_UI_FONTS
    "kFontDisplayBold=${CMAKE_CURRENT_SOURCE_DIR}/fonts/ChakraPetch-Bold.ttf"
    "kFontDisplaySemiBold=${CMAKE_CURRENT_SOURCE_DIR}/fonts/ChakraPetch-SemiBold.ttf"
    "kFontBodyRegular=${CMAKE_CURRENT_SOURCE_DIR}/fonts/AtkinsonHyperlegible-Regular.ttf"
    "kFontBodyBold=${CMAKE_CURRENT_SOURCE_DIR}/fonts/AtkinsonHyperlegible-Bold.ttf"
)
set(embedded_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(embedded_header "${embedded_dir}/embedded_fonts.h")
set(embedded_source "${embedded_dir}/embedded_fonts.cpp")
set(font_files "")
foreach(entry IN LISTS DUALDECK_HOST_UI_FONTS)
    string(REGEX REPLACE "^[^=]+=" "" path "${entry}")
    list(APPEND font_files "${path}")
endforeach()
# Passed as one argument: a raw list would split into several.
string(REPLACE ";" "%%" fonts_arg "${DUALDECK_HOST_UI_FONTS}")
add_custom_command(
    OUTPUT "${embedded_header}" "${embedded_source}"
    COMMAND ${CMAKE_COMMAND} "-DOUT_DIR=${embedded_dir}" "-DFONTS=${fonts_arg}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_fonts.cmake"
    DEPENDS ${font_files} "${CMAKE_CURRENT_SOURCE_DIR}/embed_fonts.cmake"
    COMMENT "Embedding host UI fonts"
    VERBATIM
)

add_library(dualdeck_host_ui_core STATIC
    src/app.cpp
    src/draw.cpp
    src/protocol.cpp
    src/text.cpp
    "${embedded_source}"
)
target_include_directories(dualdeck_host_ui_core PUBLIC src "${embedded_dir}")
target_include_directories(dualdeck_host_ui_core SYSTEM PRIVATE third_party/stb)
target_link_libraries(dualdeck_host_ui_core PUBLIC SDL3::SDL3)
target_compile_features(dualdeck_host_ui_core PUBLIC cxx_std_20)

add_executable(dualdeck-host-ui src/main.cpp)
target_link_libraries(dualdeck-host-ui PRIVATE dualdeck_host_ui_core)
find_package(Threads REQUIRED)
target_link_libraries(dualdeck-host-ui PRIVATE Threads::Threads)

if(DUALDECK_ENABLE_WARNINGS)
    foreach(target dualdeck_host_ui_core dualdeck-host-ui)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endforeach()
endif()

if(DUALDECK_BUILD_TESTS)
    add_executable(dualdeck_host_ui_tests tests/main.cpp tests/test_protocol.cpp src/protocol.cpp)
    target_include_directories(dualdeck_host_ui_tests PRIVATE src ../../protocol/tests)
    target_compile_features(dualdeck_host_ui_tests PRIVATE cxx_std_20)
    if(DUALDECK_ENABLE_WARNINGS)
        target_compile_options(dualdeck_host_ui_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
    add_test(NAME host_ui_protocol_tests COMMAND dualdeck_host_ui_tests)
endif()
