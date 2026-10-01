#pragma once

// Runtime loader for libinput (docs/006-step-2.5-security-permissions.md's
// "Exact pointer tracking via libinput" section). keeby-inputd's ELF file
// links ONLY libevdev (plus libc/libstdc++/libgcc/libm) -- never libinput.
// Linking libinput directly would add it, and its transitive dependencies
// (libwacom, libgudev, GLib/GObject, liblua5.4, libpcre2, libffi, mtdev,
// libudev), as ELF NEEDED entries. Those libraries' own ELF constructors
// (e.g. GLib's own init, which reads G_DEBUG/G_MESSAGES_DEBUG etc.) would
// then run as part of process startup, i.e. before main() -- before
// drop_privilege_or_die() -- inside a still-setgid-input process. GLib
// explicitly does not support setuid/setgid use, so this must never happen.
//
// Instead, keeby-inputd calls load_libinput_api() only AFTER the privilege
// drop. It dlopen()s libinput.so by bare soname (never a path), which means
// glibc's secure-execution mode governs the search: the kernel latches
// AT_SECURE for this process's entire lifetime because the binary was
// exec'd setgid, regardless of this process voluntarily dropping that
// group later -- so LD_LIBRARY_PATH/LD_PRELOAD are already ignored for this
// dlopen() exactly as they were for the original exec, and only trusted
// system library directories are ever searched.
//
// Every libinput symbol keeby-inputd calls is resolved by name via dlsym()
// into this struct of function pointers, each typed via decltype(&real_fn)
// so a signature mismatch is a compile error. Nothing here is on any
// real-time or privileged path, so failure (library or symbol missing) is
// handled by returning a default (unloaded) LibinputApi rather than dying:
// the caller falls back to keyboard-only, exactly as when no pointer
// device exists at all.
//
// This header only ever takes the ADDRESS of libinput_* names inside
// decltype() (an unevaluated context -- it names a type, it never emits a
// reference the linker has to resolve), so merely including this header,
// or libinput.h, creates no link-time dependency on libinput. Never call a
// libinput_* function directly anywhere in keeby-inputd: always go through
// a loaded LibinputApi instance.

#include <dlfcn.h>

#include <libinput.h>

namespace keeby::pointer {

struct LibinputApi {
    void* handle = nullptr;

    decltype(&libinput_path_create_context) path_create_context = nullptr;
    decltype(&libinput_path_add_device) path_add_device = nullptr;
    decltype(&libinput_log_set_priority) log_set_priority = nullptr;
    decltype(&libinput_device_set_user_data) device_set_user_data = nullptr;
    decltype(&libinput_device_get_user_data) device_get_user_data = nullptr;
    decltype(&libinput_device_config_accel_set_speed) config_accel_set_speed = nullptr;
    decltype(&libinput_device_config_accel_set_profile) config_accel_set_profile = nullptr;
    decltype(&libinput_device_config_tap_set_enabled) config_tap_set_enabled = nullptr;
    decltype(&libinput_device_config_tap_set_button_map) config_tap_set_button_map = nullptr;
    decltype(&libinput_device_config_dwt_set_enabled) config_dwt_set_enabled = nullptr;
    decltype(&libinput_device_config_dwtp_set_enabled) config_dwtp_set_enabled = nullptr;
    decltype(&libinput_device_config_tap_set_drag_lock_enabled) config_tap_set_drag_lock_enabled = nullptr;
    decltype(&libinput_get_fd) get_fd = nullptr;
    decltype(&libinput_dispatch) dispatch = nullptr;
    decltype(&libinput_get_event) get_event = nullptr;
    decltype(&libinput_event_get_type) event_get_type = nullptr;
    decltype(&libinput_event_get_device) event_get_device = nullptr;
    decltype(&libinput_event_get_pointer_event) event_get_pointer_event = nullptr;
    decltype(&libinput_event_pointer_get_dx) event_pointer_get_dx = nullptr;
    decltype(&libinput_event_pointer_get_dy) event_pointer_get_dy = nullptr;
    decltype(&libinput_event_pointer_get_time_usec) event_pointer_get_time_usec = nullptr;
    decltype(&libinput_event_pointer_get_button) event_pointer_get_button = nullptr;
    decltype(&libinput_event_pointer_get_button_state) event_pointer_get_button_state = nullptr;
    decltype(&libinput_event_destroy) event_destroy = nullptr;
    decltype(&libinput_unref) unref = nullptr;

    bool loaded() const noexcept { return handle != nullptr; }
};

namespace detail {
template <typename Fn>
bool load_sym(void* handle, const char* name, Fn& out) {
    out = reinterpret_cast<Fn>(dlsym(handle, name));
    return out != nullptr;
}
} // namespace detail

// dlopen()s libinput by its stable soname and resolves every symbol above.
// Returns a default-constructed (handle == nullptr, loaded() == false)
// LibinputApi if the library can't be opened or any single symbol is
// missing -- in the latter case the (fully local, not-yet-escaped) handle
// is closed here before returning, since nothing outside this function can
// have used it yet. Once a fully-resolved LibinputApi is returned, this
// header never closes it again: the caller must not dlclose() it while a
// libinput context created through it is still alive, and since this
// process is short-lived and single-purpose, the simplest and safest rule
// is to never dlclose() at all once loading has fully succeeded.
inline LibinputApi load_libinput_api() {
    LibinputApi api;
    void* h = dlopen("libinput.so.10", RTLD_NOW | RTLD_LOCAL);
    if (!h) return api;

    bool ok = true;
    ok &= detail::load_sym(h, "libinput_path_create_context", api.path_create_context);
    ok &= detail::load_sym(h, "libinput_path_add_device", api.path_add_device);
    ok &= detail::load_sym(h, "libinput_log_set_priority", api.log_set_priority);
    ok &= detail::load_sym(h, "libinput_device_set_user_data", api.device_set_user_data);
    ok &= detail::load_sym(h, "libinput_device_get_user_data", api.device_get_user_data);
    ok &= detail::load_sym(h, "libinput_device_config_accel_set_speed", api.config_accel_set_speed);
    ok &= detail::load_sym(h, "libinput_device_config_accel_set_profile", api.config_accel_set_profile);
    ok &= detail::load_sym(h, "libinput_device_config_tap_set_enabled", api.config_tap_set_enabled);
    ok &= detail::load_sym(h, "libinput_device_config_tap_set_button_map", api.config_tap_set_button_map);
    ok &= detail::load_sym(h, "libinput_device_config_dwt_set_enabled", api.config_dwt_set_enabled);
    ok &= detail::load_sym(h, "libinput_device_config_dwtp_set_enabled", api.config_dwtp_set_enabled);
    ok &= detail::load_sym(h, "libinput_device_config_tap_set_drag_lock_enabled", api.config_tap_set_drag_lock_enabled);
    ok &= detail::load_sym(h, "libinput_get_fd", api.get_fd);
    ok &= detail::load_sym(h, "libinput_dispatch", api.dispatch);
    ok &= detail::load_sym(h, "libinput_get_event", api.get_event);
    ok &= detail::load_sym(h, "libinput_event_get_type", api.event_get_type);
    ok &= detail::load_sym(h, "libinput_event_get_device", api.event_get_device);
    ok &= detail::load_sym(h, "libinput_event_get_pointer_event", api.event_get_pointer_event);
    ok &= detail::load_sym(h, "libinput_event_pointer_get_dx", api.event_pointer_get_dx);
    ok &= detail::load_sym(h, "libinput_event_pointer_get_dy", api.event_pointer_get_dy);
    ok &= detail::load_sym(h, "libinput_event_pointer_get_time_usec", api.event_pointer_get_time_usec);
    ok &= detail::load_sym(h, "libinput_event_pointer_get_button", api.event_pointer_get_button);
    ok &= detail::load_sym(h, "libinput_event_pointer_get_button_state", api.event_pointer_get_button_state);
    ok &= detail::load_sym(h, "libinput_event_destroy", api.event_destroy);
    ok &= detail::load_sym(h, "libinput_unref", api.unref);

    if (!ok) {
        dlclose(h);
        return LibinputApi{};
    }
    api.handle = h;
    return api;
}

} // namespace keeby::pointer
