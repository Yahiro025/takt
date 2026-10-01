#include <sdbus-c++/sdbus-c++.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "control_service.hpp"
#include "engine_controller.hpp"
#include "input_capture.hpp"
#include "paths.hpp"
#include "settings.hpp"
#include "sound_pack.hpp"
#include "tray_service.hpp"
#include "visualizer_wire.hpp"

namespace {
std::atomic<bool> g_running{true};
void handle_signal(int) { g_running.store(false, std::memory_order_relaxed); }

keeby::Settings current_settings(const keeby::EngineController& engine) {
    const auto [tone_x, tone_y] = engine.tone();
    keeby::Settings s;
    s.enabled = engine.enabled();
    s.volume = engine.master_gain();
    s.stereo_width = engine.stereo_width();
    s.profile = engine.profile();
    s.tone_x = tone_x;
    s.tone_y = tone_y;
    s.visualizer = engine.visualizer_enabled();
    s.visualizer_position = keeby::viz::to_string(engine.visualizer_position());
    s.visualizer_dismiss_ms = engine.visualizer_dismiss_ms();
    s.visualizer_follow_speed = static_cast<float>(engine.visualizer_follow_speed());
    return s;
}

void save_now(const std::filesystem::path& path, const keeby::EngineController& engine) {
    if (auto res = keeby::save_settings(path, current_settings(engine)); !res) {
        std::fprintf(stderr, "keeby: %s\n", res.error().c_str());
    }
}
} // namespace

int main(int argc, char** argv) try {
    // `keeby ctl ...`: a one-shot D-Bus client call, nothing else. No engine,
    // no audio, no input helper, no settings load.
    if (argc > 1 && std::string_view(argv[1]) == "ctl") {
        return keeby::run_ctl(std::vector<std::string>(argv + 2, argv + argc));
    }

    // Loads every available profile (no audio/input/D-Bus/settings touched)
    // and reports ok/error per id -- safe to run alongside a live `keeby`
    // instance, e.g. to sanity-check newly downloaded packs.
    if (argc > 1 && std::string_view(argv[1]) == "--check-profiles") {
        bool all_ok = true;
        for (const auto& pack : keeby::list_packs()) {
            std::vector<std::string> warnings;
            auto bank = pack.id == "default" ? keeby::load_default_bank(keeby::paths::asset_dir())
                                              : keeby::load_pack(pack.dir, &warnings);
            if (bank) {
                std::printf("ok %s\n", pack.id.c_str());
                for (const auto& w : warnings) std::printf("  %s\n", w.c_str());
            } else {
                std::printf("error %s: %s\n", pack.id.c_str(), bank.error().c_str());
                all_ok = false;
            }
        }
        return all_ok ? 0 : 1;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    // The tray's "Settings…" item spawns keeby-settings detached (posix_spawnp,
    // no waitpid -- see tray_service.cpp); ignoring SIGCHLD makes the kernel
    // reap it directly instead of leaving a zombie for this long-running process.
    std::signal(SIGCHLD, SIG_IGN);

    // Empty means "let keeby-inputd auto-discover a keyboard": this process
    // is unprivileged and cannot open /dev/input/* itself to check, so
    // discovery (like the actual device open) happens only inside the
    // privileged helper. See docs/006-step-2.5-security-permissions.md.
    std::string device_path;
    std::string profile_arg; // set only by --profile; empty means "use settings"
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--list-profiles") {
            for (const auto& pack : keeby::list_packs())
                std::printf("%s\t%s\t%s\n", pack.id.c_str(), pack.name.c_str(), pack.dir.c_str());
            return 0;
        } else if (arg == "--profile") {
            if (++i >= argc) {
                std::fprintf(stderr, "keeby: --profile requires an argument\n");
                return 1;
            }
            profile_arg = argv[i];
        } else if (device_path.empty()) {
            device_path = argv[i];
        }
    }

    // One shared session-bus connection for the tray, the control service and
    // the single-instance guard. A missing bus degrades to "no tray/control",
    // exactly like today's tray-unavailable handling -- it never blocks audio.
    std::unique_ptr<sdbus::IConnection> bus;
    try {
        bus = sdbus::createSessionBusConnection();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "keeby: session bus unreachable, continuing without tray/control: %s\n", e.what());
    }
    if (bus && !keeby::acquire_instance_name(*bus)) {
        std::fprintf(stderr, "keeby: already running — use `keeby ctl ...`\n");
        return 3; // distinct from other failures: packaging/keeby.service excludes this from restart
    }

    const auto settings_path = keeby::default_settings_path();
    std::vector<std::string> settings_warnings;
    keeby::Settings settings = keeby::load_settings(settings_path, &settings_warnings);
    for (const auto& w : settings_warnings) std::fprintf(stderr, "keeby: %s\n", w.c_str());
    std::atomic<bool> settings_dirty{false};

    // Precedence: --profile > settings.profile > "default" (Settings' own
    // default), all funneled through one variable.
    std::string profile_id = !profile_arg.empty() ? profile_arg : settings.profile;

    auto load_profile = [](const std::string& id) -> std::expected<keeby::SoundBank, std::string> {
        if (id == "default") return keeby::load_default_bank(keeby::paths::asset_dir());
        for (const auto& pack : keeby::list_packs())
            if (pack.id == id) {
                std::vector<std::string> warnings;
                auto bank = keeby::load_pack(pack.dir, &warnings);
                for (const auto& w : warnings) std::fprintf(stderr, "keeby: %s\n", w.c_str());
                return bank;
            }
        return std::unexpected("unknown profile: " + id);
    };

    auto samples = load_profile(profile_id);
    if (!samples) {
        std::fprintf(stderr, "keeby: profile '%s' failed to load: %s; falling back to default\n",
                     profile_id.c_str(), samples.error().c_str());
        profile_id = "default";
        settings_dirty.store(true, std::memory_order_relaxed); // persist the corrected profile
        samples = keeby::load_default_bank(keeby::paths::asset_dir());
        if (!samples) {
            std::fprintf(stderr, "keeby: default profile failed to load: %s\n", samples.error().c_str());
            return 1;
        }
    }
    keeby::EngineController engine(std::move(*samples), keeby::MixerVariation{}, profile_id);
    engine.set_enabled(settings.enabled);
    engine.set_master_gain(settings.volume);
    engine.set_stereo_width(settings.stereo_width);
    engine.set_tone(settings.tone_x, settings.tone_y);

    if (auto res = engine.start(device_path); !res) {
        std::fprintf(stderr, "keeby: engine failed to start: %s\n", res.error().c_str());
        return 1;
    }

    // Visualizer: position/dismiss_ms are applied before enabling so a
    // startup spawn's first config message already reflects them (see
    // VisualizerService::set_enabled). load_settings() already validated
    // visualizer_position; position_from_string() should never fail here,
    // but a corrupted/hand-edited file falls back to the compiled default
    // rather than crash.
    engine.set_visualizer_position(
        keeby::viz::position_from_string(settings.visualizer_position).value_or(keeby::viz::kDefaultPosition));
    engine.set_visualizer_dismiss_ms(settings.visualizer_dismiss_ms);
    engine.set_visualizer_follow_speed(settings.visualizer_follow_speed);
    engine.set_visualizer_enabled(settings.visualizer);

    std::fprintf(stderr,
                 "keeby: engine started (%u Hz, %u ch, 32 voices, press+release) on \"%s\", not grabbed, "
                 "profile \"%s\"\n",
                 keeby::kNativeSampleRate, keeby::kNativeChannels,
                 device_path.empty() ? "auto-discovered by keeby-inputd" : device_path.c_str(), profile_id.c_str());
    std::fprintf(stderr, "keeby: active profile determines press/release mappings; auto-repeat is silent\n");
    std::fprintf(stderr, "keeby: master volume scales up to 2x before output clamp; "
                          "tone and stereo width are configurable\n");

    // refresh_and_mark_dirty captures `tray` by reference: it's constructed
    // below (still null here), but the callback isn't invoked until later,
    // once tray (if any) is fully set up.
    std::unique_ptr<keeby::TrayService> tray;
    std::unique_ptr<keeby::ControlService> control;
    auto refresh_and_mark_dirty = [&] {
        settings_dirty.store(true, std::memory_order_relaxed);
        if (tray) tray->refresh();
        // Lets an open keeby-settings window refresh live -- see
        // docs/011-tone-and-control-api.md's StateChanged contract.
        if (control) control->notify_state_changed();
    };

    if (bus) {
        tray = std::make_unique<keeby::TrayService>(
            *bus, engine, [] { g_running.store(false, std::memory_order_relaxed); }, refresh_and_mark_dirty);
        if (auto res = tray->start(); !res) {
            std::fprintf(stderr, "keeby: tray unavailable, continuing without it: %s\n", res.error().c_str());
            tray.reset();
        } else {
            std::fprintf(stderr, "keeby: tray published (left-click toggles enabled, right-click shows the "
                                  "menu, scroll adjusts volume, middle-click quits)\n");
        }

        keeby::ControlHandlers handlers;
        handlers.get_state = [&engine] {
            return keeby::ControlState{engine.enabled(), engine.master_gain(), engine.stereo_width(),
                                        engine.profile()};
        };
        handlers.set_enabled = [&](bool e) {
            engine.set_enabled(e);
            refresh_and_mark_dirty();
        };
        handlers.set_volume = [&](double v) {
            engine.set_master_gain(static_cast<float>(std::clamp(v, 0.0, 1.0)));
            refresh_and_mark_dirty();
        };
        handlers.set_stereo_width = [&](double w) {
            engine.set_stereo_width(static_cast<float>(std::clamp(w, 0.0, 2.0)));
            refresh_and_mark_dirty();
        };
        handlers.set_profile = [&](const std::string& id) -> std::expected<void, std::string> {
            auto res = engine.set_profile(id);
            if (res) refresh_and_mark_dirty(); // failure: leave the current profile, nothing to persist
            return res;
        };
        handlers.list_profiles = [&engine] {
            std::vector<std::string> ids;
            for (const auto& pack : engine.available_profiles()) ids.push_back(pack.id);
            return ids;
        };
        handlers.list_profiles_detailed = [&engine] {
            std::vector<std::pair<std::string, std::string>> out;
            for (const auto& pack : engine.available_profiles()) out.emplace_back(pack.id, pack.name);
            return out;
        };
        handlers.get_tone = [&engine] {
            const auto [x, y] = engine.tone();
            return std::pair<double, double>{x, y};
        };
        handlers.set_tone = [&](double x, double y) {
            engine.set_tone(static_cast<float>(x), static_cast<float>(y));
            refresh_and_mark_dirty();
        };
        handlers.device_connected = [&engine] { return engine.device_connected(); };
        handlers.get_visualizer = [&engine] {
            return keeby::VisualizerState{engine.visualizer_enabled(), engine.visualizer_position(),
                                           engine.visualizer_dismiss_ms(), engine.visualizer_follow_speed()};
        };
        handlers.set_visualizer = [&](bool e) {
            engine.set_visualizer_enabled(e);
            refresh_and_mark_dirty();
        };
        handlers.set_visualizer_position = [&](keeby::viz::Position p) {
            engine.set_visualizer_position(p);
            refresh_and_mark_dirty();
        };
        handlers.set_visualizer_dismiss = [&](uint32_t ms) {
            engine.set_visualizer_dismiss_ms(ms);
            refresh_and_mark_dirty();
        };
        handlers.set_visualizer_follow_speed = [&](double speed) {
            engine.set_visualizer_follow_speed(speed);
            refresh_and_mark_dirty();
        };
        handlers.quit = [] { g_running.store(false, std::memory_order_relaxed); };

        control = std::make_unique<keeby::ControlService>(*bus, std::move(handlers));

        // Every object (SNI, its menu, the control service) is exported by
        // this point: only now does the shared connection start dispatching.
        bus->enterEventLoopAsync();
    }

    std::fprintf(stderr, "keeby: running. Press Ctrl+C to stop.\n");
    bool reported_disconnect = false;
    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!reported_disconnect && !engine.device_connected()) {
            reported_disconnect = true;
            std::fprintf(stderr, "keeby: keyboard device disappeared; no further keyboard sounds "
                                  "will trigger. The app keeps running (tray/Ctrl+C quit still work); "
                                  "no automatic reconnect is attempted.\n");
        }
        // At most one write per half-second even under a burst of scroll/ctl
        // calls, and off the D-Bus and RT threads.
        if (settings_dirty.exchange(false, std::memory_order_relaxed)) save_now(settings_path, engine);
    }

    // Shutdown order: leave the event loop first (no more handlers can start
    // touching `engine` or `tray`/`control` after this), then destroy
    // ControlService/TrayService (and its DBusMenu), then stop the engine,
    // then save settings once more.
    if (bus) bus->leaveEventLoop();
    control.reset();
    tray.reset();
    engine.stop();
    save_now(settings_path, engine);

    const auto& c = engine.counters();
    std::fprintf(stderr, "\n--- keeby session stats ---\n");
    std::fprintf(stderr, "presses:        %llu\n", (unsigned long long)c.presses.load());
    std::fprintf(stderr, "releases:       %llu\n", (unsigned long long)c.releases.load());
    std::fprintf(stderr, "repeats:        %llu\n", (unsigned long long)c.repeats.load());
    std::fprintf(stderr, "consumed total: %llu\n", (unsigned long long)c.consumed_total.load());
    std::fprintf(stderr, "last code seen: %u\n", (unsigned)c.last_code.load());
    std::fprintf(stderr, "budget exhausted (callbacks that hit the %zu-event cap): %llu\n",
                 keeby::kMaxEventsPerCallback, (unsigned long long)c.budget_exhausted.load());
    std::fprintf(stderr, "dropped events: %llu\n", (unsigned long long)engine.transport_dropped_count());
    std::fprintf(stderr, "voices started: %llu\n", (unsigned long long)c.voices_started.load());
    std::fprintf(stderr, "voices dropped (pool full): %llu\n", (unsigned long long)c.voices_dropped.load());
    std::fprintf(stderr, "final enabled: %s, final volume: %d%%\n",
                 engine.enabled() ? "true" : "false",
                 static_cast<int>(engine.master_gain() * 100.0f + 0.5f));
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "keeby: startup/runtime failure: %s\n", error.what());
    return 1;
}
