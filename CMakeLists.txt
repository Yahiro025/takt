cmake_minimum_required(VERSION 3.25)
project(keeby VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(GNUInstallDirs)

# Step 2.9 packaging: compiled-in defaults for the built-in asset dir and the
# privileged helper's installed path. Empty KEEBY_HELPER_PATH keeps today's
# dev behavior (resolve next to keeby's own executable); packaging/arch/PKGBUILD
# sets both to their installed /usr paths. The runtime env vars $KEEBY_ASSET_DIR
# and the pre-existing $KEEBY_INPUTD_PATH override these at runtime -- see
# src/paths.hpp and src/input_capture.cpp.
set(KEEBY_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets" CACHE PATH
    "Compiled-in default built-in sound asset directory")
set(KEEBY_HELPER_PATH "" CACHE FILEPATH
    "Compiled-in absolute path to keeby-inputd; empty resolves next to keeby's own executable")

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBEVDEV REQUIRED IMPORTED_TARGET libevdev)
# keeby-inputd only (docs/006's "Exact pointer tracking via libinput"
# section). Deliberately NOT an IMPORTED_TARGET and never passed to
# target_link_libraries(): keeby-inputd uses only ${LIBINPUT_INCLUDE_DIRS}
# (headers, for types) and dlopen()s the actual library itself at runtime,
# post-privilege-drop only -- see src/libinput_loader.hpp for why. Never
# linked into keeby_core either way.
pkg_check_modules(LIBINPUT REQUIRED libinput)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
# Desktop tray: raw StatusNotifierItem over D-Bus via sdbus-c++ (systemd's
# sd-bus with an idiomatic C++ API). No GTK/Qt/AppIndicator pulled in — see
# docs/005-step-2.4-desktop-shell.md for why this fits Wayland/Niri.
pkg_check_modules(SDBUSCPP REQUIRED IMPORTED_TARGET sdbus-c++)
# Step 2.7: Mechvibes/native sound-pack config parsing (nlohmann-json) and
# source-rate/pitch conversion (libsamplerate) for user-downloaded packs —
# see docs/008-step-2.7-sound-packs.md.
find_package(nlohmann_json 3 REQUIRED)
pkg_check_modules(SAMPLERATE REQUIRED IMPORTED_TARGET samplerate)

find_package(Threads REQUIRED)

add_library(keeby_core STATIC
    src/input_capture.cpp
    src/audio_boundary.cpp
    src/sample_mixer.cpp
    src/sound_pack.cpp
    src/engine_controller.cpp
    src/tray_service.cpp
    src/dbus_menu.cpp
    src/settings.cpp
    src/control_service.cpp
    src/visualizer_service.cpp
)
target_include_directories(keeby_core PUBLIC src)
target_link_libraries(keeby_core PUBLIC
    PkgConfig::LIBEVDEV
    PkgConfig::SNDFILE
    PkgConfig::PIPEWIRE
    PkgConfig::SDBUSCPP
    PkgConfig::SAMPLERATE
    nlohmann_json::nlohmann_json
    Threads::Threads
)
target_compile_options(keeby_core PRIVATE -Wall -Wextra)
# engine_controller.cpp's "default" profile fallback needs the same built-in
# asset path `keeby` uses (see main.cpp/src/paths.hpp), and input_capture.cpp's
# resolve_helper_path() needs the compiled-in helper default.
target_compile_definitions(keeby_core PRIVATE
    KEEBY_ASSET_DIR_DEFAULT="${KEEBY_ASSET_DIR}"
    KEEBY_HELPER_PATH_DEFAULT="${KEEBY_HELPER_PATH}"
    KEEBY_VERSION="${PROJECT_VERSION}"
    # visualizer_service.cpp's resolve_helper_path() last-resort fallback
    # when readlink("/proc/self/exe") fails: keeby-visualizer is installed
    # to the same bindir as keeby itself (see docs/013-visualizer.md), so
    # this is intentionally just a directory, unlike KEEBY_HELPER_PATH_DEFAULT
    # (a full path -- keeby-inputd installs elsewhere, to ${CMAKE_INSTALL_LIBDIR}/keeby).
    KEEBY_VISUALIZER_BINDIR_DEFAULT="${CMAKE_INSTALL_FULL_BINDIR}")

add_executable(keeby src/main.cpp)
target_link_libraries(keeby PRIVATE keeby_core)
target_compile_options(keeby PRIVATE -Wall -Wextra)
# Resolve application library calls at startup, not on their first RT use.
target_link_options(keeby PRIVATE "LINKER:-z,now")

target_compile_definitions(keeby PRIVATE KEEBY_ASSET_DIR_DEFAULT="${KEEBY_ASSET_DIR}")

# Privileged keyboard-input helper (Step 2.5B): the ONLY component that
# opens /dev/input/*. Deliberately self-contained — no keeby_core, no
# PipeWire, no D-Bus, no sound library — see
# docs/006-step-2.5-security-permissions.md for the privilege model and
# scripts/install-keeby-inputd.sh for the setgid-input install step this
# binary requires to be useful in production.
add_executable(keeby-inputd src/inputd_main.cpp)
target_include_directories(keeby-inputd PRIVATE src ${LIBINPUT_INCLUDE_DIRS})
# ${CMAKE_DL_LIBS}: dlopen/dlsym/dlclose for src/libinput_loader.hpp (empty
# on a glibc where these are already in libc; CMake supplies -ldl only
# where actually needed). Deliberately no libinput link -- see above.
target_link_libraries(keeby-inputd PRIVATE PkgConfig::LIBEVDEV ${CMAKE_DL_LIBS})
target_compile_options(keeby-inputd PRIVATE -Wall -Wextra)

# Step 2.9 packaging (packaging/arch/PKGBUILD): plain installs only here.
# root:input ownership, the 2750 setgid mode and the group ACL on
# keeby-inputd are applied by the PKGBUILD/keeby.install afterwards --
# install() itself has no setgid concept.
install(TARGETS keeby RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS keeby-inputd RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/keeby)
install(DIRECTORY assets/ DESTINATION ${CMAKE_INSTALL_DATADIR}/keeby/assets
        FILES_MATCHING PATTERN "*.wav")
install(PROGRAMS scripts/fetch-mechvibes-packs.sh
        DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME keeby-fetch-packs)
install(FILES packaging/keeby.service DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/user)
install(FILES packaging/keeby.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/keeby.conf DESTINATION ${CMAKE_INSTALL_LIBDIR}/sysusers.d)

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

# keeby-inputd's pointer-motion pure logic (device classification, the
# sub-pixel carry/truncate accumulator fed by each libinput motion event):
# no fd, libevdev, or fork needed -- see src/pointer_input.hpp.
add_executable(pointer_input_test tests/pointer_input_test.cpp)
target_include_directories(pointer_input_test PRIVATE src)
target_compile_options(pointer_input_test PRIVATE -Wall -Wextra -UNDEBUG)
add_test(NAME pointer_input_test COMMAND pointer_input_test)

# Exercises src/libinput_loader.hpp's dlopen()-based runtime loading against
# the real system libinput.so, headers-only at build time exactly like
# keeby-inputd itself -- see tests/libinput_loader_test.cpp for what it
# checks. No /dev/input access, no root, no fork.
add_executable(libinput_loader_test tests/libinput_loader_test.cpp)
target_include_directories(libinput_loader_test PRIVATE src ${LIBINPUT_INCLUDE_DIRS})
target_link_libraries(libinput_loader_test PRIVATE ${CMAKE_DL_LIBS})
target_compile_options(libinput_loader_test PRIVATE -Wall -Wextra -UNDEBUG)
add_test(NAME libinput_loader_test COMMAND libinput_loader_test)
set_tests_properties(libinput_loader_test PROPERTIES TIMEOUT 15)

# keeby-visualizer's pure logic (layout table, message validation,
# dismiss-timer, position<->anchor mapping): no GTK/layer-shell dependency,
# so this always builds and runs regardless of KEEBY_BUILD_VISUALIZER below.
add_executable(visualizer_test tests/visualizer_test.cpp)
target_include_directories(visualizer_test PRIVATE src)
target_compile_options(visualizer_test PRIVATE -Wall -Wextra -UNDEBUG)
add_test(NAME visualizer_test COMMAND visualizer_test)

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

# Test-only stand-in for keeby-inputd (see tests/fake_inputd.cpp):
# InputCapture's real fork/exec/socketpair/IPC path is exercised against
# this instead of the real helper, via KEEBY_INPUTD_PATH, so no setgid
# privilege or real keyboard is needed to test it.
add_executable(fake_inputd_test_helper tests/fake_inputd.cpp)
target_include_directories(fake_inputd_test_helper PRIVATE src)
target_compile_options(fake_inputd_test_helper PRIVATE -Wall -Wextra)

add_executable(input_capture_test tests/input_capture_test.cpp)
target_link_libraries(input_capture_test PRIVATE keeby_core)
target_compile_options(input_capture_test PRIVATE -Wall -Wextra -UNDEBUG)
target_compile_definitions(input_capture_test PRIVATE
    FAKE_INPUTD_PATH="$<TARGET_FILE:fake_inputd_test_helper>")
add_test(NAME input_capture_test COMMAND input_capture_test)
set_tests_properties(input_capture_test PROPERTIES TIMEOUT 15)

# Test-only stand-in for keeby-visualizer (see tests/fake_keeby_visualizer.cpp):
# VisualizerService's real fork/execv/socketpair/IPC path is exercised
# against this instead of the real overlay app, via KEEBY_VISUALIZER_PATH.
add_executable(fake_keeby_visualizer_test_helper tests/fake_keeby_visualizer.cpp)
target_include_directories(fake_keeby_visualizer_test_helper PRIVATE src)
target_compile_options(fake_keeby_visualizer_test_helper PRIVATE -Wall -Wextra)

add_executable(visualizer_service_test tests/visualizer_service_test.cpp)
target_link_libraries(visualizer_service_test PRIVATE keeby_core)
target_compile_options(visualizer_service_test PRIVATE -Wall -Wextra -UNDEBUG)
target_compile_definitions(visualizer_service_test PRIVATE
    FAKE_VISUALIZER_PATH="$<TARGET_FILE:fake_keeby_visualizer_test_helper>"
    FAKE_INPUTD_PATH="$<TARGET_FILE:fake_inputd_test_helper>")
add_test(NAME visualizer_service_test COMMAND visualizer_service_test)
set_tests_properties(visualizer_service_test PROPERTIES TIMEOUT 20)

add_executable(sample_mixer_test tests/sample_mixer_test.cpp)
target_link_libraries(sample_mixer_test PRIVATE keeby_core)
target_compile_options(sample_mixer_test PRIVATE -Wall -Wextra -UNDEBUG)
target_compile_definitions(sample_mixer_test PRIVATE KEEBY_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
add_test(NAME sample_mixer_test COMMAND sample_mixer_test)
set_tests_properties(sample_mixer_test PROPERTIES TIMEOUT 15)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME sample_variants_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/assets/generate_variants.py --check)
set_tests_properties(sample_variants_test PROPERTIES TIMEOUT 15)

# Combines audio_boundary_test's PipeWire wraps with the same fake-helper
# substitution input_capture_test uses for the input side: EngineController
# composes both AudioBoundary and InputCapture, real hardware/server/setgid
# privilege needed for neither.
add_executable(engine_controller_test tests/engine_controller_test.cpp)
target_link_libraries(engine_controller_test PRIVATE keeby_core)
target_compile_options(engine_controller_test PRIVATE -Wall -Wextra -UNDEBUG)
target_compile_definitions(engine_controller_test PRIVATE
    FAKE_INPUTD_PATH="$<TARGET_FILE:fake_inputd_test_helper>")
foreach(symbol pw_stream_new_simple pw_stream_connect pw_stream_destroy
               pw_stream_dequeue_buffer pw_stream_queue_buffer
               pw_thread_loop_start pw_thread_loop_destroy pw_context_destroy)
    target_link_options(engine_controller_test PRIVATE "LINKER:--wrap=${symbol}")
endforeach()
add_test(NAME engine_controller_test COMMAND engine_controller_test)
# Forces the source tree's assets regardless of the compiled KEEBY_ASSET_DIR
# default: needed so this test (set_profile("default")) still passes during
# the Arch package's check(), which runs before anything is installed to
# the compiled-in /usr/share/keeby/assets.
set_tests_properties(engine_controller_test PROPERTIES TIMEOUT 15
    ENVIRONMENT "KEEBY_ASSET_DIR=${CMAKE_CURRENT_SOURCE_DIR}/assets")

add_executable(sound_pack_test tests/sound_pack_test.cpp)
target_link_libraries(sound_pack_test PRIVATE keeby_core)
target_compile_options(sound_pack_test PRIVATE -Wall -Wextra -UNDEBUG)
add_test(NAME sound_pack_test COMMAND sound_pack_test)
set_tests_properties(sound_pack_test PROPERTIES TIMEOUT 15)

add_executable(native_catalog_audio_policy_test tests/native_catalog_audio_policy_test.cpp)
target_link_libraries(native_catalog_audio_policy_test PRIVATE keeby_core)
target_compile_options(native_catalog_audio_policy_test PRIVATE -Wall -Wextra -UNDEBUG)
add_test(NAME native_catalog_audio_policy_test COMMAND native_catalog_audio_policy_test)
set_tests_properties(native_catalog_audio_policy_test PROPERTIES TIMEOUT 15)

add_executable(dbus_menu_test tests/dbus_menu_test.cpp)
target_link_libraries(dbus_menu_test PRIVATE keeby_core)
target_compile_options(dbus_menu_test PRIVATE -Wall -Wextra -UNDEBUG)
add_test(NAME dbus_menu_test COMMAND dbus_menu_test)
# No live session bus in some CI/sandboxed environments: the test detects
# that itself and exits 77, which ctest treats as SKIP via this property.
set_tests_properties(dbus_menu_test PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77)

add_executable(settings_test tests/settings_test.cpp)
target_link_libraries(settings_test PRIVATE keeby_core)
target_compile_options(settings_test PRIVATE -Wall -Wextra -UNDEBUG)
add_test(NAME settings_test COMMAND settings_test)
set_tests_properties(settings_test PROPERTIES TIMEOUT 15)

add_executable(control_service_test tests/control_service_test.cpp)
target_link_libraries(control_service_test PRIVATE keeby_core)
target_compile_options(control_service_test PRIVATE -Wall -Wextra -UNDEBUG)
add_test(NAME control_service_test COMMAND control_service_test)
# Unique bus name per run (see the test) and SKIP (exit 77) with no session
# bus: never touches a real running KEEBY.
set_tests_properties(control_service_test PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77)

# Pure logic (build_tray_menu / dispatch_tray_menu_click): no D-Bus
# connection needed, so unlike dbus_menu_test/control_service_test this
# always runs, even with no session bus.
add_executable(tray_menu_test tests/tray_menu_test.cpp)
target_link_libraries(tray_menu_test PRIVATE keeby_core)
target_compile_options(tray_menu_test PRIVATE -Wall -Wextra -UNDEBUG)
add_test(NAME tray_menu_test COMMAND tray_menu_test)
set_tests_properties(tray_menu_test PROPERTIES TIMEOUT 15)

# --- keeby-settings: GTK4 settings window (docs/012) ------------------
# Fully independent of keeby_core -- no audio/input/engine code, only a
# GDBus client (org.keeby.Control1) and plain GTK4 widgets. Optional so the
# engine still builds on a system without GTK4 installed.
option(KEEBY_BUILD_SETTINGS_UI "Build the keeby-settings GTK4 window" ON)
if(KEEBY_BUILD_SETTINGS_UI)
    pkg_check_modules(GTK4 IMPORTED_TARGET gtk4)
    if(NOT GTK4_FOUND)
        message(WARNING "KEEBY_BUILD_SETTINGS_UI is ON but gtk4 was not found via pkg-config; skipping keeby-settings")
    else()
        add_executable(keeby-settings
            src/ui/keeby_settings_main.cpp
            src/ui/app.cpp
            src/ui/tabs.cpp
            src/ui/visualizer_tab.cpp
            src/ui/sound_controls.cpp
            src/ui/systemctl.cpp
            src/ui/kbus_client.cpp
        )
        target_include_directories(keeby-settings PRIVATE src src/ui)
        target_link_libraries(keeby-settings PRIVATE PkgConfig::GTK4)
        target_compile_options(keeby-settings PRIVATE -Wall -Wextra)
        # Dev/unpackaged-run fallback when "keeby" isn't in the icon theme yet
        # (see src/ui/tabs.cpp) -- packaging installs the real hicolor icon.
        target_compile_definitions(keeby-settings PRIVATE
            KEEBY_ICON_DEV_PATH="${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/keeby.svg")
        install(TARGETS keeby-settings RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(FILES assets/icons/keeby.svg
                DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
                RENAME keeby.svg)

        # Pure logic (grouping/color/tone-mapping/throttle): no GTK/D-Bus
        # needed, always runs.
        add_executable(settings_logic_test tests/settings_logic_test.cpp)
        target_include_directories(settings_logic_test PRIVATE src)
        target_compile_options(settings_logic_test PRIVATE -Wall -Wextra -UNDEBUG)
        add_test(NAME settings_logic_test COMMAND settings_logic_test)
        set_tests_properties(settings_logic_test PROPERTIES TIMEOUT 15)

        # Test-only fake org.keeby.Control1 service (see tests/fake_keeby_service.cpp),
        # a full implementation of the contract on a caller-chosen bus name --
        # never the real org.keeby.Keeby.
        add_executable(fake_keeby_service tests/fake_keeby_service.cpp)
        target_link_libraries(fake_keeby_service PRIVATE PkgConfig::GTK4)
        target_compile_options(fake_keeby_service PRIVATE -Wall -Wextra)

        add_executable(kbus_client_test tests/kbus_client_test.cpp src/ui/kbus_client.cpp)
        target_include_directories(kbus_client_test PRIVATE src src/ui)
        target_link_libraries(kbus_client_test PRIVATE PkgConfig::GTK4)
        target_compile_options(kbus_client_test PRIVATE -Wall -Wextra -UNDEBUG)
        target_compile_definitions(kbus_client_test PRIVATE
            FAKE_KEEBY_SERVICE_PATH="$<TARGET_FILE:fake_keeby_service>")
        add_test(NAME kbus_client_test COMMAND kbus_client_test)
        # SKIP (exit 77) with no session bus: never touches a real running KEEBY
        # (unique test-only bus name -- see the test).
        set_tests_properties(kbus_client_test PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77)
    endif()
endif()

# --- keeby-visualizer: floating on-screen-keyboard overlay (docs/013) -----
# GTK4 + gtk4-layer-shell. Optional: skipped with a warning if either
# dependency is missing so the rest of the build (including the pure-logic
# visualizer_test above) is unaffected.
option(KEEBY_BUILD_VISUALIZER "Build the keeby-visualizer overlay" ON)
if(KEEBY_BUILD_VISUALIZER)
    pkg_check_modules(GTK4 IMPORTED_TARGET gtk4)
    pkg_check_modules(GTK4LAYERSHELL IMPORTED_TARGET gtk4-layer-shell-0)
    pkg_check_modules(WAYLAND_CLIENT IMPORTED_TARGET wayland-client)
    if(NOT GTK4_FOUND)
        message(WARNING "KEEBY_BUILD_VISUALIZER is ON but gtk4 was not found via pkg-config; skipping keeby-visualizer")
    elseif(NOT GTK4LAYERSHELL_FOUND)
        message(WARNING "KEEBY_BUILD_VISUALIZER is ON but gtk4-layer-shell-0 was not found via pkg-config; skipping keeby-visualizer")
    else()
        add_executable(keeby-visualizer src/visualizer/viz_app.cpp)
        target_include_directories(keeby-visualizer PRIVATE src src/visualizer)
        # wayland-client is linked explicitly: viz_app.cpp calls
        # wl_surface_set_input_region()/wl_compositor_create_region()
        # directly (for real pointer click-through) and those symbols are
        # only pulled transitively by GTK's *shared* object, not re-exported
        # to a linker that needs them resolved at link time.
        target_link_libraries(keeby-visualizer PRIVATE PkgConfig::GTK4 PkgConfig::GTK4LAYERSHELL
                                                         PkgConfig::WAYLAND_CLIENT)
        target_compile_options(keeby-visualizer PRIVATE -Wall -Wextra)
        install(TARGETS keeby-visualizer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

        # Backdrop transparency regression (headless render-node check under
        # a private gtk4-broadwayd display -- no compositor, screen, or
        # session bus needed; SKIPs without gtk4-broadwayd).
        add_executable(visualizer_backdrop_test tests/visualizer_backdrop_test.cpp)
        target_include_directories(visualizer_backdrop_test PRIVATE src src/visualizer)
        target_link_libraries(visualizer_backdrop_test PRIVATE PkgConfig::GTK4)
        target_compile_options(visualizer_backdrop_test PRIVATE -Wall -Wextra -UNDEBUG)
        add_test(NAME visualizer_backdrop_test COMMAND visualizer_backdrop_test)
        set_tests_properties(visualizer_backdrop_test PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    endif()
endif()
