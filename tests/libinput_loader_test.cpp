// Exercises src/libinput_loader.hpp against the REAL system libinput.so:
// resolves every symbol keeby-inputd calls (catching a symbol-name typo at
// build-verification time on any machine, rather than only failing later
// on the user's installed package -- see docs/006's "Exact pointer
// tracking via libinput" section), then creates and immediately tears down
// a minimal path-backend context to prove path_create_context()/unref()
// actually work end-to-end. Adds no devices (needs no /dev/input access,
// no root, no fork): the stub interface's callbacks are never invoked.
#include "libinput_loader.hpp"

#include <cassert>
#include <cstdio>

namespace {
int stub_open_restricted(const char*, int, void*) { return -1; }
void stub_close_restricted(int, void*) {}
constexpr libinput_interface kStubInterface = {
    .open_restricted = stub_open_restricted,
    .close_restricted = stub_close_restricted,
};
} // namespace

int main() {
    keeby::pointer::LibinputApi api = keeby::pointer::load_libinput_api();
    assert(api.loaded());

    assert(api.path_create_context != nullptr);
    assert(api.path_add_device != nullptr);
    assert(api.log_set_priority != nullptr);
    assert(api.device_set_user_data != nullptr);
    assert(api.device_get_user_data != nullptr);
    assert(api.config_accel_set_speed != nullptr);
    assert(api.config_accel_set_profile != nullptr);
    assert(api.config_tap_set_enabled != nullptr);
    assert(api.config_tap_set_button_map != nullptr);
    assert(api.config_dwt_set_enabled != nullptr);
    assert(api.config_dwtp_set_enabled != nullptr);
    assert(api.config_tap_set_drag_lock_enabled != nullptr);
    assert(api.get_fd != nullptr);
    assert(api.dispatch != nullptr);
    assert(api.get_event != nullptr);
    assert(api.event_get_type != nullptr);
    assert(api.event_get_device != nullptr);
    assert(api.event_get_pointer_event != nullptr);
    assert(api.event_pointer_get_dx != nullptr);
    assert(api.event_pointer_get_dy != nullptr);
    assert(api.event_pointer_get_time_usec != nullptr);
    assert(api.event_pointer_get_button != nullptr);
    assert(api.event_pointer_get_button_state != nullptr);
    assert(api.event_destroy != nullptr);
    assert(api.unref != nullptr);

    libinput* ctx = api.path_create_context(&kStubInterface, nullptr);
    assert(ctx != nullptr);
    api.unref(ctx);

    std::puts("libinput_loader_test: OK (every symbol resolved against the real system "
              "libinput.so; path_create_context()/unref() exercised end-to-end)");
}
