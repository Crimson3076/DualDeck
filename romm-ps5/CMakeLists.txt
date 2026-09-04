cmake_minimum_required(VERSION 3.10)
project(romm-ps5 C)

# RomM-PS5 only makes sense cross-compiled for the PS5 ("Prospero"). This is
# not a native desktop build -- see toolchain/README.md for how to fetch the
# ps5-payload-dev SDK and invoke this with its toolchain/prospero.cmake file.
if(NOT PS5)
    message(FATAL_ERROR
        "RomM-PS5 must be cross-compiled using the ps5-payload-dev SDK's "
        "toolchain/prospero.cmake toolchain file. See "
        "romm-ps5/toolchain/README.md for setup instructions, e.g.:\n"
        "  cmake -B build -DCMAKE_TOOLCHAIN_FILE=$PS5_PAYLOAD_SDK/toolchain/prospero.cmake")
endif()

add_executable(romm-ps5 src/main.c)
set_target_properties(romm-ps5 PROPERTIES OUTPUT_NAME romm-ps5.elf)
