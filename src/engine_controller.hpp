#pragma once

#include <expected>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "audio_boundary.hpp"
#include "event_transport.hpp"
#include "input_capture.hpp"
#include "sound_pack.hpp"
#include "visualizer_service.hpp"
#include "visualizer_wire.hpp"

namespace keeby {

// Owns the engine's runtime pieces (transport, audio boundary, input
// capture) and is the only thing a desktop shell talks to: it never hands
// out PipeWire or libevdev internals. Control calls (enable/disable,
// master gain) are thin forwards to AudioBoundary's atomics — cheap,
// RT-safe, callable from any thread (e.g. a tray's D-Bus dispatch thread).
class EngineController {
public:
    // `profile_id` names the profile `samples` was already loaded from
    // (default "default"); it is reporting only, EngineController does not
    // re-derive or validate it against `samples`.
    explicit EngineController(SoundBank samples, MixerVariation variation = {},
                               std::string profile_id = "default");
    ~EngineController();
    EngineController(const EngineController&) = delete;
    EngineController& operator=(const EngineController&) = delete;

    // start/stop are serialized by the owning thread, same contract as
    // AudioBoundary/InputCapture. On failure, whatever partially started
    // is rolled back before returning, so a failed start() leaves nothing
    // running.
    std::expected<void, std::string> start(std::string device_path);
    void stop();

    void set_enabled(bool enabled) noexcept { audio_.set_enabled(enabled); }
    bool enabled() const noexcept { return audio_.enabled(); }
    void set_master_gain(float gain) noexcept { audio_.set_master_gain(gain); }
    float master_gain() const noexcept { return audio_.master_gain(); }
    void set_stereo_width(float width) noexcept { audio_.set_stereo_width(width); }
    float stereo_width() const noexcept { return audio_.stereo_width(); }
    void set_tone(float x, float y) { audio_.set_tone(x, y); } // not noexcept: see AudioBoundary::set_tone
    std::pair<float, float> tone() const noexcept { return audio_.tone(); }

    // Step 2.7: real pack discovery/switching (see sound_pack.hpp) replaces
    // the Step 2.6 single-profile stub. available_profiles() decodes no
    // audio (id/name only). set_profile() loads and validates the new bank
    // fully off-RT, then swaps it in -- see docs/008-step-2.7-sound-packs.md
    // for the RT-safe handshake. profile() is mutex-guarded: it's read from
    // the tray's D-Bus thread while set_profile() may be running concurrently
    // on another thread.
    std::vector<PackInfo> available_profiles() const { return list_packs(); }
    std::string profile() const;
    std::expected<void, std::string> set_profile(std::string_view id);

    const AudioBoundaryCounters& counters() const { return audio_.counters(); }
    uint64_t transport_dropped_count() const noexcept { return transport_.dropped_count(); }
    // False once the keyboard device has disappeared out from under a
    // running capture thread (distinct from "never started"/"stopped").
    bool device_connected() const noexcept { return capture_.connected(); }

    // Floating on-screen keyboard visualizer (docs/013-visualizer.md).
    // set_visualizer_enabled additionally wires/unwires capture_'s second
    // tap transport so InputCapture only ever pushes into it while the
    // visualizer is actually enabled.
    void set_visualizer_enabled(bool enabled);
    bool visualizer_enabled() const noexcept { return visualizer_.enabled(); }
    // Also re-evaluates the pointer-motion tap below, since whether it
    // should be wired depends on both enabled() and position().
    void set_visualizer_position(viz::Position position);
    viz::Position visualizer_position() const noexcept { return visualizer_.position(); }
    void set_visualizer_dismiss_ms(uint32_t ms) { visualizer_.set_dismiss_ms(ms); }
    uint16_t visualizer_dismiss_ms() const noexcept { return visualizer_.dismiss_ms(); }
    void set_visualizer_follow_speed(double multiplier) { visualizer_.set_follow_speed(multiplier); }
    double visualizer_follow_speed() const noexcept { return visualizer_.follow_speed(); }

private:
    // Wires/unwires capture_'s pointer-motion tap to exactly reflect
    // "enabled AND position == FollowCursor" (docs/006's "Pointer motion
    // for the visualizer" section: motion goes to the visualizer only, and
    // only when there's a reason to render it). Called after every change
    // to either input.
    void update_pointer_motion_tap();

    KeyEventTransport transport_;
    AudioBoundary audio_;
    InputCapture capture_;
    VisualizerService visualizer_;
    // Serializes start(), stop() (incl. from the destructor), and the
    // swap_bank() section of set_profile against each other: all three
    // touch AudioBoundary's started_/retired_, which have no locking of
    // their own (AudioBoundary documents an "owner serializes" contract).
    // Deliberately separate from profile_mutex_ below so a tray/D-Bus
    // profile() read is never stuck behind a swap's up-to-500ms RT-ack
    // wait, or behind a stop()/start() cycle.
    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex profile_mutex_; // guards profile_ only; always briefly held
    std::string profile_;
};

} // namespace keeby
