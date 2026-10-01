#include "engine_controller.hpp"
#include "input_wire.hpp"

#include <pipewire/pipewire.h>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <thread>

// PipeWire stream wraps (AudioBoundary side) plus a fake keeby-inputd
// (InputCapture side, via KEEBY_INPUTD_PATH — see input_capture_test.cpp
// for why this replaced direct libevdev wraps): EngineController composes
// both AudioBoundary and InputCapture, so both need stubbing to run
// headless, with no real audio server, keyboard, or setgid privilege.
namespace {
int stream_token;
bool fail_connect = false;
void use_fake_helper(const char* mode) {
    setenv(keeby::wire::kHelperPathOverrideEnv, FAKE_INPUTD_PATH, 1);
    if (mode) setenv("FAKE_INPUTD_MODE", mode, 1);
    else unsetenv("FAKE_INPUTD_MODE");
}
}
extern "C" {
pw_stream* __wrap_pw_stream_new_simple(pw_loop*, const char*, pw_properties* props,
                                       const pw_stream_events*, void*) {
    pw_properties_free(props);
    return reinterpret_cast<pw_stream*>(&stream_token);
}
int __wrap_pw_stream_connect(pw_stream*, pw_direction, uint32_t, pw_stream_flags,
                             const spa_pod**, uint32_t) { return fail_connect ? -5 : 0; }
void __wrap_pw_stream_destroy(pw_stream*) {}
pw_buffer* __wrap_pw_stream_dequeue_buffer(pw_stream*) { return nullptr; }
int __wrap_pw_stream_queue_buffer(pw_stream*, pw_buffer*) { return 0; }
int __real_pw_thread_loop_start(pw_thread_loop*);
int __wrap_pw_thread_loop_start(pw_thread_loop* loop) { return __real_pw_thread_loop_start(loop); }
void __real_pw_thread_loop_destroy(pw_thread_loop*);
void __wrap_pw_thread_loop_destroy(pw_thread_loop* loop) { __real_pw_thread_loop_destroy(loop); }
void __real_pw_context_destroy(pw_context*);
void __wrap_pw_context_destroy(pw_context* context) { __real_pw_context_destroy(context); }
}

int main() {
    use_fake_helper(nullptr); // "normal" mode for most of this file

    {
        keeby::EngineController engine({});
        assert(engine.start(""));
        assert(!engine.start("")); // no second start while running
        engine.stop();
        engine.stop(); // idempotent
        assert(engine.start("")); // restart after stop
        engine.stop();
    } // destructor stops safely even without an explicit stop() call

    { // start() rolls back a half-started engine (audio already up) when
      // the input side fails — simulated via a helper that never becomes
      // ready, the same failure InputCapture itself tests directly.
        use_fake_helper("no_ready");
        keeby::EngineController engine({});
        assert(!engine.start(""));
        use_fake_helper(nullptr);
    }

    { // enable/disable and master gain forward through the controller
        keeby::EngineController engine({});
        assert(engine.enabled());
        engine.set_enabled(false);
        assert(!engine.enabled());
        engine.set_enabled(true);
        assert(engine.enabled());

        assert(engine.master_gain() == 1.0f);
        engine.set_master_gain(0.0f);
        assert(engine.master_gain() == 0.0f);
        engine.set_master_gain(0.5f);
        assert(engine.master_gain() == 0.5f);
        engine.set_master_gain(2.0f); // out of range: clamped
        assert(engine.master_gain() == 1.0f);
        engine.set_master_gain(-1.0f); // out of range: clamped
        assert(engine.master_gain() == 0.0f);
    }

    { // profile is reported and only a real profile id validates
        keeby::EngineController engine({});
        assert(engine.profile() == "default");
        bool has_default = false;
        for (const auto& pack : engine.available_profiles())
            if (pack.id == "default") has_default = true;
        assert(has_default);
        assert(engine.set_profile("default"));
        assert(engine.profile() == "default");
        assert(!engine.set_profile("nonexistent"));
        assert(engine.profile() == "default"); // a failed switch leaves the old profile in place
    }

    { // device_connected() forwards through to InputCapture
        keeby::EngineController engine({});
        assert(engine.start(""));
        assert(engine.device_connected());
        engine.stop();
    }

    { // Step 2.7 review fix 3: set_profile's swap section shares
      // lifecycle_mutex_ with start()/stop() (also called from the
      // destructor), so a profile switch on one thread can never race
      // AudioBoundary's started_/retired_ against a stop()/start() cycle on
      // another -- must be clean under TSAN (the race itself) and ASAN
      // (any UAF the race would otherwise cause).
        keeby::EngineController engine({});
        assert(engine.start(""));

        std::atomic<bool> stop_flag{false};
        std::thread profiler([&] {
            while (!stop_flag.load(std::memory_order_relaxed))
                (void)engine.set_profile("default"); // result ignored: "default" always loads
        });
        for (int i = 0; i < 6; ++i) {
            engine.stop();
            assert(engine.start(""));
        }
        stop_flag.store(true, std::memory_order_relaxed);
        profiler.join();
        engine.stop();
    }

    unsetenv(keeby::wire::kHelperPathOverrideEnv);
    unsetenv("FAKE_INPUTD_MODE");
    std::puts("engine_controller_test: OK (lifecycle, rollback, idempotent stop, "
              "enable/disable, gain clamping, profile reporting, device_connected forward, "
              "set_profile/stop/start concurrency)");
}
