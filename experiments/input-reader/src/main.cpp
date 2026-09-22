#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>

#include <linux/input-event-codes.h>

#include "audio_engine.hpp"
#include "keyboard_device.hpp"
#include "sample.hpp"

#ifndef ASSET_DIR
#define ASSET_DIR "assets"
#endif

namespace {
std::atomic<bool> g_running{true};
void handle_signal(int) { g_running.store(false, std::memory_order_relaxed); }
} // namespace

int main(int argc, char** argv) {
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

    keeby::AudioEngine engine;

    keeby::Sample down_sample, up_sample;
    const std::string down_path = std::string(ASSET_DIR) + "/click_down.wav";
    const std::string up_path = std::string(ASSET_DIR) + "/click_up.wav";
    if (!keeby::load_wav_sample(down_path, keeby::kNativeSampleRate, keeby::kNativeChannels, down_sample) ||
        !keeby::load_wav_sample(up_path, keeby::kNativeSampleRate, keeby::kNativeChannels, up_sample)) {
        std::fprintf(stderr, "keeby: failed to preload sound assets\n");
        return 1;
    }
    const int down_idx = engine.add_sample(std::move(down_sample));
    const int up_idx = engine.add_sample(std::move(up_sample));

    // Small fixed set of physical keys mapped to sound, per the feasibility
    // spec — not a full keymap.
    const uint16_t mapped_keys[] = {KEY_A, KEY_S, KEY_D, KEY_SPACE, KEY_ENTER};
    for (uint16_t k : mapped_keys) {
        engine.map_key(k, down_idx, up_idx);
    }

    if (!engine.start()) {
        std::fprintf(stderr, "keeby: failed to start PipeWire audio engine\n");
        return 1;
    }
    std::fprintf(stderr, "keeby: audio engine started (%u Hz, %u ch)\n", keeby::kNativeSampleRate,
                 keeby::kNativeChannels);
    std::fprintf(stderr, "keeby: mapped keys: A S D SPACE ENTER\n");

    keeby::KeyboardDeviceStats stats;
    std::thread reader([&] {
        keeby::run_keyboard_reader(device_path, engine.event_queue(), g_running, stats);
    });

    std::fprintf(stderr, "keeby: running on \"%s\". Press Ctrl+C to stop.\n", device_path.c_str());
    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    reader.join();
    engine.stop();

    const uint64_t n = engine.latency_stats().count.load();
    std::fprintf(stderr, "\n--- keeby spike stats ---\n");
    std::fprintf(stderr, "events read:                 %llu\n", (unsigned long long)stats.events_read);
    std::fprintf(stderr, "events dropped (ring full):  %llu\n", (unsigned long long)stats.events_dropped);
    std::fprintf(stderr, "sounds triggered:             %llu\n", (unsigned long long)n);
    if (n > 0) {
        const double avg_ms = (engine.latency_stats().sum_ns.load() / static_cast<double>(n)) / 1e6;
        const double max_ms = engine.latency_stats().max_ns.load() / 1e6;
        std::fprintf(stderr, "avg trigger latency:          %.3f ms\n", avg_ms);
        std::fprintf(stderr, "max trigger latency:          %.3f ms\n", max_ms);
    }
    return 0;
}
