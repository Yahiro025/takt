#include "sound_pack.hpp"

#include <linux/input-event-codes.h>
#include <sndfile.h>
#include <samplerate.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace keeby;
namespace fs = std::filesystem;

namespace {
fs::path make_temp_dir() {
    std::string pattern = "/tmp/keeby-native-policy-XXXXXX";
    std::vector<char> buf(pattern.begin(), pattern.end());
    buf.push_back('\0');
    assert(mkdtemp(buf.data()) != nullptr);
    return fs::path(buf.data()) / "keeby-native-akko-v3-pro-cream-yellow";
}

void write_text(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << body;
    assert(out.good());
}

void write_constant_audio(const fs::path& path, int rate, int frames, float amplitude, int format = SF_FORMAT_WAV | SF_FORMAT_FLOAT) {
    fs::create_directories(path.parent_path());
    SF_INFO info{};
    info.samplerate = rate;
    info.channels = 2;
    info.format = format;
    SNDFILE* file = sf_open(path.c_str(), SFM_WRITE, &info);
    assert(file);
    std::vector<float> pcm(static_cast<size_t>(frames) * 2);
    for (int frame = 0; frame < frames; ++frame) {
        const float sample = amplitude * std::sin(2.0f * 3.14159265f * 440.0f * frame / rate);
        pcm[2 * frame] = pcm[2 * frame + 1] = sample;
    }
    assert(sf_writef_float(file, pcm.data(), frames) == frames);
    assert(sf_close(file) == 0);
}

void write_wav_constant(const fs::path& path, int rate, int frames, float amplitude) {
    fs::create_directories(path.parent_path());
    SF_INFO info{};
    info.samplerate = rate;
    info.channels = 2;
    info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
    SNDFILE* file = sf_open(path.c_str(), SFM_WRITE, &info);
    assert(file);
    std::vector<float> pcm(static_cast<size_t>(frames) * 2, amplitude);
    assert(sf_writef_float(file, pcm.data(), frames) == frames);
    assert(sf_close(file) == 0);
}

void write_transient_wav(const fs::path& path) {
    constexpr int rate = 44100;
    constexpr int frames = rate / 10;
    fs::create_directories(path.parent_path());
    SF_INFO info{};
    info.samplerate = rate;
    info.channels = 2;
    info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
    SNDFILE* file = sf_open(path.c_str(), SFM_WRITE, &info);
    assert(file);
    std::vector<float> pcm(static_cast<size_t>(frames) * 2, 0.0f);
    const int first = rate / 2 / 10;
    for (int frame = first; frame < first + rate / 100; ++frame)
        pcm[2 * frame] = pcm[2 * frame + 1] = 1.25f;
    assert(sf_writef_float(file, pcm.data(), frames) == frames);
    assert(sf_close(file) == 0);
}

std::vector<float> read_resampled_sprite(const fs::path& path, uint32_t& frames) {
    SF_INFO info{};
    SNDFILE* file = sf_open(path.c_str(), SFM_READ, &info);
    assert(file && info.channels == 2);
    std::vector<float> input(static_cast<size_t>(info.frames) * 2);
    assert(sf_readf_float(file, input.data(), info.frames) == info.frames);
    assert(sf_close(file) == 0);
    SRC_DATA src{};
    src.data_in = input.data();
    src.input_frames = info.frames;
    src.src_ratio = static_cast<double>(kNativeSampleRate) / info.samplerate;
    src.end_of_input = 1;
    const long capacity = static_cast<long>(std::ceil(info.frames * src.src_ratio)) + 16;
    std::vector<float> output(static_cast<size_t>(capacity) * 2);
    src.data_out = output.data();
    src.output_frames = capacity;
    assert(src_simple(&src, SRC_SINC_MEDIUM_QUALITY, 2) == 0);
    frames = static_cast<uint32_t>(src.output_frames_gen);
    output.resize(static_cast<size_t>(frames) * 2);
    return output;
}

double peak(const std::vector<float>& pcm) {
    double out = 0.0;
    for (float v : pcm) out = std::max(out, std::abs(static_cast<double>(v)));
    return out;
}

float first_sample(const SoundBank& bank, float volume) {
    SampleMixer mixer(bank, MixerVariation{0x4b454542u, 0.5f, 0.5f});
    mixer.set_master_gain(volume);
    mixer.set_stereo_width(0.0f);
    assert(mixer.handle_event({KEY_A, KeyEventKind::Down, 0}) == TriggerResult::Started);
    float out[2]{};
    mixer.mix(out, 1);
    return out[0];
}
} // namespace

int main() {
    const fs::path dir = make_temp_dir();
    const fs::path temp_root = dir.parent_path();
    fs::create_directories(dir);
    write_wav_constant(dir / "alpha_down_1.wav", 48000, 4800, 0.01f);
    write_wav_constant(dir / "alpha_down_2.wav", 48000, 4800, 0.02f);
    write_wav_constant(dir / "alpha_up.wav", 48000, 4800, 0.01f);
    write_wav_constant(dir / "mouse_down.wav", 48000, 4800, 0.5f);
    write_wav_constant(dir / "mouse_up.wav", 48000, 4800, 0.5f);
    write_wav_constant(dir / "unity.wav", 48000, 4800, 0.319f);
    write_transient_wav(dir / "transient.wav");
    write_text(dir / "config.json", R"({
        "version": 2,
        "id": "keeby-native-akko-v3-pro-cream-yellow",
        "name": "fixture",
        "key_define_type": "multi",
        "sound": "alpha_down_{1-2}.wav",
        "soundup": "alpha_up.wav",
        "defines": { "16": "transient.wav", "17": "unity.wav",
                     "272": "mouse_down.wav", "272-up": "mouse_up.wav" }
    })");

    auto loaded = load_pack(dir);
    assert(loaded);
    const auto& bank = *loaded;
    const auto& sample = bank.samples[bank.press[KEY_A].first].pcm;

    // Independent F7 B7 reference: max sliding 10ms RMS is 0.01, so the
    // per-clip target gain is clamp(0.32 / 0.01, 0.55, 16) == 16. The quiet
    // A clip must therefore peak at 0.16 even though mouse samples are louder.
    const double observed_peak = peak(sample);
    std::cerr << "quiet-source B7 expected peak=0.16 actual=" << observed_peak
              << " mapped-source-count=" << static_cast<unsigned>(bank.press[KEY_A].count) << '\n';
    if (std::abs(observed_peak - 0.16) > 0.002) return 1;
    if (bank.press[KEY_A].count != 2) return 2; // source files only, no synthetic pitch variants

    const auto& transient = bank.samples[bank.press[KEY_Q].first].pcm;
    if (transient.size() / 2 != 4801 || peak(transient) < 0.65 || peak(transient) > 0.75) {
        std::cerr << "transient frames=" << transient.size() / 2 << " peak=" << peak(transient) << '\n';
        return 3;
    }
    const auto& near_unity = bank.samples[bank.press[KEY_W].first].pcm;
    if (std::abs(peak(near_unity) - 0.319) > 1e-6) return 4;

    const float attack_full = first_sample(bank, 1.0f);
    const float attack_half = first_sample(bank, 0.5f);
    if (!(attack_full > 0.0f && std::abs(attack_full / attack_half - 2.0f) < 0.01f)) return 5;
    if (first_sample(bank, 0.0f) != 0.0f) return 20;

    SoundBank invalid_policy = bank;
    invalid_policy.playback_policy = static_cast<PlaybackPolicy>(99);
    if (validate_sound_bank(invalid_policy)) return 15;
    SoundBank excessive_pcm = bank;
    excessive_pcm.samples[excessive_pcm.press[KEY_Q].first].pcm[0] = 16.01f;
    if (validate_sound_bank(excessive_pcm)) return 16;
    SoundBank nonfinite_pcm = bank;
    nonfinite_pcm.samples[nonfinite_pcm.press[KEY_Q].first].pcm[0] = std::numeric_limits<float>::quiet_NaN();
    if (validate_sound_bank(nonfinite_pcm)) return 17;

    auto render_center = [](SampleMixer& mixer, uint16_t code, KeyEventKind kind) {
        assert(mixer.handle_event({code, kind, 0}) == TriggerResult::Started);
        std::array<float, 4800 * 2> output{};
        uint32_t cursor = 0;
        while (cursor < 4800) {
            const uint32_t count = std::min<uint32_t>(kMaxFramesPerCallback, 4800 - cursor);
            mixer.mix(output.data() + static_cast<size_t>(cursor) * 2, count);
            cursor += count;
        }
        return output;
    };
    SampleMixer sequence_mixer(bank, MixerVariation{0x4b454542u, 0.5f, 0.5f});
    sequence_mixer.set_master_gain(1.0f);
    sequence_mixer.set_stereo_width(0.0f);
    const auto first_a = render_center(sequence_mixer, KEY_A, KeyEventKind::Down);
    const auto second_space = render_center(sequence_mixer, KEY_SPACE, KeyEventKind::Down);
    const auto third_a = render_center(sequence_mixer, KEY_A, KeyEventKind::Down);
    const auto a_release = render_center(sequence_mixer, KEY_A, KeyEventKind::Up);
    const float phase_down = 0.28f * 0.88f;
    const float phase_up = 0.16f * 0.88f;
    const auto at_frame200 = [](const auto& pcm) { return pcm[200 * 2]; };
    SampleMixer pointer_sequence_mixer(bank, MixerVariation{0x4b454542u, 0.5f, 0.5f});
    pointer_sequence_mixer.set_master_gain(1.0f);
    pointer_sequence_mixer.set_stereo_width(0.0f);
    const auto pointer_click = render_center(pointer_sequence_mixer, BTN_RIGHT, KeyEventKind::Down);
    const auto keyboard_after_pointer = render_center(pointer_sequence_mixer, KEY_A, KeyEventKind::Down);
    if (std::abs(at_frame200(pointer_click) - 0.16f * phase_down * 2.0f) > 1e-5f) return 18;
    if (std::abs(at_frame200(keyboard_after_pointer) - 0.16f * phase_down * 2.0f) > 1e-5f) return 19;

    if (std::abs(at_frame200(first_a) - 0.16f * phase_down * 2.0f) > 1e-5f) return 6;
    if (std::abs(at_frame200(second_space) - 0.32f * phase_down * 2.0f) > 1e-5f) return 7;
    if (std::abs(at_frame200(third_a) - 0.16f * phase_down * 2.0f) > 1e-5f) return 8;
    if (std::abs(at_frame200(a_release) - 0.16f * phase_up * 2.0f) > 1e-5f) return 9;
    const float last_envelope = 0.001f + (phase_down - 0.001f) / 960.0f;
    if (std::abs(first_a.back() - 0.16f * last_envelope * 2.0f) > 1e-5f) return 10;

    const fs::path brown = temp_root / "keeby-web-keychron-k2-max-brown";
    write_constant_audio(brown / "sound.ogg", 44100, 8820, 0.01f, SF_FORMAT_OGG | SF_FORMAT_VORBIS);
    write_text(brown / "config.json", R"({
        "version": 2,
        "id": "keeby-web-keychron-k2-max-brown",
        "key_define_type": "single",
        "sound": "sound.ogg",
        "defines": { "30": [10, 50], "30-up": [60, 50] }
    })");
    auto brown_loaded = load_pack(brown);
    assert(brown_loaded);
    if (brown_loaded->playback_policy != PlaybackPolicy::KeebyCatalogSprite ||
        brown_loaded->press[KEY_A].count != 1 || brown_loaded->release[KEY_A].count != 1) return 11;
    uint32_t full_frames = 0;
    const auto decoded = read_resampled_sprite(brown / "sound.ogg", full_frames);
    const size_t first_frame = 480;
    const size_t slice_frames = 2400;
    const auto& brown_slice = brown_loaded->samples[brown_loaded->press[KEY_A].first].pcm;
    if (full_frames < first_frame + slice_frames || brown_slice.size() != slice_frames * 2) return 12;
    for (size_t i = 0; i < brown_slice.size(); ++i)
        if (std::abs(brown_slice[i] - decoded[first_frame * 2 + i]) > 1e-6f) return 13;
    if (peak(brown_slice) >= 0.02) return 14; // OGG keeps its decoded source level; no B7 boost.

    std::error_code ec;
    fs::remove_all(temp_root, ec);
    std::cout << "native_catalog_audio_policy_test: OK\n";
    return 0;
}
