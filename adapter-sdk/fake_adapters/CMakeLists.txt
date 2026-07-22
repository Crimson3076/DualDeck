# Fake DS/3DS/Wii U capability fixtures (GitHub issue #28 Phase 1) --
# implement IEmulatorAdapter for testing/proving the contract's
# genericity only, never a real emulator integration. See each
# fake_*_adapter.h for what specifically it's a fixture for.
add_library(dualdeck_adapter_sdk_fakes STATIC
    src/fake_adapter_base.cpp
    src/fake_ds_adapter.cpp
    src/fake_3ds_adapter.cpp
    src/fake_wiiu_adapter.cpp
)

target_include_directories(dualdeck_adapter_sdk_fakes PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(dualdeck_adapter_sdk_fakes PUBLIC dualdeck_adapter_sdk)
target_compile_features(dualdeck_adapter_sdk_fakes PUBLIC cxx_std_20)

if(DUALDECK_ENABLE_WARNINGS)
    target_compile_options(dualdeck_adapter_sdk_fakes PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()
