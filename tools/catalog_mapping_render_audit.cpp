#include "sample_mixer.hpp"
#include "sound_pack.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <set>
#include <tuple>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace keeby;

namespace {
struct Metrics {
    uint64_t frames = 0;
    uint64_t nonzero_frames = 0;
    double square_sum = 0.0;
    double peak = 0.0;
};

constexpr std::array<uint16_t, 25> kExpectedKeys{{
    KEY_A, KEY_SPACE, KEY_ENTER, KEY_BACKSPACE, KEY_KPENTER, KEY_TAB, KEY_ESC,
    KEY_CAPSLOCK, KEY_UP, KEY_LEFT, KEY_RIGHT, KEY_DOWN, KEY_LEFTSHIFT,
    KEY_LEFTCTRL, KEY_LEFTALT, KEY_LEFTMETA, KEY_1, KEY_5, KEY_0, KEY_F1,
    KEY_F12, KEY_FN, BTN_LEFT, BTN_RIGHT, BTN_MIDDLE,
}};

bool profile_of_interest(const fs::path& path) {
    const auto id = path.filename().string();
    return id.starts_with("keeby-native-") || id == "keeby-web-keychron-k2-max-brown";
}

Metrics render_one(const SoundBank& source_bank, uint16_t code, KeyEventKind kind,
                   uint16_t sample_index, bool& started) {
    SoundBank one;
    one.samples.push_back(source_bank.samples[sample_index]);
    one.playback_policy = source_bank.playback_policy;
    one.press_gain = source_bank.press_gain;
    one.release_gain = source_bank.release_gain;
    one.pan[code] = source_bank.pan[code];
    if (kind == KeyEventKind::Down) one.press[code] = KeySound{0, 1};
    else one.release[code] = KeySound{0, 1};

    SampleMixer mixer(std::move(one));
    mixer.set_master_gain(1.0f);
    mixer.set_stereo_width(0.0f);
    started = mixer.handle_event({code, kind, 0}) == TriggerResult::Started;

    Metrics metrics;
    if (!started) return metrics;
    const uint32_t frames = static_cast<uint32_t>(source_bank.samples[sample_index].pcm.size() / kNativeChannels);
    std::array<float, static_cast<size_t>(kMaxFramesPerCallback) * kNativeChannels> output{};
    uint32_t left = frames;
    while (left != 0) {
        const uint32_t count = std::min<uint32_t>(left, kMaxFramesPerCallback);
        mixer.mix(output.data(), count);
        for (uint32_t f = 0; f < count; ++f) {
            const double l = output[static_cast<size_t>(f) * kNativeChannels];
            const double r = output[static_cast<size_t>(f) * kNativeChannels + 1];
            const double frame_peak = std::max(std::abs(l), std::abs(r));
            metrics.peak = std::max(metrics.peak, frame_peak);
            metrics.square_sum += l * l + r * r;
            metrics.frames++;
            if (frame_peak > 0.0) metrics.nonzero_frames++;
        }
        left -= count;
    }
    return metrics;
}

bool pcm_is_valid_and_nonzero(const Sample& sample, double& peak, double& rms) {
    if (sample.pcm.empty() || sample.pcm.size() % kNativeChannels != 0) return false;
    double squares = 0.0;
    peak = 0.0;
    for (float value : sample.pcm) {
        if (!std::isfinite(value)) return false;
        const double x = value;
        peak = std::max(peak, std::abs(x));
        squares += x * x;
    }
    rms = std::sqrt(squares / sample.pcm.size());
    return peak > 0.0;
}

void report_empty(const std::string& profile, uint16_t code, const char* direction) {
    std::cout << "EMPTY_MAPPING," << profile << ',' << code << ',' << direction << ",,,,,,,,,\n";
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: catalog_mapping_render_audit PACKS_DIR\n";
        return 64;
    }
    const fs::path packs_dir(argv[1]);
    if (!fs::is_directory(packs_dir)) {
        std::cerr << "PACKS_DIR is not a directory\n";
        return 64;
    }

    std::vector<fs::path> profiles;
    for (const auto& entry : fs::directory_iterator(packs_dir))
        if (entry.is_directory() && profile_of_interest(entry.path())) profiles.push_back(entry.path());
    std::sort(profiles.begin(), profiles.end());

    uint64_t profiles_loaded = 0;
    uint64_t mapped_directions = 0;
    uint64_t variant_references = 0;
    uint64_t rendered_variants = 0;
    uint64_t empty_mappings = 0;
    uint64_t failures = 0;
    uint64_t unique_mapped_samples = 0;

    std::cout << "type,profile,key_code,direction,variant_slot,sample_index,source_frames,source_rms,source_peak,render_frames,render_nonzero_frames,render_rms,render_peak\n";
    for (const auto& profile_path : profiles) {
        const std::string profile = profile_path.filename().string();
        std::vector<std::string> warnings;
        auto loaded = load_pack(profile_path, &warnings);
        if (!loaded) {
            std::cout << "ERROR," << profile << ",,,,,,,,,,,,load:" << loaded.error() << "\n";
            ++failures;
            continue;
        }
        ++profiles_loaded;
        const SoundBank& bank = *loaded;
        std::vector<bool> mapped_samples(bank.samples.size(), false);

        for (uint16_t code = 1; code < kKeyCodeCount; ++code) {
            if (!is_supported_input_code(code)) continue;
            const KeySound& down = bank.press[code];
            const KeySound& up = bank.release[code];
            const bool representative = std::find(kExpectedKeys.begin(), kExpectedKeys.end(), code) != kExpectedKeys.end();
            if (down.count == 0 && (representative || up.count != 0)) {
                report_empty(profile, code, "down");
                ++empty_mappings;
            }
            if (up.count == 0 && (representative || down.count != 0)) {
                report_empty(profile, code, "up");
                ++empty_mappings;
            }

            const std::array<std::tuple<const char*, KeyEventKind, KeySound>, 2> directions{{
                {"down", KeyEventKind::Down, down}, {"up", KeyEventKind::Up, up}}};
            for (const auto& [direction, kind, sound] : directions) {
                if (sound.count == 0) continue;
                ++mapped_directions;
                if (static_cast<size_t>(sound.first) + sound.count > bank.samples.size()) {
                    std::cout << "ERROR," << profile << ',' << code << ',' << direction << ",,,,,,,,,range-out-of-bounds\n";
                    ++failures;
                    continue;
                }
                for (uint8_t variant = 0; variant < sound.count; ++variant) {
                    ++variant_references;
                    const uint16_t sample_index = static_cast<uint16_t>(sound.first + variant);
                    mapped_samples[sample_index] = true;
                    const Sample& sample = bank.samples[sample_index];
                    double source_peak = 0.0, source_rms = 0.0;
                    const bool source_ok = pcm_is_valid_and_nonzero(sample, source_peak, source_rms);
                    bool started = false;
                    const Metrics rendered = render_one(bank, code, kind, sample_index, started);
                    const double render_rms = rendered.frames
                        ? std::sqrt(rendered.square_sum / (rendered.frames * kNativeChannels)) : 0.0;
                    const bool render_ok = started && rendered.frames == sample.pcm.size() / kNativeChannels &&
                                           rendered.nonzero_frames > 0 && std::isfinite(render_rms) &&
                                           std::isfinite(rendered.peak);
                    std::cout << (source_ok && render_ok ? "MAPPED" : "ERROR") << ',' << profile << ',' << code << ','
                              << direction << ',' << static_cast<unsigned>(variant) << ',' << sample_index << ','
                              << sample.pcm.size() / kNativeChannels << ',' << std::setprecision(9) << source_rms << ','
                              << source_peak << ',' << rendered.frames << ',' << rendered.nonzero_frames << ','
                              << render_rms << ',' << rendered.peak << '\n';
                    ++rendered_variants;
                    if (!source_ok || !render_ok) ++failures;
                }
            }
        }
        unique_mapped_samples += std::count(mapped_samples.begin(), mapped_samples.end(), true);
    }

    std::cerr << "profiles_loaded=" << profiles_loaded
              << " profiles_found=" << profiles.size()
              << " mapped_key_directions=" << mapped_directions
              << " mapped_variant_references=" << variant_references
              << " rendered_variants=" << rendered_variants
              << " unique_mapped_samples=" << unique_mapped_samples
              << " empty_mappings_in_audit_scope=" << empty_mappings
              << " failures=" << failures << '\n';
    if (profiles.size() != 26) return 2;
    return failures == 0 ? 0 : 1;
}
