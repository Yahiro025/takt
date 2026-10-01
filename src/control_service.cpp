#include "control_service.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <tuple>
#include <utility>

namespace keeby {
namespace {

constexpr const char* kObjectPath = "/org/keeby/Keeby";
constexpr const char* kInterface = "org.keeby.Control1";
constexpr const char* kProfileFailedError = "org.keeby.Error.ProfileFailed";
constexpr const char* kInvalidArgumentError = "org.keeby.Error.InvalidArgument";
constexpr const char* kFailedError = "org.keeby.Error.Failed";
constexpr const char* kServiceUnknownError = "org.freedesktop.DBus.Error.ServiceUnknown";
// Set from CMake's project(keeby VERSION 0.1.0 ...) via a compile
// definition on keeby_core; fall back for any translation unit that somehow
// doesn't get it (there shouldn't be one -- see CMakeLists.txt).
#ifndef KEEBY_VERSION
#define KEEBY_VERSION "0.1.0"
#endif
constexpr const char* kVersion = KEEBY_VERSION;
// sd_bus_request_name() with no queueing flag fails with -EEXIST when the
// name already has an owner; sdbus-c++ 2.3.1 maps that to this D-Bus error
// name (Error.h doesn't document which name, so this was confirmed against
// the installed library: two requestName() calls for the same name produce
// getName() == "org.freedesktop.DBus.Error.FileExists").
constexpr const char* kNameExistsError = "org.freedesktop.DBus.Error.FileExists";

void require_finite(double value) {
    if (!std::isfinite(value)) {
        throw sdbus::Error(sdbus::Error::Name{kInvalidArgumentError}, "value must be finite");
    }
}

// Converts any exception escaping a user-supplied handler into a D-Bus
// error reply instead of letting it cross the vtable dispatch boundary.
template <typename F>
auto call_handler(F&& fn) -> decltype(fn()) {
    try {
        return fn();
    } catch (const sdbus::Error&) {
        throw;
    } catch (const std::exception& e) {
        throw sdbus::Error(sdbus::Error::Name{kFailedError}, e.what());
    } catch (...) {
        throw sdbus::Error(sdbus::Error::Name{kFailedError}, "handler threw an unknown exception");
    }
}

constexpr auto kCtlTimeout = std::chrono::milliseconds(2000);

void print_usage(std::FILE* out) {
    std::fprintf(out,
                 "usage: keeby ctl <command> [args]\n"
                 "commands:\n"
                 "  toggle          flip enabled on/off\n"
                 "  on              enable sound\n"
                 "  off             disable sound\n"
                 "  volume <0-100>  set volume percent\n"
                 "  volume +N|-N    adjust volume by N percentage points\n"
                 "  width <0-200>   set stereo width percent\n"
                 "  tone <x> <y>    set tone pad, each -100..100 percent "
                 "(x: Thock..Clack, y: Warm..Bright)\n"
                 "  profile <id>    switch sound profile\n"
                 "  profiles        list available profiles\n"
                 "  visualizer on|off           toggle the on-screen keyboard visualizer\n"
                 "  visualizer position <p>     top-left|top-center|top-right|\n"
                 "                              bottom-left|bottom-center|bottom-right|\n"
                 "                              follow-cursor (default)\n"
                 "  visualizer dismiss <ms>     250-5000\n"
                 "  visualizer speed <x>        follow-cursor speed multiplier, 0.1-4.0\n"
                 "  status          print current state\n"
                 "  quit            tell the running instance to exit\n"
                 "  help            show this help\n"
                 "exit codes: 0 ok, 1 call failed, 2 not running, 64 usage error\n");
}

struct PercentArg {
    double points;
    bool relative;
};

std::optional<PercentArg> parse_percent(const std::string& s) {
    if (s.empty()) return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || !std::isfinite(v)) return std::nullopt;
    return PercentArg{v, s[0] == '+' || s[0] == '-'};
}

ControlState fetch_state(sdbus::IProxy& proxy) {
    bool enabled{};
    double volume{};
    double width{};
    std::string profile;
    proxy.callMethod("GetState")
        .onInterface(kInterface)
        .withTimeout(kCtlTimeout)
        .storeResultsTo(enabled, volume, width, profile);
    return ControlState{enabled, volume, width, profile};
}

std::pair<double, double> fetch_tone(sdbus::IProxy& proxy) {
    double x{};
    double y{};
    proxy.callMethod("GetTone").onInterface(kInterface).withTimeout(kCtlTimeout).storeResultsTo(x, y);
    return {x, y};
}

struct VisualizerCli {
    bool enabled;
    std::string position;
    uint32_t dismiss_ms;
    double follow_speed;
};

VisualizerCli fetch_visualizer(sdbus::IProxy& proxy) {
    VisualizerCli v{};
    proxy.callMethod("GetVisualizer")
        .onInterface(kInterface)
        .withTimeout(kCtlTimeout)
        .storeResultsTo(v.enabled, v.position, v.dismiss_ms, v.follow_speed);
    return v;
}

void print_state(const ControlState& s, std::pair<double, double> tone, const VisualizerCli& v) {
    std::printf("enabled=%s volume=%ld width=%ld profile=%s tone=%ld,%ld\n", s.enabled ? "true" : "false",
                std::lround(s.volume * 100.0), std::lround(s.stereo_width * 100.0), s.profile.c_str(),
                std::lround(tone.first * 100.0), std::lround(tone.second * 100.0));
    std::printf("visualizer=%s,%s,%u,%.1f\n", v.enabled ? "on" : "off", v.position.c_str(), v.dismiss_ms,
                v.follow_speed);
}

} // namespace

ControlService::ControlService(sdbus::IConnection& connection, ControlHandlers handlers)
    : handlers_(std::move(handlers)) {
    object_ = sdbus::createObject(connection, sdbus::ObjectPath{kObjectPath});
    object_->addVTable(
        sdbus::registerMethod("Toggle").implementedAs([this] {
            return call_handler([this] {
                const bool next = !handlers_.get_state().enabled;
                handlers_.set_enabled(next);
                return next;
            });
        }),
        sdbus::registerMethod("SetEnabled").implementedAs([this](bool enabled) {
            call_handler([this, enabled] { handlers_.set_enabled(enabled); });
        }),
        sdbus::registerMethod("SetVolume").implementedAs([this](double volume) {
            require_finite(volume);
            call_handler([this, volume] { handlers_.set_volume(volume); });
        }),
        sdbus::registerMethod("SetStereoWidth").implementedAs([this](double width) {
            require_finite(width);
            call_handler([this, width] { handlers_.set_stereo_width(width); });
        }),
        sdbus::registerMethod("SetProfile").implementedAs([this](const std::string& profile) {
            auto result = call_handler([this, &profile] { return handlers_.set_profile(profile); });
            if (!result) throw sdbus::Error(sdbus::Error::Name{kProfileFailedError}, result.error());
        }),
        sdbus::registerMethod("ListProfiles").implementedAs([this] {
            return call_handler([this] { return handlers_.list_profiles(); });
        }),
        sdbus::registerMethod("ListProfilesDetailed").implementedAs([this] {
            return call_handler([this] {
                std::vector<sdbus::Struct<std::string, std::string>> out;
                if (handlers_.list_profiles_detailed)
                    for (auto& [id, name] : handlers_.list_profiles_detailed()) out.emplace_back(id, name);
                return out;
            });
        }),
        sdbus::registerMethod("GetState").implementedAs([this] {
            return call_handler([this] {
                const ControlState s = handlers_.get_state();
                return std::make_tuple(s.enabled, s.volume, s.stereo_width, s.profile);
            });
        }),
        sdbus::registerMethod("GetTone").implementedAs([this] {
            return call_handler([this] {
                const auto [x, y] = handlers_.get_tone ? handlers_.get_tone() : std::pair<double, double>{0.0, 0.0};
                return std::make_tuple(x, y);
            });
        }),
        sdbus::registerMethod("SetTone").implementedAs([this](double x, double y) {
            require_finite(x);
            require_finite(y);
            call_handler([this, x, y] {
                if (handlers_.set_tone) handlers_.set_tone(x, y);
            });
        }),
        sdbus::registerMethod("GetInfo").implementedAs([this] {
            return call_handler([this] {
                const bool connected = handlers_.device_connected ? handlers_.device_connected() : true;
                return std::make_tuple(std::string(kVersion), connected);
            });
        }),
        sdbus::registerMethod("GetVisualizer").implementedAs([this] {
            return call_handler([this] {
                const VisualizerState s =
                    handlers_.get_visualizer
                        ? handlers_.get_visualizer()
                        : VisualizerState{false, viz::kDefaultPosition, viz::kDefaultDismissMs, viz::kDefaultFollowSpeed};
                return std::make_tuple(s.enabled, std::string(viz::to_string(s.position)),
                                        static_cast<uint32_t>(s.dismiss_ms), s.follow_speed);
            });
        }),
        sdbus::registerMethod("SetVisualizer").implementedAs([this](bool enabled) {
            call_handler([this, enabled] {
                if (handlers_.set_visualizer) handlers_.set_visualizer(enabled);
            });
        }),
        sdbus::registerMethod("SetVisualizerPosition").implementedAs([this](const std::string& position) {
            const auto parsed = viz::position_from_string(position);
            if (!parsed) {
                throw sdbus::Error(sdbus::Error::Name{kInvalidArgumentError},
                                    "unknown visualizer position: " + position);
            }
            call_handler([this, p = *parsed] {
                if (handlers_.set_visualizer_position) handlers_.set_visualizer_position(p);
            });
        }),
        sdbus::registerMethod("SetVisualizerDismiss").implementedAs([this](uint32_t ms) {
            call_handler([this, ms] {
                if (handlers_.set_visualizer_dismiss) handlers_.set_visualizer_dismiss(ms);
            });
        }),
        sdbus::registerMethod("SetVisualizerFollowSpeed").implementedAs([this](double speed) {
            require_finite(speed);
            call_handler([this, speed] {
                if (handlers_.set_visualizer_follow_speed) handlers_.set_visualizer_follow_speed(speed);
            });
        }),
        sdbus::registerSignal("StateChanged"),
        sdbus::registerMethod("Quit").implementedAs([this] { call_handler([this] { handlers_.quit(); }); }))
        .forInterface(kInterface);
}

ControlService::~ControlService() = default;

void ControlService::notify_state_changed() noexcept {
    if (!object_) return;
    try {
        object_->emitSignal("StateChanged").onInterface(kInterface);
    } catch (...) {
        // Best-effort UI refresh only, mirrors TrayService::notify_changed().
    }
}

bool acquire_instance_name(sdbus::IConnection& connection, const std::string& name) {
    try {
        connection.requestName(sdbus::ServiceName{name});
        return true;
    } catch (const sdbus::Error& e) {
        if (e.getName() == kNameExistsError) return false;
        throw;
    }
}

int run_ctl(const std::vector<std::string>& args, const std::string& bus_name) {
    if (args.empty()) {
        print_usage(stderr);
        return 64;
    }
    const std::string& cmd = args[0];
    if (cmd == "help") {
        print_usage(stdout);
        return 0;
    }

    try {
        // Explicit session-bus connection: the connection-less overload
        // opens whatever sd_bus_open's heuristic picks (the system bus for
        // root or other non-session contexts), which is never what a user
        // CLI talking to their own KEEBY instance wants.
        auto connection = sdbus::createSessionBusConnection();
        auto proxy = sdbus::createLightWeightProxy(std::move(connection), sdbus::ServiceName{bus_name},
                                                    sdbus::ObjectPath{kObjectPath});

        if (cmd == "toggle" && args.size() == 1) {
            bool enabled{};
            proxy->callMethod("Toggle").onInterface(kInterface).withTimeout(kCtlTimeout).storeResultsTo(enabled);
            std::printf("enabled=%s\n", enabled ? "true" : "false");
        } else if ((cmd == "on" || cmd == "off") && args.size() == 1) {
            const bool enable = cmd == "on";
            proxy->callMethod("SetEnabled").onInterface(kInterface).withTimeout(kCtlTimeout).withArguments(enable);
            std::printf("enabled=%s\n", enable ? "true" : "false");
        } else if (cmd == "volume" && args.size() == 2) {
            const auto pct = parse_percent(args[1]);
            if (!pct) {
                print_usage(stderr);
                return 64;
            }
            const double raw = pct->relative ? fetch_state(*proxy).volume + pct->points / 100.0 : pct->points / 100.0;
            proxy->callMethod("SetVolume").onInterface(kInterface).withTimeout(kCtlTimeout).withArguments(raw);
            std::printf("volume=%ld\n", std::lround(fetch_state(*proxy).volume * 100.0));
        } else if (cmd == "width" && args.size() == 2) {
            const auto pct = parse_percent(args[1]);
            if (!pct) {
                print_usage(stderr);
                return 64;
            }
            const double raw = pct->points / 100.0;
            proxy->callMethod("SetStereoWidth").onInterface(kInterface).withTimeout(kCtlTimeout).withArguments(raw);
            std::printf("width=%ld\n", std::lround(fetch_state(*proxy).stereo_width * 100.0));
        } else if (cmd == "tone" && args.size() == 3) {
            const auto px = parse_percent(args[1]);
            const auto py = parse_percent(args[2]);
            if (!px || !py) {
                print_usage(stderr);
                return 64;
            }
            proxy->callMethod("SetTone")
                .onInterface(kInterface)
                .withTimeout(kCtlTimeout)
                .withArguments(px->points / 100.0, py->points / 100.0);
            const auto tone = fetch_tone(*proxy);
            std::printf("tone=%ld,%ld\n", std::lround(tone.first * 100.0), std::lround(tone.second * 100.0));
        } else if (cmd == "profile" && args.size() == 2) {
            proxy->callMethod("SetProfile").onInterface(kInterface).withTimeout(kCtlTimeout).withArguments(args[1]);
            std::printf("profile=%s\n", args[1].c_str());
        } else if (cmd == "profiles" && args.size() == 1) {
            std::vector<std::string> profiles;
            proxy->callMethod("ListProfiles")
                .onInterface(kInterface)
                .withTimeout(kCtlTimeout)
                .storeResultsTo(profiles);
            for (const auto& p : profiles) std::printf("%s\n", p.c_str());
        } else if (cmd == "visualizer" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
            const bool enable = args[1] == "on";
            proxy->callMethod("SetVisualizer").onInterface(kInterface).withTimeout(kCtlTimeout).withArguments(enable);
            std::printf("visualizer=%s\n", enable ? "on" : "off");
        } else if (cmd == "visualizer" && args.size() == 3 && args[1] == "position") {
            proxy->callMethod("SetVisualizerPosition")
                .onInterface(kInterface)
                .withTimeout(kCtlTimeout)
                .withArguments(args[2]);
            std::printf("visualizer_position=%s\n", args[2].c_str());
        } else if (cmd == "visualizer" && args.size() == 3 && args[1] == "dismiss") {
            char* end = nullptr;
            const long v = std::strtol(args[2].c_str(), &end, 10);
            if (end != args[2].c_str() + args[2].size() || v < 0) {
                print_usage(stderr);
                return 64;
            }
            proxy->callMethod("SetVisualizerDismiss")
                .onInterface(kInterface)
                .withTimeout(kCtlTimeout)
                .withArguments(static_cast<uint32_t>(v));
            std::printf("visualizer_dismiss_ms=%ld\n", v);
        } else if (cmd == "visualizer" && args.size() == 3 && args[1] == "speed") {
            char* end = nullptr;
            const double v = std::strtod(args[2].c_str(), &end);
            if (end != args[2].c_str() + args[2].size() || !std::isfinite(v)) {
                print_usage(stderr);
                return 64;
            }
            proxy->callMethod("SetVisualizerFollowSpeed")
                .onInterface(kInterface)
                .withTimeout(kCtlTimeout)
                .withArguments(v);
            std::printf("visualizer_follow_speed=%.1f\n", fetch_visualizer(*proxy).follow_speed);
        } else if (cmd == "status" && args.size() == 1) {
            print_state(fetch_state(*proxy), fetch_tone(*proxy), fetch_visualizer(*proxy));
        } else if (cmd == "quit" && args.size() == 1) {
            proxy->callMethod("Quit").onInterface(kInterface).withTimeout(kCtlTimeout);
        } else {
            print_usage(stderr);
            return 64;
        }
    } catch (const sdbus::Error& e) {
        if (e.getName() == kServiceUnknownError) {
            std::fprintf(stderr, "keeby: not running\n");
            return 2;
        }
        std::fprintf(stderr, "keeby: %s\n", e.getMessage().empty() ? e.what() : e.getMessage().c_str());
        return 1;
    }
    return 0;
}

} // namespace keeby
