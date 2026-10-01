#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "key_event.hpp"
#include "sample_mixer.hpp"
#include "sound_pack.hpp"

namespace fs = std::filesystem;

struct ProbeKey { const char* name; uint16_t code; };
constexpr std::array<ProbeKey, 25> kKeys{{
    {"A", KEY_A}, {"Space", KEY_SPACE}, {"Enter", KEY_ENTER}, {"Backspace", KEY_BACKSPACE},
    {"KeypadEnter", KEY_KPENTER}, {"Tab", KEY_TAB}, {"Escape", KEY_ESC}, {"CapsLock", KEY_CAPSLOCK},
    {"Up", KEY_UP}, {"Left", KEY_LEFT}, {"Right", KEY_RIGHT}, {"Down", KEY_DOWN},
    {"Shift", KEY_LEFTSHIFT}, {"Ctrl", KEY_LEFTCTRL}, {"Alt", KEY_LEFTALT}, {"Meta", KEY_LEFTMETA},
    {"Digit1", KEY_1}, {"Digit5", KEY_5}, {"Digit0", KEY_0}, {"F1", KEY_F1}, {"F12", KEY_F12},
    {"Fn", KEY_FN},
    {"BtnLeft", BTN_LEFT}, {"BtnRight", BTN_RIGHT}, {"BtnMiddle", BTN_MIDDLE},
}};

struct Metrics {
    uint64_t samples = 0;
    uint64_t nonzero_frames = 0;
    uint64_t clipped_frames = 0;
    double square_sum = 0.0;
    double peak = 0.0;
};

Metrics render(const keeby::SoundBank& bank, uint16_t code, keeby::KeyEventKind kind,
               bool actual_settings) {
    keeby::SampleMixer mixer(bank, keeby::MixerVariation{0x4b454542u, 0.5f, 0.5f});
    mixer.set_master_gain(1.0f);
    mixer.set_stereo_width(actual_settings ? 2.0f : 0.0f);
    if (actual_settings) mixer.set_tone(-1.0f, -1.0f);

    auto render_event = [&](keeby::KeyEventKind event_kind) {
        const auto& sound = event_kind == keeby::KeyEventKind::Down ? bank.press[code] : bank.release[code];
        uint32_t frames = 0;
        for (uint8_t i = 0; i < sound.count; ++i)
            frames = std::max(frames, static_cast<uint32_t>(bank.samples[sound.first + i].pcm.size() / 2));
        Metrics m;
        if (mixer.handle_event({code, event_kind, 0}) != keeby::TriggerResult::Started || frames == 0) return m;
        std::array<float, keeby::kMaxFramesPerCallback * 2> block{};
        uint32_t left = frames;
        while (left != 0) {
            const uint32_t n = std::min<uint32_t>(left, keeby::kMaxFramesPerCallback);
            mixer.mix(block.data(), n);
            for (uint32_t f = 0; f < n; ++f) {
                const double l = block[2 * f];
                const double r = block[2 * f + 1];
                const double p = std::max(std::abs(l), std::abs(r));
                m.peak = std::max(m.peak, p);
                m.square_sum += l * l + r * r;
                m.samples += 2;
                if (p > 0.0) ++m.nonzero_frames;
                if (p >= static_cast<double>(keeby::kOutputPeak) - 1e-7) ++m.clipped_frames;
            }
            left -= n;
        }
        return m;
    };

    if (kind == keeby::KeyEventKind::Up) (void)render_event(keeby::KeyEventKind::Down);
    return render_event(kind);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: sound_fidelity_probe PACKS_DIR\n";
        return 64;
    }
    const fs::path packs_dir(argv[1]);
    std::cout << "profile,key,direction,scenario,mapped_variants,first_variant_frames,render_window_frames,nonzero_frames,rms,peak,clipped_frames\n";
    bool load_failed = false;
    std::size_t profiles = 0;
    for (const auto& entry : fs::directory_iterator(packs_dir)) {
        if (!entry.is_directory()) continue;
        const std::string id = entry.path().filename().string();
        if (id.rfind("keeby-native-", 0) != 0 && id != "keeby-web-keychron-k2-max-brown" && id != "turquoise") continue;
        ++profiles;
        std::vector<std::string> warnings;
        auto loaded = keeby::load_pack(entry.path(), &warnings);
        if (!loaded) {
            std::cerr << "LOAD_ERROR," << id << "," << loaded.error() << "\n";
            load_failed = true;
            continue;
        }
        const auto& bank = *loaded;
        for (const auto& key : kKeys) {
            for (const auto kind : {keeby::KeyEventKind::Down, keeby::KeyEventKind::Up}) {
                const auto& sound = kind == keeby::KeyEventKind::Down ? bank.press[key.code] : bank.release[key.code];
                const uint32_t first_variant_frames = sound.count
                    ? static_cast<uint32_t>(bank.samples[sound.first].pcm.size() / 2) : 0;
                uint32_t render_window_frames = 0;
                for (uint8_t i = 0; i < sound.count; ++i)
                    render_window_frames = std::max(render_window_frames,
                        static_cast<uint32_t>(bank.samples[sound.first + i].pcm.size() / 2));
                for (const bool actual : {false, true}) {
                    const Metrics m = render(bank, key.code, kind, actual);
                    const double rms = m.samples ? std::sqrt(m.square_sum / m.samples) : 0.0;
                    std::cout << id << ',' << key.name << ','
                              << (kind == keeby::KeyEventKind::Down ? "down" : "up") << ','
                              << (actual ? "volume1_width2_tone-1-1" : "neutral_center_volume1") << ','
                              << static_cast<unsigned>(sound.count) << ',' << first_variant_frames << ','
                              << render_window_frames << ','
                              << m.nonzero_frames << ',' << rms << ',' << m.peak << ',' << m.clipped_frames << '\n';
                }
            }
        }
    }
    std::cerr << "profiles=" << profiles << " load_failures=" << (load_failed ? "yes" : "no") << "\n";
    return profiles == 27 && !load_failed ? 0 : 2;
}
