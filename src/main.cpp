#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <exception>
#include <string>
#include <thread>
#include <utility>

#include "audio_boundary.hpp"
#include "input_capture.hpp"

namespace {
std::atomic<bool> g_running{true};
void handle_signal(int) { g_running.store(false, std::memory_order_relaxed); }
} // namespace

int main(int argc, char** argv) try {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::string device_path;
    if (argc > 1) {
        device_path = argv[1];
    } else {
        auto found = keeby::discover_keyboard_device();
        if (!found) {
            std::fprintf(stderr, "keeby: no physical keyboard found under /dev/input/event*\n");
            return 1;
        }
        device_path = *found;
    }

    keeby::KeyEventTransport transport;

    keeby::SampleBank samples;
    const char* names[] = {"click_down.wav", "click_up.wav"};
    for (std::size_t i = 0; i < samples.size(); ++i) {
        auto loaded = keeby::load_wav_sample(std::string(KEEBY_ASSET_DIR) + "/" + names[i]);
        if (!loaded) {
            std::fprintf(stderr, "keeby: sample load failed: %s\n", loaded.error().c_str());
            return 1;
        }
        samples[i] = std::move(*loaded);
    }
    keeby::AudioBoundary audio(std::move(samples));
    if (auto res = audio.start(transport); !res) {
        std::fprintf(stderr, "keeby: audio boundary failed to start: %s\n", res.error().c_str());
        return 1;
    }
    std::fprintf(stderr, "keeby: audio boundary started (%u Hz, %u ch, 32 voices, press only)\n",
                 keeby::kNativeSampleRate, keeby::kNativeChannels);

    keeby::InputCapture capture;
    if (auto res = capture.start(device_path, transport); !res) {
        std::fprintf(stderr, "keeby: input capture failed to start: %s\n", res.error().c_str());
        return 1;
    }
    std::fprintf(stderr, "keeby: input capture started on \"%s\", not grabbed\n", device_path.c_str());

    std::fprintf(stderr, "keeby: A/S/D = click_down; Space/Enter = click_up; repeats/releases do not trigger or stop sound\n");
    std::fprintf(stderr, "keeby: running. Press Ctrl+C to stop.\n");
    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    capture.stop();
    audio.stop();

    const auto& c = audio.counters();
    std::fprintf(stderr, "\n--- keeby step 2.2 verification stats ---\n");
    std::fprintf(stderr, "presses:        %llu\n", (unsigned long long)c.presses.load());
    std::fprintf(stderr, "releases:       %llu\n", (unsigned long long)c.releases.load());
    std::fprintf(stderr, "repeats:        %llu\n", (unsigned long long)c.repeats.load());
    std::fprintf(stderr, "consumed total: %llu\n", (unsigned long long)c.consumed_total.load());
    std::fprintf(stderr, "last code seen: %u\n", (unsigned)c.last_code.load());
    std::fprintf(stderr, "budget exhausted (callbacks that hit the %zu-event cap): %llu\n",
                 keeby::kMaxEventsPerCallback, (unsigned long long)c.budget_exhausted.load());
    std::fprintf(stderr, "dropped events: %llu\n", (unsigned long long)transport.dropped_count());
    std::fprintf(stderr, "voices started: %llu\n", (unsigned long long)c.voices_started.load());
    std::fprintf(stderr, "voices dropped (pool full): %llu\n", (unsigned long long)c.voices_dropped.load());
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "keeby: startup/runtime failure: %s\n", error.what());
    return 1;
}
