#include "engine_controller.hpp"

#include <cstdio>
#include <utility>
#include <vector>

#include "paths.hpp"

namespace keeby {

EngineController::EngineController(SoundBank samples, MixerVariation variation, std::string profile_id)
    : audio_(std::move(samples), variation), profile_(std::move(profile_id)) {}

EngineController::~EngineController() { stop(); }

std::expected<void, std::string> EngineController::start(std::string device_path) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (auto res = audio_.start(transport_); !res) return res;
    if (auto res = capture_.start(std::move(device_path), transport_); !res) {
        audio_.stop(); // roll back the half-started engine
        return res;
    }
    return {};
}

void EngineController::stop() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    capture_.set_visualizer_transport(nullptr);
    capture_.set_visualizer_motion_transport(nullptr);
    visualizer_.set_enabled(false);
    capture_.stop();
    audio_.stop();
}

void EngineController::set_visualizer_enabled(bool enabled) {
    if (enabled) capture_.set_visualizer_transport(&visualizer_.transport());
    visualizer_.set_enabled(enabled);
    if (!enabled) capture_.set_visualizer_transport(nullptr);
    update_pointer_motion_tap();
}

void EngineController::set_visualizer_position(viz::Position position) {
    visualizer_.set_position(position);
    update_pointer_motion_tap();
}

void EngineController::update_pointer_motion_tap() {
    const bool should_tap = visualizer_.enabled() && visualizer_.position() == viz::Position::FollowCursor;
    capture_.set_visualizer_motion_transport(should_tap ? &visualizer_.motion_transport() : nullptr);
}

std::string EngineController::profile() const {
    std::lock_guard<std::mutex> lock(profile_mutex_);
    return profile_;
}

std::expected<void, std::string> EngineController::set_profile(std::string_view id) {
    std::string id_str(id);
    // Load and validate fully off-RT, and outside profile_mutex_, so a slow
    // pack decode never blocks a concurrent profile() read (e.g. the tray's
    // D-Bus thread building its title) or another set_profile() call.
    std::vector<std::string> warnings;
    auto bank = [&]() -> std::expected<SoundBank, std::string> {
        if (id_str == "default") return load_default_bank(paths::asset_dir());
        for (const auto& pack : list_packs())
            if (pack.id == id_str) return load_pack(pack.dir, &warnings);
        return std::unexpected("unknown profile: " + id_str);
    }();
    for (const auto& w : warnings) std::fprintf(stderr, "keeby: %s\n", w.c_str());
    if (!bank) return std::unexpected(bank.error());

    {
        // Shares lifecycle_mutex_ with start()/stop(): swap_bank() reads
        // AudioBoundary's started_ and mutates retired_, neither of which
        // is safe to touch concurrently with a start()/stop() call.
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        audio_.swap_bank(std::make_unique<const SoundBank>(std::move(*bank)));
    }
    // Separate, always-briefly-held mutex: never makes a profile() caller
    // (e.g. the tray's D-Bus thread) wait behind the swap's RT-ack wait above.
    std::lock_guard<std::mutex> lock(profile_mutex_);
    profile_ = std::move(id_str);
    return {};
}

} // namespace keeby
