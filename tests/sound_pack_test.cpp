#include "sound_pack.hpp"
#include "key_layout.hpp"

#include <linux/input-event-codes.h>
#include <sndfile.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace keeby;
namespace fs = std::filesystem;

namespace {

fs::path make_temp_dir(const char* suffix) {
    std::string tmpl = std::string("/tmp/keeby-pack-test-") + suffix + "-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    assert(mkdtemp(buf.data()) != nullptr);
    return fs::path(buf.data());
}

void write_text(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
    assert(out.good());
}

// A short, deterministic tone (distinct per-frame values) so slice offsets
// can be checked by content, not just by length.
void write_audio(const fs::path& path, int format, int rate, int channels, int frames, float amplitude) {
    fs::create_directories(path.parent_path());
    SF_INFO info{};
    info.samplerate = rate;
    info.channels = channels;
    info.format = format;
    SNDFILE* f = sf_open(path.c_str(), SFM_WRITE, &info);
    assert(f);
    std::vector<float> pcm(static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels));
    for (int i = 0; i < frames; ++i) {
        const float v = amplitude * std::sin(0.3f * static_cast<float>(i));
        for (int c = 0; c < channels; ++c) pcm[static_cast<std::size_t>(i) * channels + c] = v;
    }
    assert(sf_writef_float(f, pcm.data(), frames) == frames);
    assert(sf_close(f) == 0);
}

void write_wav48(const fs::path& path, int frames, float amplitude = 0.5f) {
    write_audio(path, SF_FORMAT_WAV | SF_FORMAT_FLOAT, 48000, 2, frames, amplitude);
}

struct EnvGuard {
    std::string name;
    bool had_value = false;
    std::string old_value;
    explicit EnvGuard(const char* n) : name(n) {
        if (const char* v = std::getenv(n)) { had_value = true; old_value = v; }
    }
    ~EnvGuard() {
        if (had_value) setenv(name.c_str(), old_value.c_str(), 1);
        else unsetenv(name.c_str());
    }
};

int count_defined_press_codes(const SoundBank& bank) {
    int n = 0;
    for (std::size_t c = 1; c < kKeyCodeCount; ++c)
        if (bank.press[c].count > 0) ++n;
    return n;
}

} // namespace

// --- v1 single sprite, 48 kHz: exact slice boundaries + pitch variants ---
static void test_v1_single_sprite_48k() {
    auto dir = make_temp_dir("v1-single-48k");
    write_wav48(dir / "sprite.wav", 1000);
    write_text(dir / "config.json", R"({
        "key_define_type": "single",
        "sound": "sprite.wav",
        "defines": { "30": [0, 10], "48": [10, 10] }
    })");

    auto bank = load_pack(dir);
    assert(bank);
    assert(bank->press[KEY_A].count == 3);  // base + low + high
    assert(bank->press[KEY_B].count == 3);
    assert(bank->press[KEY_A].first != bank->press[KEY_B].first);

    // Whole-pack loudness normalization (normalize_pack_loudness) rescales
    // every sample by one global factor once the whole bank is built, so raw
    // slice content can't be checked against the source tone's formula
    // directly. base_a[0] (source frame 0) is exactly sin(0) == 0, which is
    // scale-invariant; every other check derives the pack's actual scale from
    // that same range's frame 100 and confirms OTHER offsets -- including
    // base_b's start -- stay consistent with that one factor, which is
    // exactly as sensitive to a wrong slice offset as an absolute check.
    const auto& base_a = bank->samples[bank->press[KEY_A].first].pcm;
    assert(base_a.size() / kNativeChannels == 480); // 10ms @ 48kHz, exact
    assert(std::abs(base_a[0] - 0.5f * std::sin(0.0f)) < 1e-5f);

    const double raw_a_100 = 0.5 * std::sin(0.3 * 100.0);
    const double scale = base_a[100 * kNativeChannels] / raw_a_100;
    assert(scale > 0.1 && scale < 4.5); // sane normalization factor (cap is 4x)
    auto expect_scaled = [&](float actual, double raw) {
        assert(std::abs(actual - static_cast<float>(scale * raw)) < 1e-3f);
    };
    expect_scaled(base_a[200 * kNativeChannels], 0.5 * std::sin(0.3 * 200.0));

    const auto& base_b = bank->samples[bank->press[KEY_B].first].pcm;
    assert(base_b.size() / kNativeChannels == 480);
    expect_scaled(base_b[0], 0.5 * std::sin(0.3 * 480.0));               // starts at source frame 480
    expect_scaled(base_b[100 * kNativeChannels], 0.5 * std::sin(0.3 * 580.0));

    const auto low = bank->samples[bank->press[KEY_A].first + 1].pcm.size() / kNativeChannels;
    const auto high = bank->samples[bank->press[KEY_A].first + 2].pcm.size() / kNativeChannels;
    assert(std::abs(static_cast<double>(low) - 480.0 / 0.92) < 6.0);
    assert(std::abs(static_cast<double>(high) - 480.0 / 1.08) < 6.0);
    assert(validate_sound_bank(*bank));

    std::error_code ec;
    fs::remove_all(dir, ec);
    std::puts("test_v1_single_sprite_48k: OK");
}

// --- v1 single sprite, 44.1 kHz: resampled length ~= x 48000/44100 ---
static void test_v1_single_sprite_44k1() {
    auto dir = make_temp_dir("v1-single-44k1");
    write_audio(dir / "sprite.wav", SF_FORMAT_WAV | SF_FORMAT_FLOAT, 44100, 2, 1000, 0.5f);
    write_text(dir / "config.json", R"({
        "key_define_type": "single",
        "sound": "sprite.wav",
        "defines": { "30": [0, 10] }
    })");

    auto bank = load_pack(dir);
    assert(bank);
    assert(bank->press[KEY_A].count == 3);
    const double expected = 441.0 * 48000.0 / 44100.0; // 441 source frames = 10ms @ 44100
    const auto got = bank->samples[bank->press[KEY_A].first].pcm.size() / kNativeChannels;
    assert(std::abs(static_cast<double>(got) - expected) < 6.0);

    std::error_code ec;
    fs::remove_all(dir, ec);
    std::puts("test_v1_single_sprite_44k1: OK");
}

// --- v2 multi: {0-2} range, "-up" key, soundup default, fallback for undefined keys ---
static void test_v2_multi_range_up_fallback() {
    auto dir = make_temp_dir("v2-multi");
    write_wav48(dir / "default_press.wav", 200, 0.3f);
    write_wav48(dir / "default_release.wav", 150, 0.2f);
    write_wav48(dir / "press" / "GENERIC_0.wav", 100, 0.4f);
    write_wav48(dir / "press" / "GENERIC_1.wav", 110, 0.4f);
    write_wav48(dir / "press" / "GENERIC_2.wav", 120, 0.4f);
    write_wav48(dir / "release" / "16up.wav", 90, 0.25f);
    write_text(dir / "config.json", R"({
        "key_define_type": "multi",
        "version": 2,
        "sound": "default_press.wav",
        "soundup": "default_release.wav",
        "defines": {
            "16": "press/GENERIC_{0-2}.wav",
            "16-up": "release/16up.wav"
        }
    })");

    auto bank = load_pack(dir);
    assert(bank);
    assert(bank->press[KEY_Q].count == 9);   // 3 files * 3 pitch variants each
    assert(bank->release[KEY_Q].count == 3); // 1 explicit file * 3 variants

    // Undefined keys fall back to the pack defaults (v2 only), and share
    // one range (dedupe of the shared default clip).
    assert(bank->press[KEY_W].count == 3);
    assert(bank->release[KEY_W].count == 3);
    assert(bank->press[BTN_LEFT].count == 3 && bank->release[BTN_LEFT].count == 3);
    assert(bank->press[BTN_LEFT].first == bank->press[KEY_W].first);
    assert(bank->press[KEY_FN].count == 3 && bank->release[KEY_FN].count == 3);
    assert(bank->press[KEY_FN].first == bank->press[KEY_W].first);
    assert(bank->press[256].count == 0 && bank->press[271].count == 0);
    assert(bank->press[KEY_E].count == 3);
    assert(bank->press[KEY_E].first == bank->press[KEY_W].first);
    assert(bank->release[KEY_E].first == bank->release[KEY_W].first);

    // KEY_Q's explicit ranges are distinct from the defaults.
    assert(bank->press[KEY_Q].first != bank->press[KEY_W].first);
    assert(bank->release[KEY_Q].first != bank->release[KEY_W].first);
    assert(validate_sound_bank(*bank));

    // Imported press/release PCM uses the same 2x gain as the built-in bank.
    bank->press[KEY_Q].count = bank->release[KEY_Q].count = 1;
    SampleMixer mixer(*bank, {.min_gain = kVoiceGain, .max_gain = kVoiceGain});
    mixer.set_stereo_width(0.0f);
    for (auto kind : {KeyEventKind::Down, KeyEventKind::Up}) {
        const auto sound = kind == KeyEventKind::Down ? bank->press[KEY_Q] : bank->release[KEY_Q];
        const auto& pcm = bank->samples[sound.first].pcm;
        assert(std::any_of(pcm.begin(), pcm.end(), [](float v) { return v != 0.0f; }));
        std::vector<float> out(pcm.size());
        for (float volume : {0.0f, 0.25f, 1.0f}) {
            mixer.reset();
            mixer.set_master_gain(volume);
            assert(mixer.handle_event({KEY_Q, kind, 0}) == TriggerResult::Started);
            mixer.mix(out.data(), static_cast<uint32_t>(pcm.size() / kNativeChannels));
            for (std::size_t i = 0; i < pcm.size(); ++i) {
                const float expected = std::clamp(pcm[i] * kVoiceGain * volume * 2.0f, -kOutputPeak, kOutputPeak);
                assert(std::abs(out[i] - expected) < 1e-6f);
            }
        }
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    std::puts("test_v2_multi_range_up_fallback: OK");
}

// --- v2 single: "-up" slices into the same sprite ---
static void test_v2_single_up_slices() {
    auto dir = make_temp_dir("v2-single-up");
    write_wav48(dir / "sprite.wav", 1000);
    write_text(dir / "config.json", R"({
        "version": 2,
        "key_define_type": "single",
        "sound": "sprite.wav",
        "defines": { "30": [0, 10], "30-up": [10, 10] }
    })");

    auto bank = load_pack(dir);
    assert(bank);
    assert(bank->press[KEY_A].count == 3);
    assert(bank->release[KEY_A].count == 3);
    assert(bank->press[KEY_A].first != bank->release[KEY_A].first);

    std::error_code ec;
    fs::remove_all(dir, ec);
    std::puts("test_v2_single_up_slices: OK");
}

// --- v1 multi: no fallback, even though a top-level `sound` is present ---
static void test_v1_multi_no_fallback() {
    auto dir = make_temp_dir("v1-multi");
    write_wav48(dir / "a.wav", 100);
    // Deliberately do NOT create default.wav: proves v1's `sound` field is
    // never read (let alone resolved/decoded) in multi mode.
    write_text(dir / "config.json", R"({
        "key_define_type": "multi",
        "sound": "default.wav",
        "defines": { "30": "a.wav" }
    })");

    auto bank = load_pack(dir);
    assert(bank);
    assert(bank->press[KEY_A].count == 3);
    assert(bank->press[KEY_B].count == 0); // undefined key stays silent: no fallback in v1

    std::error_code ec;
    fs::remove_all(dir, ec);
    std::puts("test_v1_multi_no_fallback: OK");
}

// --- v1: "-up" keys are ignored (v1 has no key-up sounds), single AND multi ---
static void test_v1_ignores_up_keys() {
    { // single mode
        auto dir = make_temp_dir("v1-up-single");
        write_wav48(dir / "sprite.wav", 1000);
        write_text(dir / "config.json", R"({
            "key_define_type": "single",
            "sound": "sprite.wav",
            "defines": { "30": [0, 10], "30-up": [10, 10] }
        })");
        auto bank = load_pack(dir);
        assert(bank);
        assert(bank->press[KEY_A].count == 3);
        assert(bank->release[KEY_A].count == 0); // "-up" ignored in v1: silent, not an error
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    { // multi mode
        auto dir = make_temp_dir("v1-up-multi");
        write_wav48(dir / "a.wav", 100);
        write_wav48(dir / "a-up.wav", 100);
        write_text(dir / "config.json", R"({
            "key_define_type": "multi",
            "defines": { "30": "a.wav", "30-up": "a-up.wav" }
        })");
        auto bank = load_pack(dir);
        assert(bank);
        assert(bank->press[KEY_A].count == 3);
        assert(bank->release[KEY_A].count == 0);
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::puts("test_v1_ignores_up_keys: OK");
}

// --- key code mapping: extended codes, plus unknown/out-of-range skipped ---
static void test_key_code_mapping() {
    auto dir = make_temp_dir("code-map");
    write_wav48(dir / "sprite.wav", 2000);
    write_text(dir / "config.json", R"({
        "key_define_type": "single",
        "version": 2,
        "sound": "sprite.wav",
        "defines": {
            "3612": [0, 10],
            "57416": [10, 10],
            "3675": [20, 10],
            "272": [0, 10],
            "272-up": [0, 10],
            "273": [0, 10],
            "273-up": [0, 10],
            "274": [0, 10],
            "274-up": [0, 10],
            "464": [0, 10],
            "464-up": [0, 10],
            "256": [0, 10],
            "271": [0, 10],
            "275": [0, 10],
            "463": [0, 10],
            "465": [0, 10],
            "300": [30, 10],
            "999999999999999999": [40, 10]
        }
    })");

    auto bank = load_pack(dir);
    assert(bank);
    assert(bank->press[KEY_KPENTER].count == 3);
    assert(bank->press[KEY_UP].count == 3);
    assert(bank->press[KEY_LEFTMETA].count == 3);
    for (uint16_t code : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}) {
        assert(bank->press[code].count == 3);
        assert(bank->release[code].count == 3);
    }
    assert(bank->press[KEY_FN].count == 3 && bank->release[KEY_FN].count == 3);
    assert(bank->press[256].count == 0 && bank->press[271].count == 0);
    assert(bank->press[275].count == 0 && bank->press[463].count == 0);
    assert(count_defined_press_codes(*bank) == 7); // gaps and unknown codes stay skipped

    std::error_code ec;
    fs::remove_all(dir, ec);
    std::puts("test_key_code_mapping: OK");
}

// --- whole-pack peak normalization, and its 4x boost cap ---
static void test_peak_normalization() {
    auto peak_of = [](const SoundBank& bank) {
        float peak = 0.0f;
        for (const auto& s : bank.samples)
            for (float v : s.pcm) peak = std::max(peak, std::fabs(v));
        return peak;
    };

    { // loud pack: normalized down toward 0.9, uncapped
        auto dir = make_temp_dir("norm-loud");
        write_wav48(dir / "sprite.wav", 500, 0.99f);
        write_text(dir / "config.json", R"({"key_define_type":"single","sound":"sprite.wav",
                                             "defines":{"30":[0,10]}})");
        auto bank = load_pack(dir);
        assert(bank);
        assert(std::abs(peak_of(*bank) - 0.9f) < 0.02f);
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    { // very quiet pack: boost capped at 4x, well short of 0.9
        auto dir = make_temp_dir("norm-quiet");
        write_wav48(dir / "sprite.wav", 500, 0.01f);
        write_text(dir / "config.json", R"({"key_define_type":"single","sound":"sprite.wav",
                                             "defines":{"30":[0,10]}})");
        auto bank = load_pack(dir);
        assert(bank);
        const float peak = peak_of(*bank);
        assert(peak < 0.9f * 0.5f); // capped boost never reaches the 0.9 target from this far down
        assert(peak <= 0.01f * 4.0f + 0.01f); // and never exceeds the 4x cap (with resampler headroom)
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::puts("test_peak_normalization: OK");
}

// --- dedupe: keys sharing one clip/slice share one range, never duplicated ---
static void test_clip_dedupe() {
    { // multi mode: two keys, same filename
        auto dir = make_temp_dir("dedupe-multi");
        write_wav48(dir / "shared.wav", 100);
        write_text(dir / "config.json", R"({"key_define_type":"multi",
            "defines":{"30":"shared.wav","48":"shared.wav"}})");
        auto bank = load_pack(dir);
        assert(bank);
        assert(bank->press[KEY_A].first == bank->press[KEY_B].first);
        assert(bank->press[KEY_A].count == bank->press[KEY_B].count);
        assert(bank->samples.size() == 3); // one clip's worth of variants, not six
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    { // single mode: two keys, identical slice
        auto dir = make_temp_dir("dedupe-single");
        write_wav48(dir / "sprite.wav", 500);
        write_text(dir / "config.json", R"({"key_define_type":"single","sound":"sprite.wav",
            "defines":{"30":[0,10],"48":[0,10]}})");
        auto bank = load_pack(dir);
        assert(bank);
        assert(bank->press[KEY_A].first == bank->press[KEY_B].first);
        assert(bank->samples.size() == 3);
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::puts("test_clip_dedupe: OK");
}

// --- rejections: every one of these must fail cleanly, never crash ---
static void test_rejections() {
    { // "../x.wav"
        auto dir = make_temp_dir("rej-dotdot");
        write_text(dir / "config.json",
                   R"({"key_define_type":"single","sound":"../x.wav","defines":{"30":[0,10]}})");
        assert(!load_pack(dir));
        std::error_code ec; fs::remove_all(dir, ec);
    }
    { // "/etc/passwd"
        auto dir = make_temp_dir("rej-abs");
        write_text(dir / "config.json",
                   R"({"key_define_type":"single","sound":"/etc/passwd","defines":{"30":[0,10]}})");
        assert(!load_pack(dir));
        std::error_code ec; fs::remove_all(dir, ec);
    }
    { // symlink escaping the pack dir
        auto dir = make_temp_dir("rej-symlink");
        auto outside_root = make_temp_dir("rej-symlink-outside");
        write_wav48(outside_root / "outside.wav", 100);
        std::error_code sym_ec;
        fs::create_symlink(outside_root / "outside.wav", dir / "link.wav", sym_ec);
        assert(!sym_ec);
        write_text(dir / "config.json",
                   R"({"key_define_type":"single","sound":"link.wav","defines":{"30":[0,10]}})");
        assert(!load_pack(dir));
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::remove_all(outside_root, ec);
    }
    { // malformed JSON
        auto dir = make_temp_dir("rej-badjson");
        write_text(dir / "config.json", "{ this is not json ");
        assert(!load_pack(dir));
        std::error_code ec; fs::remove_all(dir, ec);
    }
    { // missing file
        auto dir = make_temp_dir("rej-missing");
        write_text(dir / "config.json",
                   R"({"key_define_type":"single","sound":"nofile.wav","defines":{"30":[0,10]}})");
        assert(!load_pack(dir));
        std::error_code ec; fs::remove_all(dir, ec);
    }
    { // v3 config
        auto dir = make_temp_dir("rej-v3");
        write_wav48(dir / "sprite.wav", 100);
        write_text(dir / "config.json",
                   R"({"version":3,"key_define_type":"single","sound":"sprite.wav","defines":{"30":[0,10]}})");
        assert(!load_pack(dir));
        std::error_code ec; fs::remove_all(dir, ec);
    }
    { // oversize config
        auto dir = make_temp_dir("rej-oversize");
        write_wav48(dir / "sprite.wav", 100);
        std::string padding(1100000, 'a');
        write_text(dir / "config.json",
                   R"({"key_define_type":"single","sound":"sprite.wav","defines":{"30":[0,10]},"pad":")" +
                       padding + R"("})");
        assert(!load_pack(dir));
        std::error_code ec; fs::remove_all(dir, ec);
    }
    std::puts("test_rejections: OK");
}

// --- real-pack compatibility: null defines are silent/fallback, never an error ---
static void test_null_defines() {
    { // single mode: null press and null "-up" are both silent
        auto dir = make_temp_dir("null-single");
        write_wav48(dir / "sprite.wav", 1000);
        write_text(dir / "config.json", R"({
            "version": 2, "key_define_type": "single", "sound": "sprite.wav",
            "defines": { "30": [0, 10], "48": null, "30-up": null }
        })");
        auto bank = load_pack(dir);
        assert(bank);
        assert(bank->press[KEY_A].count == 3);
        assert(bank->press[KEY_B].count == 0);   // null: silent, not an error
        assert(bank->release[KEY_A].count == 0); // null "-up": silent
        std::error_code ec; fs::remove_all(dir, ec);
    }
    { // v1 multi: null is silent (no fallback exists in v1 anyway)
        auto dir = make_temp_dir("null-v1-multi");
        write_wav48(dir / "a.wav", 100);
        write_text(dir / "config.json",
                   R"({"key_define_type":"multi","defines":{"30":"a.wav","48":null}})");
        auto bank = load_pack(dir);
        assert(bank);
        assert(bank->press[KEY_A].count == 3);
        assert(bank->press[KEY_B].count == 0);
        std::error_code ec; fs::remove_all(dir, ec);
    }
    { // v2 multi: null falls back to the default, exactly like an undefined key
        auto dir = make_temp_dir("null-v2-multi");
        write_wav48(dir / "a.wav", 100);
        write_wav48(dir / "default.wav", 150);
        write_text(dir / "config.json", R"({
            "version": 2, "key_define_type": "multi", "sound": "default.wav",
            "defines": { "30": "a.wav", "48": null }
        })");
        auto bank = load_pack(dir);
        assert(bank);
        assert(bank->press[KEY_A].count == 3);
        assert(bank->press[KEY_B].count == 3);              // null key still sounds via fallback
        assert(bank->press[KEY_B].first != bank->press[KEY_A].first);
        assert(bank->press[KEY_C].first == bank->press[KEY_B].first); // same fallback as a truly undefined key
        std::error_code ec; fs::remove_all(dir, ec);
    }
    std::puts("test_null_defines: OK");
}

// --- real-pack compatibility: a per-key file missing on disk skips that key ---
static void test_missing_per_key_file() {
    { // v2 multi: falls back to the default, with a warning (mxblue-travel's case)
        auto dir = make_temp_dir("missing-v2-fallback");
        write_wav48(dir / "a.wav", 100);
        write_wav48(dir / "default.wav", 150);
        // Deliberately never create "missing.wav".
        write_text(dir / "config.json", R"({
            "version": 2, "key_define_type": "multi", "sound": "default.wav",
            "defines": { "30": "a.wav", "48": "missing.wav" }
        })");
        std::vector<std::string> warnings;
        auto bank = load_pack(dir, &warnings);
        assert(bank);
        assert(bank->press[KEY_A].count == 3);
        assert(bank->press[KEY_B].count == 3); // fell back to the default
        assert(bank->press[KEY_B].first != bank->press[KEY_A].first);
        assert(warnings.size() == 1);
        assert(warnings[0].find("missing.wav") != std::string::npos);
        std::error_code ec; fs::remove_all(dir, ec);
    }
    { // v1 multi: no fallback available, so the key just stays silent
        auto dir = make_temp_dir("missing-v1-silent");
        write_wav48(dir / "a.wav", 100);
        write_text(dir / "config.json",
                   R"({"key_define_type":"multi","defines":{"30":"a.wav","48":"missing.wav"}})");
        std::vector<std::string> warnings;
        auto bank = load_pack(dir, &warnings);
        assert(bank);
        assert(bank->press[KEY_A].count == 3);
        assert(bank->press[KEY_B].count == 0);
        assert(warnings.size() == 1);
        std::error_code ec; fs::remove_all(dir, ec);
    }
    { // two keys sharing the same missing file collapse to one warning
        auto dir = make_temp_dir("missing-collapse");
        write_wav48(dir / "a.wav", 100);
        write_text(dir / "config.json", R"({"key_define_type":"multi",
            "defines":{"30":"a.wav","48":"missing.wav","18":"missing.wav"}})");
        std::vector<std::string> warnings;
        auto bank = load_pack(dir, &warnings);
        assert(bank);
        assert(warnings.size() == 1); // collapsed, not one per key
        std::error_code ec; fs::remove_all(dir, ec);
    }
    std::puts("test_missing_per_key_file: OK");
}

// --- real-pack compatibility: these stay hard errors, never silently skipped ---
static void test_still_hard_errors() {
    { // missing v2 default `sound`, referenced by key_define_type multi
        auto dir = make_temp_dir("missing-default-error");
        write_wav48(dir / "a.wav", 100);
        write_text(dir / "config.json", R"({
            "version": 2, "key_define_type": "multi", "sound": "no_such_default.wav",
            "defines": { "30": "a.wav" }
        })");
        assert(!load_pack(dir));
        std::error_code ec; fs::remove_all(dir, ec);
    }
    { // single mode: every define null -> no press sound for any key
        auto dir = make_temp_dir("no-press-single");
        write_wav48(dir / "sprite.wav", 100);
        write_text(dir / "config.json",
                   R"({"key_define_type":"single","sound":"sprite.wav","defines":{"30":null}})");
        assert(!load_pack(dir));
        std::error_code ec; fs::remove_all(dir, ec);
    }
    { // v2 multi: only a release default/define exists, no press anywhere -> error
        auto dir = make_temp_dir("no-press-multi");
        write_wav48(dir / "up.wav", 100);
        write_text(dir / "config.json", R"({
            "version": 2, "key_define_type": "multi", "soundup": "up.wav",
            "defines": { "30-up": "up.wav" }
        })");
        assert(!load_pack(dir));
        std::error_code ec; fs::remove_all(dir, ec);
    }
    std::puts("test_still_hard_errors: OK");
}

// --- list_packs: $XDG_DATA_HOME before $XDG_DATA_DIRS, first id wins, "default" reserved ---
static void test_list_packs_precedence() {
    EnvGuard home_guard("XDG_DATA_HOME");
    EnvGuard dirs_guard("XDG_DATA_DIRS");

    auto home = make_temp_dir("xdg-home");
    auto extra = make_temp_dir("xdg-dirs");
    write_text(home / "keeby/packs/mypack/config.json", R"({"name":"Home Version"})");
    write_text(extra / "keeby/packs/mypack/config.json", R"({"name":"Dirs Version"})"); // shadowed
    write_text(extra / "keeby/packs/other/config.json", R"({"name":"Other"})");
    write_text(home / "keeby/packs/default/config.json", R"({"name":"Sneaky"})"); // reserved, ignored

    setenv("XDG_DATA_HOME", home.c_str(), 1);
    setenv("XDG_DATA_DIRS", extra.c_str(), 1);

    auto packs = list_packs();
    std::string default_name, mypack_name;
    bool has_other = false;
    for (const auto& p : packs) {
        if (p.id == "default") default_name = p.name;
        if (p.id == "mypack") mypack_name = p.name;
        if (p.id == "other") has_other = true;
    }
    assert(default_name == "Default (built-in)"); // never shadowed by a directory named "default"
    assert(mypack_name == "Home Version");         // XDG_DATA_HOME wins over XDG_DATA_DIRS
    assert(has_other);

    std::error_code ec;
    fs::remove_all(home, ec);
    fs::remove_all(extra, ec);
    std::puts("test_list_packs_precedence: OK");
}

// --- swap safety: an RT stand-in thread mixes while another swaps banks ---
static void test_swap_safety_stress() {
    auto make_bank = [](float value) {
        SoundBank bank;
        bank.samples.push_back(Sample{{value, -value, value, -value}});
        bank.press[KEY_A] = {0, 1};
        return std::make_unique<const SoundBank>(std::move(bank));
    };

    constexpr int kSlots = 4;
    constexpr int kIterations = 200;
    std::vector<std::unique_ptr<const SoundBank>> banks;
    for (int i = 0; i < kSlots; ++i) banks.push_back(make_bank(0.1f * static_cast<float>(i + 1)));

    SampleMixer mixer(SoundBank{});
    std::atomic<bool> stop{false};
    std::thread rt_thread([&] {
        std::array<float, 64> scratch{};
        while (!stop.load(std::memory_order_relaxed)) {
            mixer.handle_event({KEY_A, KeyEventKind::Down, 0});
            mixer.mix(scratch.data(), 8);
        }
    });

    std::vector<std::unique_ptr<const SoundBank>> retired;
    for (int i = 0; i < kIterations; ++i) {
        const auto slot = static_cast<std::size_t>(i) % kSlots;
        const SoundBank* raw = banks[slot].get();
        retired.push_back(std::move(banks[slot]));
        banks[slot] = make_bank(0.01f * static_cast<float>(i % 11 + 1));

        mixer.request_bank(raw);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (mixer.active_bank() != raw && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        assert(mixer.active_bank() == raw); // the RT stand-in must keep up within the deadline

        // Same pruning rule as AudioBoundary::swap_bank: free anything that
        // is neither the latest request nor the RT-acknowledged bank.
        const SoundBank* requested = mixer.requested_bank();
        const SoundBank* active = mixer.active_bank();
        std::erase_if(retired, [&](const std::unique_ptr<const SoundBank>& b) {
            return b.get() != requested && b.get() != active;
        });
    }

    stop.store(true, std::memory_order_relaxed);
    rt_thread.join();
    std::puts("test_swap_safety_stress: OK");
}

int main() {
    test_v1_single_sprite_48k();
    test_v1_single_sprite_44k1();
    test_v2_multi_range_up_fallback();
    test_v2_single_up_slices();
    test_v1_multi_no_fallback();
    test_v1_ignores_up_keys();
    test_key_code_mapping();
    test_peak_normalization();
    test_clip_dedupe();
    test_rejections();
    test_null_defines();
    test_missing_per_key_file();
    test_still_hard_errors();
    test_list_packs_precedence();
    test_swap_safety_stress();
    std::puts("sound_pack_test: OK (mechvibes v1/v2 parsing, key mapping, normalization, dedupe, "
              "path/config safety rejections, list_packs precedence, RT-safe bank swap under concurrency)");
}
