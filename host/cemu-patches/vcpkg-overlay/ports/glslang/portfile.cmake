# DualDeck (github.com/crimson3076/DualDeck): real Fedora build failure,
# 2026-08-26 -- glslang 14.2.0 uses uint32_t (and friends) in headers/
# translation units that don't themselves #include <cstdint>, relying on
# it arriving transitively through some other standard header. That
# transitive include is not guaranteed by the C++ standard, and a newer
# libstdc++ (Fedora ships a newer GCC/libstdc++ than this project's own
# release.yml CI runner, ubuntu-latest, has as of this writing) can
# legitimately stop providing it, breaking the build with no upstream
# code change at all -- the same class of failure this port's sibling
# sdl2 overlay (host/cemu-patches/vcpkg-overlay/ports/sdl2/) works around
# for a different header-strictness reason. See
# host/cemu-patches/vcpkg-overlay/README.md for why this whole overlay
# mechanism exists.
#
# Scoped to just this one vcpkg port's own compilation (VCPKG_CXX_FLAGS
# is read by vcpkg_cmake_configure() below, for glslang's build only) --
# deliberately NOT a blanket CXXFLAGS="... -include cstdint" env var
# covering scripts/build-release.sh's *entire* invocation (Cemu's ~500
# own source files plus all ~108 other vcpkg dependencies), which is
# what the original local workaround this replaces did.
#
# Real CI failure, 2026-08-26 (this exact overlay's first actual build
# attempt): vcpkg_cmake_configure()'s own precondition check --
# `If VCPKG_CXX_FLAGS is set, then VCPKG_C_FLAGS must be set.` -- vcpkg
# requires both to be defined together, even though this fix only needs
# the C++ one (the actual missing include is in glslang's C++ headers;
# nothing in this port's C sources, if any, hit the same issue). Setting
# VCPKG_C_FLAGS to plain `-DDUALDECK_UNUSED=1` here rather than also
# forcing `-include cstdint` onto C compilation: `<cstdint>` (no `.h`) is
# a C++-only header name, not guaranteed to resolve for a C translation
# unit at all, so reusing the C++ flag verbatim for C risked trading one
# real build failure for a different one exactly like it -- this is a
# harmless per-port command-line define that satisfies vcpkg's "both or
# neither" precondition without asserting anything about C header
# behavior this fix was never about.
if(NOT DEFINED VCPKG_CXX_FLAGS)
    set(VCPKG_CXX_FLAGS "")
endif()
set(VCPKG_CXX_FLAGS "${VCPKG_CXX_FLAGS} -include cstdint")
if(NOT DEFINED VCPKG_C_FLAGS)
    set(VCPKG_C_FLAGS "")
endif()
set(VCPKG_C_FLAGS "${VCPKG_C_FLAGS} -DDUALDECK_UNUSED=1")

vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO KhronosGroup/glslang
    REF "${VERSION}"
    SHA512 570d2ff15116f48e195c73d9be1517b05e7c37541af10f6c05779a001e2d0295725349c1f4dd0bcca6f0c7e7e48c5162a60726c3e76cf04619c8e14bd0636ab6
    HEAD_REF master
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        opt ENABLE_OPT
        opt ALLOW_EXTERNAL_SPIRV_TOOLS
        tools ENABLE_GLSLANG_BINARIES
        rtti ENABLE_RTTI
)

if (ENABLE_GLSLANG_BINARIES)
    vcpkg_find_acquire_program(PYTHON3)
    get_filename_component(PYTHON_PATH ${PYTHON3} DIRECTORY)
    vcpkg_add_to_path("${PYTHON_PATH}")
endif ()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_EXTERNAL=OFF
        -DGLSLANG_TESTS=OFF
        ${FEATURE_OPTIONS}
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/glslang DO_NOT_DELETE_PARENT_CONFIG_PATH)
vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/share/${PORT}/glslang-config.cmake"
    [[${PACKAGE_PREFIX_DIR}/lib/cmake/glslang/glslang-targets.cmake]]
    [[${CMAKE_CURRENT_LIST_DIR}/glslang-targets.cmake]]
)
file(REMOVE_RECURSE CONFIG_PATH "${CURRENT_PACKAGES_DIR}/lib/cmake" "${CURRENT_PACKAGES_DIR}/debug/lib/cmake")

if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/glslang/Public/ShaderLang.h" "ifdef GLSLANG_IS_SHARED_LIBRARY" "if 1")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/glslang/Include/glslang_c_interface.h" "ifdef GLSLANG_IS_SHARED_LIBRARY" "if 1")
endif()

vcpkg_copy_pdbs()

if (ENABLE_GLSLANG_BINARIES)
    vcpkg_copy_tools(TOOL_NAMES glslang glslangValidator spirv-remap AUTO_CLEAN)
endif ()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")
