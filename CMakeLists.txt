cmake_minimum_required(VERSION 3.25)
project(keeby CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBEVDEV REQUIRED IMPORTED_TARGET libevdev)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)

find_package(Threads REQUIRED)

add_library(keeby_core STATIC
    src/input_capture.cpp
    src/audio_boundary.cpp
    src/sample_mixer.cpp
)
target_include_directories(keeby_core PUBLIC src)
target_link_libraries(keeby_core PUBLIC
    PkgConfig::LIBEVDEV
    PkgConfig::SNDFILE
    PkgConfig::PIPEWIRE
    Threads::Threads
)
target_compile_options(keeby_core PRIVATE -Wall -Wextra)

add_executable(keeby src/main.cpp)
target_link_libraries(keeby PRIVATE keeby_core)
target_compile_options(keeby PRIVATE -Wall -Wextra)
# Resolve application library calls at startup, not on their first RT use.
target_link_options(keeby PRIVATE "LINKER:-z,now")

target_compile_definitions(keeby PRIVATE KEEBY_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")

enable_testing()

add_executable(event_transport_test tests/event_transport_test.cpp)
target_include_directories(event_transport_test PRIVATE src)
target_link_libraries(event_transport_test PRIVATE Threads::Threads)
target_compile_options(event_transport_test PRIVATE -Wall -Wextra)
add_test(NAME event_transport_test COMMAND event_transport_test)

add_executable(key_event_test tests/key_event_test.cpp)
target_include_directories(key_event_test PRIVATE src)
target_compile_options(key_event_test PRIVATE -Wall -Wextra)
add_test(NAME key_event_test COMMAND key_event_test)

# These tests use assert(), including operations with side effects.
# Keep checks active in Release builds as well.
target_compile_options(event_transport_test PRIVATE -UNDEBUG)
target_compile_options(key_event_test PRIVATE -UNDEBUG)
set_tests_properties(event_transport_test PROPERTIES TIMEOUT 60)

add_executable(audio_boundary_test tests/audio_boundary_test.cpp)
target_link_libraries(audio_boundary_test PRIVATE keeby_core)
target_compile_options(audio_boundary_test PRIVATE -Wall -Wextra -UNDEBUG)
foreach(symbol pw_stream_new_simple pw_stream_connect pw_stream_destroy
               pw_stream_dequeue_buffer pw_stream_queue_buffer
               pw_thread_loop_start pw_thread_loop_destroy pw_context_destroy)
    target_link_options(audio_boundary_test PRIVATE "LINKER:--wrap=${symbol}")
endforeach()
add_test(NAME audio_boundary_test COMMAND audio_boundary_test)
set_tests_properties(audio_boundary_test PROPERTIES TIMEOUT 15)

add_executable(input_capture_test tests/input_capture_test.cpp)
target_link_libraries(input_capture_test PRIVATE keeby_core)
target_compile_options(input_capture_test PRIVATE -Wall -Wextra -UNDEBUG)
foreach(symbol libevdev_new_from_fd libevdev_next_event libevdev_free ioctl)
    target_link_options(input_capture_test PRIVATE "LINKER:--wrap=${symbol}")
endforeach()
add_test(NAME input_capture_test COMMAND input_capture_test)
set_tests_properties(input_capture_test PROPERTIES TIMEOUT 15)

add_executable(sample_mixer_test tests/sample_mixer_test.cpp)
target_link_libraries(sample_mixer_test PRIVATE keeby_core)
target_compile_options(sample_mixer_test PRIVATE -Wall -Wextra -UNDEBUG)
target_compile_definitions(sample_mixer_test PRIVATE KEEBY_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
add_test(NAME sample_mixer_test COMMAND sample_mixer_test)
set_tests_properties(sample_mixer_test PROPERTIES TIMEOUT 15)
