cmake_minimum_required(VERSION 3.25)
project(keeby_input_audio_spike CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBEVDEV REQUIRED IMPORTED_TARGET libevdev)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)

find_package(Threads REQUIRED)

add_executable(keeby_spike
    src/main.cpp
    src/keyboard_device.cpp
    src/sample.cpp
    src/audio_engine.cpp
)

target_link_libraries(keeby_spike PRIVATE
    PkgConfig::LIBEVDEV
    PkgConfig::PIPEWIRE
    PkgConfig::SNDFILE
    Threads::Threads
)

target_compile_definitions(keeby_spike PRIVATE
    ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
)

target_compile_options(keeby_spike PRIVATE -Wall -Wextra)

enable_testing()
add_executable(ring_buffer_selftest tests/ring_buffer_selftest.cpp)
target_include_directories(ring_buffer_selftest PRIVATE src)
add_test(NAME ring_buffer_selftest COMMAND ring_buffer_selftest)
