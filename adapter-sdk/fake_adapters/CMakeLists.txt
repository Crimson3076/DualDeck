# Fake DS/3DS/Wii U capability fixtures (GitHub issue #28 Phase 1) --
# implement IEmulatorAdapter for testing/proving the contract's
# genericity only, never a real emulator integration. See each
# fake_*_adapter.h for what specifically it's a fixture for.
add_library(melonds_remote_adapter_sdk_fakes STATIC
    src/fake_adapter_base.cpp
    src/fake_ds_adapter.cpp
    src/fake_3ds_adapter.cpp
    src/fake_wiiu_adapter.cpp
)

target_include_directories(melonds_remote_adapter_sdk_fakes PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(melonds_remote_adapter_sdk_fakes PUBLIC melonds_remote_adapter_sdk)
target_compile_features(melonds_remote_adapter_sdk_fakes PUBLIC cxx_std_20)

if(MELONDS_REMOTE_ENABLE_WARNINGS)
    target_compile_options(melonds_remote_adapter_sdk_fakes PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()
