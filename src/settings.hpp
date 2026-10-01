#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace keeby {

// KEEBY's persisted user-facing state (mirrors EngineController's control
// surface: enabled, master gain, stereo width, active sound profile, and
// the on-screen keyboard visualizer -- see docs/013-visualizer.md).
struct Settings {
    bool enabled = true;
    float volume = 1.0f;        // [0, 1]
    float stereo_width = 1.0f;  // [0, 2]
    std::string profile = "default";
    float tone_x = 0.0f;  // [-1, 1]; Thock (-1) .. Clack (+1)
    float tone_y = 0.0f;  // [-1, 1]; Warm (-1) .. Bright (+1)
    bool visualizer = true;
    // One of viz::kPositionStrings; load_settings() validates this against
    // that set (falls back to the default + warns on anything else).
    std::string visualizer_position = "follow-cursor";
    uint16_t visualizer_dismiss_ms = 1000;  // [250, 5000]
    float visualizer_follow_speed = 1.0f;   // [0.1, 4.0]; follow-cursor speed multiplier
};

// $XDG_CONFIG_HOME/keeby/settings.ini, else ~/.config/keeby/settings.ini.
std::filesystem::path default_settings_path();

// Never throws. A missing file yields defaults with no warning; any other
// per-key problem (unknown key, malformed line, out-of-range or non-finite
// number, invalid profile) keeps that field at its default/clamps it and
// appends a human-readable warning instead of failing the whole load.
Settings load_settings(const std::filesystem::path& path, std::vector<std::string>* warnings = nullptr);

// Atomically replaces `path` with `settings` serialized as `key = value`
// lines (create parent directory, mode 0700, if missing; write via a temp
// file in the same directory, fsync, then rename over the target; file
// mode 0600).
std::expected<void, std::string> save_settings(const std::filesystem::path& path, const Settings& settings);

} // namespace keeby
