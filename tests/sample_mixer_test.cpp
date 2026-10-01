#include "sample_mixer.hpp"
#include "key_layout.hpp"

#include <linux/input-event-codes.h>
#include <sndfile.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace keeby;

// Single variants and fixed trigger gain isolate output gain from variation.
static constexpr MixerVariation fixed_gain{.min_gain = kVoiceGain, .max_gain = kVoiceGain};

// Two-range helper bank: `a`/`b_keys` press sample 0/1 respectively. Pan
// stays at the default 0 (center) for every key, so gain_l == gain_r ==
// the plain per-trigger gain -- exercised separately by test_pan.
static SoundBank two_press_bank(Sample a, Sample b, std::initializer_list<uint16_t> a_keys,
                                std::initializer_list<uint16_t> b_keys) {
    SoundBank bank;
    bank.samples = {std::move(a), std::move(b)};
    for (auto k : a_keys) bank.press[k] = {0, 1};
    for (auto k : b_keys) bank.press[k] = {1, 1};
    return bank;
}

static SoundBank one_sample_bank(Sample s, uint16_t code = KEY_A) {
    SoundBank bank;
    bank.samples.push_back(std::move(s));
    bank.press[code] = {0, 1};
    return bank;
}

static TriggerResult press(SampleMixer& mixer, uint16_t code = KEY_A) {
    return mixer.handle_event({code, KeyEventKind::Down, 0});
}
static TriggerResult key_up(SampleMixer& mixer, uint16_t code = KEY_A) {
    return mixer.handle_event({code, KeyEventKind::Up, 0});
}

static void test_mix() {
    SoundBank bank = two_press_bank(Sample{{0.2f, -0.4f, 0.6f, -0.8f, 1.0f, -1.0f}},
                                    Sample{{-0.2f, 0.4f}}, {KEY_A, KEY_S, KEY_D}, {KEY_SPACE, KEY_ENTER});
    SampleMixer mixer(std::move(bank), fixed_gain);
    std::array<float, 8> out;
    out.fill(99);
    mixer.mix(out.data(), 4);
    for (auto x : out) assert(x == 0);
    assert(mixer.handle_event({KEY_A, KeyEventKind::Repeat, 0}) == TriggerResult::Ignored);
    assert(key_up(mixer) == TriggerResult::Ignored); // KEY_A has no release range in this bank
    assert(press(mixer, KEY_B) == TriggerResult::Ignored); // unmapped key
    mixer.mix(out.data(), 1);
    assert(out[0] == 0 && out[1] == 0);

    assert(press(mixer) == TriggerResult::Started);
    mixer.mix(out.data(), 0); // no advancement
    mixer.mix(out.data(), 1);
    assert(out[0] == 0.2f && out[1] == -0.4f);
    assert(press(mixer, KEY_SPACE) == TriggerResult::Started);
    mixer.mix(out.data(), 1); // second frame + first frame, separate cursors
    assert(std::abs(out[0] - 0.4f) < 1e-6f && out[1] == -0.4f);
    mixer.mix(out.data(), 2);
    assert(out[0] == kOutputPeak && out[1] == -kOutputPeak);
    assert(out[2] == 0 && out[3] == 0); // retired mid-buffer
    assert(press(mixer, KEY_ENTER) == TriggerResult::Started);
    mixer.mix(out.data(), 2); // reuse shorter sample: no old tail/cursor
    assert(out[0] == -0.2f && out[1] == 0.4f);
    assert(out[2] == 0 && out[3] == 0);

    assert(press(mixer) == TriggerResult::Started);
    mixer.mix(out.data(), 1);
    assert(press(mixer, KEY_S) == TriggerResult::Started); // same sample, overlaps
    mixer.mix(out.data(), 1);
    assert(out[0] == kOutputPeak && out[1] == -kOutputPeak);
    mixer.reset();
    mixer.mix(out.data(), 4);
    for (auto x : out) assert(x == 0);
    assert(press(mixer, KEY_D) == TriggerResult::Started);
    mixer.mix(out.data(), 1);
    assert(out[0] == 0.2f && out[1] == -0.4f);
}

static void test_pool_and_clamp() {
    SampleMixer mixer(two_press_bank(Sample{{0.01f, -0.01f, 0.02f, -0.02f}}, Sample{{1, 1}},
                                     {KEY_A}, {KEY_SPACE}), fixed_gain);
    for (std::size_t i = 0; i < kVoicePoolSize; ++i)
        assert(press(mixer) == TriggerResult::Started);
    assert(press(mixer, KEY_SPACE) == TriggerResult::PoolFull);
    std::array<float, 4> out;
    mixer.mix(out.data(), 2);
    assert(std::abs(out[0] - 0.32f) < 1e-6f && std::abs(out[1] + 0.32f) < 1e-6f);
    assert(std::abs(out[2] - 0.64f) < 1e-6f && std::abs(out[3] + 0.64f) < 1e-6f);
    // Full pool retired; every slot, not just the first, becomes reusable.
    for (std::size_t i = 0; i < kVoicePoolSize; ++i)
        assert(press(mixer, KEY_SPACE) == TriggerResult::Started);
    mixer.mix(out.data(), 2);
    assert(out[0] == kOutputPeak && out[1] == kOutputPeak);
    assert(out[2] == 0 && out[3] == 0);
    for (int i = 0; i < 1000; ++i) {
        assert(press(mixer) == TriggerResult::Started);
        mixer.mix(out.data(), 2);
        assert(out[0] == 0.01f && out[3] == -0.02f);
    }
    SampleMixer cancellation(two_press_bank(Sample{{1, -1}}, Sample{{-1, 1}}, {KEY_A}, {KEY_SPACE}), fixed_gain);
    for (int i = 0; i < 16; ++i) assert(press(cancellation) == TriggerResult::Started);
    for (int i = 0; i < 16; ++i) assert(press(cancellation, KEY_SPACE) == TriggerResult::Started);
    cancellation.mix(out.data(), 1);
    assert(out[0] == 0 && out[1] == 0); // clamp only AFTER all voices
    for (int i = 0; i < 32; ++i) assert(press(cancellation) == TriggerResult::Started);
    cancellation.mix(out.data(), 1);
    assert(out[0] == kOutputPeak && out[1] == -kOutputPeak);
}

static void test_loading() {
    for (auto name : {"click_down.wav", "click_up.wav", "release.wav"}) {
        auto sample = load_wav_sample(std::string(KEEBY_ASSET_DIR) + "/" + name);
        assert(sample && !sample->pcm.empty());
    }
    assert(!load_wav_sample("/no/such/keeby-sample.wav"));
    char directory[] = "/tmp/keeby-wav-test-XXXXXX";
    assert(mkdtemp(directory));
    const auto path = std::string(directory) + "/sample.wav";
    auto write = [&](int rate, int channels, int format, const std::vector<float>& pcm) {
        SF_INFO info{};
        info.samplerate = rate;
        info.channels = channels;
        info.format = format;
        auto* file = sf_open(path.c_str(), SFM_WRITE, &info);
        assert(file);
        const auto frames = static_cast<sf_count_t>(pcm.size() / channels);
        assert(sf_writef_float(file, pcm.data(), frames) == frames);
        assert(sf_close(file) == 0);
    };
    write(48000, 1, SF_FORMAT_WAV | SF_FORMAT_FLOAT, {0.25f, -0.5f, 1.0f});
    auto mono = load_wav_sample(path);
    assert(mono && mono->pcm == std::vector<float>({0.25f, 0.25f, -0.5f, -0.5f, 1, 1}));
    write(48000, 2, SF_FORMAT_WAV | SF_FORMAT_FLOAT, {0.25f, -0.5f});
    auto stereo = load_wav_sample(path);
    assert(stereo && stereo->pcm == std::vector<float>({0.25f, -0.5f}));
    write(44100, 2, SF_FORMAT_WAV | SF_FORMAT_PCM_16, {0, 0});
    assert(!load_wav_sample(path));
    write(48000, 3, SF_FORMAT_WAV | SF_FORMAT_PCM_16, {0, 0, 0});
    assert(!load_wav_sample(path));
    write(48000, 2, SF_FORMAT_AIFF | SF_FORMAT_PCM_16, {0, 0});
    assert(!load_wav_sample(path));
    write(48000, 2, SF_FORMAT_WAV | SF_FORMAT_FLOAT, {});
    assert(!load_wav_sample(path));
    write(48000, 1, SF_FORMAT_WAV | SF_FORMAT_PCM_16,
          std::vector<float>(kMaxSampleFrames + 1, 0));
    assert(!load_wav_sample(path));
    for (float bad : {1.1f, std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::quiet_NaN()}) {
        write(48000, 2, SF_FORMAT_WAV | SF_FORMAT_FLOAT, {bad, 0});
        assert(!load_wav_sample(path));
        bool rejected = false;
        try { SampleMixer invalid(one_sample_bank(Sample{{bad, 0}})); }
        catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
    bool rejected = false;
    try { SampleMixer invalid(one_sample_bank(Sample{{0}})); } // odd length
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { SampleMixer invalid(one_sample_bank(Sample{})); } // empty: no longer a valid "hole"
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    std::filesystem::resize_file(path, 8); // malformed/truncated container
    assert(!load_wav_sample(path));
    std::filesystem::remove_all(directory);
}

static SoundBank variant_bank() {
    SoundBank bank;
    // Left channel identifies variant independently of its per-trigger gain.
    bank.samples = {Sample{{0.125f, 1}}, Sample{{0.25f, 1}}, Sample{{0.375f, 1}}};
    bank.press[KEY_A] = {0, 3};
    return bank;
}

static void test_variation() {
    // Golden values below were captured by running this test once with the
    // documented default seed (1) and are pinned deliberately.
    constexpr std::array<int, 12> expected{0, 2, 1, 0, 2, 1, 0, 2, 0, 2, 1, 2};
    SampleMixer mixer(variant_bank(), {.seed = 1});
    SampleMixer replay(variant_bank(), {.seed = 1});
    // Keep unit-PCM variant ratios unclipped and compare the original PRNG golden.
    mixer.set_master_gain(0.5f);
    replay.set_master_gain(0.5f);
    std::array<float, 2> out{}, copy{};
    int last = -1;
    float minimum = 1, maximum = 0;
    for (int i = 0; i < 10000; ++i) {
        assert(press(mixer) == TriggerResult::Started);
        assert(press(replay) == TriggerResult::Started);
        mixer.mix(out.data(), 1);
        replay.mix(copy.data(), 1);
        assert(out == copy);
        const int variant = std::lround(out[0] / out[1] * 8) - 1;
        assert(variant >= 0 && variant < 3 && variant != last);
        if (i < static_cast<int>(expected.size())) assert(variant == expected[i]);
        if (i == 0) assert(std::abs(out[1] - 0.35472423f) < 1e-6f);
        assert(out[1] >= kMinVoiceGain && out[1] <= kMaxVoiceGain);
        minimum = std::min(minimum, out[1]); maximum = std::max(maximum, out[1]);
        last = variant;
    }
    assert(minimum < 0.36f && maximum > 0.64f); // variation, not a constant
    mixer.reset();
    SampleMixer zero_seed(variant_bank(), {.seed = 0}); // zero maps to one
    zero_seed.set_master_gain(0.5f);
    assert(press(mixer) == TriggerResult::Started);
    assert(press(zero_seed) == TriggerResult::Started);
    mixer.mix(out.data(), 1); zero_seed.mix(copy.data(), 1);
    assert(out == copy);

    // Fewer variants than the default 3: shrink both the range and the
    // backing vector together (an unreferenced-but-invalid sample would
    // still fail validate_sound_bank, which checks every sample in it).
    auto two = variant_bank();
    two.samples.resize(2);
    two.press[KEY_A].count = 2;
    SampleMixer pair(std::move(two));
    pair.set_master_gain(0.5f);
    last = -1;
    for (int i = 0; i < 100; ++i) {
        assert(press(pair) == TriggerResult::Started);
        pair.mix(out.data(), 1);
        const int variant = std::lround(out[0] / out[1] * 8) - 1;
        assert(variant >= 0 && variant < 2 && variant != last);
        last = variant;
    }
}

// Per-key history: a key never immediately repeats its own last variant,
// and pressing a DIFFERENT key backed by the identical sample range must
// not be constrained by the first key's history (proves the model tracks
// history per key code, not per shared range/category).
static void test_per_key_variant_history() {
    // Reuse variant_bank()'s {left, right=1} encoding: with pan == 0 (so
    // gain_l == gain_r), out[0]/out[1] recovers the variant regardless of
    // the per-trigger randomized gain -- MixerVariation here uses its
    // default (randomized) gain range deliberately, unlike fixed_gain.
    SoundBank bank = variant_bank();
    bank.press[KEY_S] = {0, 3}; // identical range to KEY_A, on purpose
    SampleMixer mixer(std::move(bank), {.seed = 7});
    mixer.set_master_gain(0.5f); // keep full-scale variant-ID channel unclipped
    std::array<float, 2> out{};
    auto variant_of = [&] {
        mixer.mix(out.data(), 1);
        return std::lround(out[0] / out[1] * 8) - 1;
    };
    int last_a = -1;
    for (int i = 0; i < 200; ++i) {
        assert(press(mixer, KEY_A) == TriggerResult::Started);
        const int v = variant_of();
        assert(v >= 0 && v < 3 && v != last_a);
        last_a = v;
    }
    bool saw_a_repeated_by_s = false;
    int last_s = -1;
    for (int i = 0; i < 200; ++i) {
        assert(press(mixer, KEY_A) == TriggerResult::Started);
        const int va = variant_of();
        assert(press(mixer, KEY_S) == TriggerResult::Started);
        const int vs = variant_of();
        assert(vs != last_s); // KEY_S still never repeats ITS OWN last pick
        if (vs == va) saw_a_repeated_by_s = true;
        last_s = vs;
    }
    assert(saw_a_repeated_by_s); // ... but is free to match KEY_A's pick
}

static void test_pan() {
    SoundBank bank;
    bank.samples = {Sample{{1.0f, 1.0f}}}; // unit amplitude both channels, pre-pan
    for (uint16_t code : {KEY_Q, KEY_ESC, KEY_P, KEY_KPENTER, KEY_SPACE})
        bank.press[code] = {0, 1};
    for (uint16_t code : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}) {
        bank.press[code] = {0, 1};
        bank.pan[code] = key_pan(code);
        assert(bank.pan[code] == 0.0f);
    }
    bank.pan[KEY_Q] = key_pan(KEY_Q);
    bank.pan[KEY_ESC] = key_pan(KEY_ESC);
    bank.pan[KEY_P] = key_pan(KEY_P);
    bank.pan[KEY_KPENTER] = key_pan(KEY_KPENTER);
    bank.pan[KEY_SPACE] = key_pan(KEY_SPACE);
    assert(bank.pan[KEY_Q] < 0 && bank.pan[KEY_ESC] < 0);
    assert(bank.pan[KEY_P] > 0 && bank.pan[KEY_KPENTER] > 0);

    // Laptop-centered layout (main block alone == [0, 1]): Space near
    // center, Q/P clearly off-center, Enter (main-block right edge) clearly
    // right, and nav/numpad progressively further right than the main block.
    assert(std::abs(key_pan(KEY_SPACE)) < 0.1f);
    assert(key_pan(KEY_Q) < -0.25f);
    assert(key_pan(KEY_P) > 0.15f);
    assert(key_pan(KEY_ENTER) > 0.3f);
    assert(key_pan(KEY_KP5) > key_pan(KEY_UP));       // numpad > nav cluster
    assert(key_pan(KEY_UP) > key_pan(KEY_BACKSPACE)); // nav cluster > main-block right edge

    SampleMixer mixer(bank, fixed_gain);
    mixer.set_master_gain(0.5f); // isolate the pan law from full-scale output clipping
    std::array<float, 2> out{};

    for (uint16_t left_key : {KEY_Q, KEY_ESC}) {
        mixer.reset();
        assert(press(mixer, left_key) == TriggerResult::Started);
        mixer.mix(out.data(), 1);
        assert(out[0] > out[1]); // more energy left
        assert(out[0] == kVoiceGain); // never boosted past the trigger's own gain
    }
    for (uint16_t right_key : {KEY_P, KEY_KPENTER}) {
        mixer.reset();
        assert(press(mixer, right_key) == TriggerResult::Started);
        mixer.mix(out.data(), 1);
        assert(out[1] > out[0]); // more energy right
        assert(out[1] == kVoiceGain);
    }

    mixer.reset();
    assert(press(mixer, KEY_SPACE) == TriggerResult::Started);
    mixer.mix(out.data(), 1);
    const float center_diff = std::abs(out[0] - out[1]);
    mixer.reset();
    assert(press(mixer, KEY_Q) == TriggerResult::Started);
    mixer.mix(out.data(), 1);
    const float edge_diff = std::abs(out[0] - out[1]);
    assert(center_diff < edge_diff); // "roughly centered" = far closer than a hard-panned key

    mixer.reset();
    mixer.set_stereo_width(0.0f);
    assert(press(mixer, KEY_Q) == TriggerResult::Started);
    mixer.mix(out.data(), 1);
    assert(out[0] == out[1]); // width 0 -> identical channels regardless of key

    mixer.set_stereo_width(5.0f);
    assert(mixer.stereo_width() == 2.0f); // clamped
    mixer.set_stereo_width(-1.0f);
    assert(mixer.stereo_width() == 0.0f);
}

static void test_bank_validation() {
    SoundBank bank;
    bank.samples = {Sample{{0.1f, 0.1f}}};
    bank.press[KEY_A] = {0, 2}; // count runs past samples.size()
    assert(!validate_sound_bank(bank));
    bank.press[KEY_A] = {5, 1}; // first itself out of bounds
    assert(!validate_sound_bank(bank));
    bank.press[KEY_A] = {0, 1};
    assert(validate_sound_bank(bank));
    bank.release[KEY_A] = {1, 1}; // release range out of bounds
    assert(!validate_sound_bank(bank));
    bank.release[KEY_A] = {};
    bank.pan[KEY_A] = 1.5f;
    assert(!validate_sound_bank(bank));
    bank.pan[KEY_A] = std::numeric_limits<float>::quiet_NaN();
    assert(!validate_sound_bank(bank));
    bank.pan[KEY_A] = -1.0f; // boundary: valid
    assert(validate_sound_bank(bank));
    bank.samples[0].pcm = {2.0f, 0.0f}; // out of [-1, 1]
    assert(!validate_sound_bank(bank));

    bool rejected = false;
    try { SampleMixer invalid(std::move(bank)); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}

static void test_default_bank_loading() {
    auto bank = load_default_bank(KEEBY_ASSET_DIR);
    assert(bank);
    assert(bank->samples.size() == 9); // click*3 + click_up(deep)*3 + release*3
    assert(bank->press[0].count == 0); // KEY_RESERVED stays silent
    assert(bank->press[KEY_A].count == 3 && bank->press[KEY_SPACE].count == 3);
    assert(bank->press[KEY_A].first != bank->press[KEY_SPACE].first); // click vs deep
    for (uint16_t big : {KEY_SPACE, KEY_ENTER, KEY_KPENTER, KEY_BACKSPACE})
        assert(bank->press[big].first == bank->press[KEY_SPACE].first);
    for (std::size_t code = 1; code < kKeyCodeCount; ++code) {
        if (!is_supported_input_code(static_cast<uint16_t>(code))) {
            assert(bank->press[code].count == 0 && bank->release[code].count == 0);
            continue;
        }
        assert(bank->release[code].count == 3);
        assert(bank->release[code].first == bank->release[KEY_A].first); // shared placeholder range
    }
    assert(bank->pan[KEY_Q] < 0 && bank->pan[KEY_P] > 0);
    assert(std::abs(bank->pan[KEY_SPACE]) < std::abs(bank->pan[KEY_Q]));
    assert(validate_sound_bank(*bank));

    assert(!load_default_bank("/no/such/keeby-assets"));
    char directory[] = "/tmp/keeby-bank-test-XXXXXX";
    assert(mkdtemp(directory));
    for (const auto& file : std::filesystem::directory_iterator(KEEBY_ASSET_DIR))
        if (file.path().extension() == ".wav")
            std::filesystem::copy_file(file.path(), std::filesystem::path(directory) / file.path().filename());
    assert(load_default_bank(directory));
    std::filesystem::resize_file(std::filesystem::path(directory) / "release_high.wav", 8);
    assert(!load_default_bank(directory)); // later file failure rejects the whole bank
    std::filesystem::remove_all(directory);
}

static void test_press_release_and_ignored() {
    auto loaded = load_default_bank(KEEBY_ASSET_DIR);
    assert(loaded);
    SampleMixer mixer(std::move(*loaded));
    static constexpr uint16_t kBroadKeys[] = {
        KEY_ESC, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
        KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
        KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P,
        KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L,
        KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M,
        KEY_GRAVE, KEY_MINUS, KEY_EQUAL, KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_SEMICOLON,
        KEY_APOSTROPHE, KEY_BACKSLASH, KEY_COMMA, KEY_DOT, KEY_SLASH,
        KEY_TAB, KEY_CAPSLOCK, KEY_LEFTSHIFT, KEY_RIGHTSHIFT, KEY_LEFTCTRL, KEY_RIGHTCTRL,
        KEY_LEFTALT, KEY_RIGHTALT, KEY_LEFTMETA, KEY_RIGHTMETA, KEY_COMPOSE,
        KEY_SPACE, KEY_ENTER, KEY_BACKSPACE,
        KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN,
        KEY_INSERT, KEY_DELETE, KEY_HOME, KEY_END, KEY_PAGEUP, KEY_PAGEDOWN,
        KEY_SYSRQ, KEY_SCROLLLOCK, KEY_PAUSE,
        KEY_NUMLOCK, KEY_KP0, KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP4, KEY_KP5, KEY_KP6, KEY_KP7, KEY_KP8, KEY_KP9,
        KEY_KPDOT, KEY_KPSLASH, KEY_KPASTERISK, KEY_KPMINUS, KEY_KPPLUS, KEY_KPENTER,
    };
    std::array<float, kMaxFramesPerCallback * kNativeChannels> scratch;
    for (uint16_t code : kBroadKeys) {
        assert(mixer.handle_event({code, KeyEventKind::Down, 0}) == TriggerResult::Started);
        mixer.mix(scratch.data(), kMaxFramesPerCallback); // fully drain before the next check
        assert(mixer.handle_event({code, KeyEventKind::Up, 0}) == TriggerResult::Started);
        mixer.mix(scratch.data(), kMaxFramesPerCallback);
        assert(mixer.handle_event({code, KeyEventKind::Repeat, 0}) == TriggerResult::Ignored);
    }

    // Boundary codes: ignored, no out-of-range array access (ASAN covers this).
    assert(mixer.handle_event({0, KeyEventKind::Down, 0}) == TriggerResult::Ignored);
    assert(mixer.handle_event({0, KeyEventKind::Up, 0}) == TriggerResult::Ignored);
    assert(mixer.handle_event({256, KeyEventKind::Down, 0}) == TriggerResult::Ignored);
    assert(mixer.handle_event({271, KeyEventKind::Down, 0}) == TriggerResult::Ignored);
    assert(mixer.handle_event({275, KeyEventKind::Down, 0}) == TriggerResult::Ignored);
    assert(mixer.handle_event({463, KeyEventKind::Down, 0}) == TriggerResult::Ignored);
    assert(mixer.handle_event({BTN_SIDE, KeyEventKind::Down, 0}) == TriggerResult::Ignored);
    assert(mixer.handle_event({65535, KeyEventKind::Down, 0}) == TriggerResult::Ignored);

    for (uint16_t button : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}) {
        assert(mixer.handle_event({button, KeyEventKind::Down, 0}) == TriggerResult::Started);
        mixer.mix(scratch.data(), kMaxFramesPerCallback);
        assert(mixer.handle_event({button, KeyEventKind::Up, 0}) == TriggerResult::Started);
        mixer.mix(scratch.data(), kMaxFramesPerCallback);
        assert(mixer.handle_event({button, KeyEventKind::Repeat, 0}) == TriggerResult::Ignored);
    }

    assert(mixer.handle_event({KEY_FN, KeyEventKind::Down, 0}) == TriggerResult::Started);
    mixer.mix(scratch.data(), kMaxFramesPerCallback);
    assert(mixer.handle_event({KEY_FN, KeyEventKind::Up, 0}) == TriggerResult::Started);
    mixer.mix(scratch.data(), kMaxFramesPerCallback);
    assert(mixer.handle_event({KEY_FN, KeyEventKind::Repeat, 0}) == TriggerResult::Ignored);

    // Disabled ignores both press and release for a key that just worked.
    mixer.set_enabled(false);
    assert(mixer.handle_event({KEY_A, KeyEventKind::Down, 0}) == TriggerResult::Ignored);
    assert(mixer.handle_event({KEY_A, KeyEventKind::Up, 0}) == TriggerResult::Ignored);
    mixer.set_enabled(true);
    assert(mixer.handle_event({KEY_A, KeyEventKind::Down, 0}) == TriggerResult::Started);
}

static void test_mouse_button_output() {
    SoundBank bank;
    bank.samples = {Sample{{0.1f, 0.1f}}, Sample{{0.2f, 0.2f}}};
    for (uint16_t button : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}) {
        bank.press[button] = {0, 1};
        bank.release[button] = {1, 1};
        bank.pan[button] = key_pan(button);
        assert(bank.pan[button] == 0.0f);
    }
    SampleMixer mixer(std::move(bank), fixed_gain);
    std::array<float, 2> out{};

    for (uint16_t button : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}) {
        assert(mixer.handle_event({button, KeyEventKind::Down, 0}) == TriggerResult::Started);
        mixer.mix(out.data(), 1);
        assert(std::abs(out[0] - 0.1f) < 1e-6f && out[0] == out[1]);
        assert(mixer.handle_event({button, KeyEventKind::Repeat, 0}) == TriggerResult::Ignored);

        assert(mixer.handle_event({button, KeyEventKind::Up, 0}) == TriggerResult::Started);
        mixer.mix(out.data(), 1);
        assert(std::abs(out[0] - 0.2f) < 1e-6f && out[0] == out[1]);
    }
}

static void test_default_bank_output_gain() {
    auto bank = load_default_bank(KEEBY_ASSET_DIR);
    assert(bank);
    bank->press[KEY_A].count = bank->release[KEY_A].count = 1;
    SampleMixer mixer(*bank, fixed_gain);
    mixer.set_stereo_width(0.0f); // compare gain without per-key pan attenuation
    for (auto kind : {KeyEventKind::Down, KeyEventKind::Up}) {
        const auto sound = kind == KeyEventKind::Down ? bank->press[KEY_A] : bank->release[KEY_A];
        const auto& pcm = bank->samples[sound.first].pcm;
        assert(pcm.size() / kNativeChannels <= kMaxFramesPerCallback);
        assert(std::any_of(pcm.begin(), pcm.end(), [](float v) { return v != 0.0f; }));
        std::vector<float> out(pcm.size());
        for (float volume : {0.0f, 0.25f, 1.0f}) {
            mixer.reset();
            mixer.set_master_gain(volume);
            assert(mixer.handle_event({KEY_A, kind, 0}) == TriggerResult::Started);
            mixer.mix(out.data(), static_cast<uint32_t>(pcm.size() / kNativeChannels));
            for (std::size_t i = 0; i < pcm.size(); ++i) {
                const float expected = std::clamp(pcm[i] * kVoiceGain * volume * 2.0f, -kOutputPeak, kOutputPeak);
                assert(std::abs(out[i] - expected) < 1e-6f);
            }
        }
    }
}

static void test_enable_and_master_gain() {
    SampleMixer mixer(one_sample_bank(Sample{{0.4f, -0.4f}}), fixed_gain);
    std::array<float, 2> out{};

    // Disabled: no new voice triggers, but existing state (none touched
    // here) and PRNG/history are left alone, same as any other Ignored event.
    mixer.set_enabled(false);
    assert(!mixer.enabled());
    assert(press(mixer) == TriggerResult::Ignored);
    mixer.mix(out.data(), 1);
    assert(out[0] == 0 && out[1] == 0);

    // Re-enable resumes triggering.
    mixer.set_enabled(true);
    assert(mixer.enabled());
    assert(press(mixer) == TriggerResult::Started);
    mixer.mix(out.data(), 1);
    assert(out[0] == 0.4f && out[1] == -0.4f); // PCM * kVoiceGain * master * 2

    // Master gain zero: voice still triggers (it's a volume knob, not a
    // gate) but produces zero output.
    mixer.set_master_gain(0.0f);
    assert(mixer.master_gain() == 0.0f);
    assert(press(mixer) == TriggerResult::Started);
    mixer.mix(out.data(), 1);
    assert(out[0] == 0 && out[1] == 0);

    // Nominal (non-zero, non-unity) master gain multiplies the existing
    // per-trigger gain, leaving that distribution itself untouched.
    mixer.set_master_gain(0.25f);
    assert(mixer.master_gain() == 0.25f);
    assert(press(mixer) == TriggerResult::Started);
    mixer.mix(out.data(), 1);
    assert(std::abs(out[0] - 0.1f) < 1e-6f && std::abs(out[1] + 0.1f) < 1e-6f); // 0.4*0.5*0.25*2

    // Out-of-range master gain is clamped, not rejected.
    mixer.set_master_gain(3.0f);
    assert(mixer.master_gain() == 1.0f);
    mixer.set_master_gain(-3.0f);
    assert(mixer.master_gain() == 0.0f);
}

// --- Tone filter (Thock/Clack x Warm/Bright) ---

static std::vector<float> make_noise(std::size_t frames, uint32_t seed) {
    std::vector<float> pcm(frames * kNativeChannels);
    uint32_t s = seed == 0 ? 1 : seed;
    for (auto& v : pcm) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        // Low-level input; tone boosts still pass through the final output clamp.
        v = (static_cast<float>(s) / 4294967295.0f - 0.5f) * 0.6f;
    }
    return pcm;
}

// Renders `stereo_in` through a single triggered voice with tone (tx, ty)
// applied. `stereo_in`'s frame count must fit in one mix() call.
static std::vector<float> render_tone(float tx, float ty, const std::vector<float>& stereo_in) {
    SoundBank bank = one_sample_bank(Sample{stereo_in}, KEY_A);
    SampleMixer mixer(std::move(bank), fixed_gain);
    mixer.set_tone(tx, ty);
    std::vector<float> out(stereo_in.size(), 0.0f);
    assert(press(mixer) == TriggerResult::Started);
    mixer.mix(out.data(), static_cast<uint32_t>(stereo_in.size() / kNativeChannels));
    return out;
}

// Crude one-pole low-pass energy (proxy for "low-frequency content").
static double lowpass_energy(const std::vector<float>& stereo) {
    double y = 0.0, acc = 0.0;
    constexpr double a = 0.01;
    for (std::size_t i = 0; i + 1 < stereo.size(); i += kNativeChannels) {
        const double mono = 0.5 * (stereo[i] + stereo[i + 1]);
        y += a * (mono - y);
        acc += y * y;
    }
    return acc;
}

// Crude first-difference energy (proxy for "high-frequency content").
static double highpass_energy(const std::vector<float>& stereo) {
    double acc = 0.0, prev = 0.0;
    for (std::size_t i = 0; i + 1 < stereo.size(); i += kNativeChannels) {
        const double mono = 0.5 * (stereo[i] + stereo[i + 1]);
        const double d = mono - prev;
        acc += d * d;
        prev = mono;
    }
    return acc;
}

static void test_tone_default_and_clamping() {
    SoundBank bank = one_sample_bank(Sample{{0.1f, -0.1f}}, KEY_A);
    SampleMixer mixer(std::move(bank), fixed_gain);
    assert(mixer.tone().first == 0.0f && mixer.tone().second == 0.0f);
    mixer.set_tone(5.0f, -5.0f); // out of range: clamped, not rejected
    assert(mixer.tone().first == 1.0f && mixer.tone().second == -1.0f);
}

static void test_tone_bypass_exact() {
    const auto noise = make_noise(2000, 12345);
    const auto never_touched = render_tone(0.0f, 0.0f, noise);

    // Explicitly setting a non-zero tone and then setting it back to (0, 0)
    // must be bit-identical to never having called set_tone() at all --
    // mix() checks ToneCoeffs::bypass, not "did anyone ever call set_tone".
    SoundBank bank = one_sample_bank(Sample{noise}, KEY_A);
    SampleMixer mixer(std::move(bank), fixed_gain);
    mixer.set_tone(0.7f, -0.3f);
    mixer.set_tone(0.0f, 0.0f);
    std::vector<float> out(noise.size(), 0.0f);
    assert(press(mixer) == TriggerResult::Started);
    mixer.mix(out.data(), static_cast<uint32_t>(noise.size() / kNativeChannels));
    assert(out == never_touched);
}

static void test_tone_response_sanity() {
    const auto noise = make_noise(4000, 987654321u);
    const auto flat = render_tone(0.0f, 0.0f, noise);
    const auto thock = render_tone(-1.0f, 0.0f, noise);  // Thock: boost low, cut high
    const auto clack = render_tone(1.0f, 0.0f, noise);   // Clack: the reverse
    const auto bright = render_tone(0.0f, 1.0f, noise);  // Bright: boost high only

    const double flat_low = lowpass_energy(flat);
    const double flat_high = highpass_energy(flat);
    assert(lowpass_energy(thock) > flat_low);
    assert(highpass_energy(thock) < flat_high);
    assert(lowpass_energy(clack) < flat_low);
    assert(highpass_energy(clack) > flat_high);
    assert(highpass_energy(bright) > flat_high);
}

// TSAN target: a control thread republishing tone coefficients while an
// "RT" thread concurrently triggers and mixes. Passing here (run under
// -fsanitize=thread) is the actual assertion; the data asserts below just
// confirm nothing crashed/misbehaved functionally either.
static void test_tone_concurrent_publish() {
    SoundBank bank = one_sample_bank(Sample{{0.1f, -0.1f, 0.2f, -0.2f}}, KEY_A);
    SampleMixer mixer(std::move(bank), fixed_gain);
    // Seeded before the writer starts: on a slow/loaded box the writer
    // thread may not get scheduled at all during the 2000-iteration mixing
    // loop below, and tone() must still land on one of the two published
    // values (never the untouched default 0) regardless of that race.
    mixer.set_tone(-1.0f, 1.0f);

    std::atomic<bool> stop{false};
    std::thread writer([&] {
        float t = -1.0f;
        while (!stop.load(std::memory_order_relaxed)) {
            mixer.set_tone(t, -t);
            t = -t;
        }
    });

    std::array<float, 8> out{};
    for (int i = 0; i < 2000; ++i) {
        press(mixer);
        mixer.mix(out.data(), 4);
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    assert(mixer.tone().first == 1.0f || mixer.tone().first == -1.0f); // always one of the two published values
}

int main() {
    test_mix();
    test_pool_and_clamp();
    test_loading();
    test_variation();
    test_per_key_variant_history();
    test_pan();
    test_bank_validation();
    test_default_bank_loading();
    test_press_release_and_ignored();
    test_default_bank_output_gain();
    test_mouse_button_output();
    test_enable_and_master_gain();
    test_tone_default_and_clamping();
    test_tone_bypass_exact();
    test_tone_response_sanity();
    test_tone_concurrent_publish();
    std::puts("sample_mixer_test: OK (silence, stereo sum, overlap, advancement, reuse, retrigger, 32-voice drop-newest, clamp, WAV validation)");
    std::puts("sound_engine_v2: OK (generic SoundBank, press+release, per-key variant history, pan/width balance law, bank validation, default-bank loading, broad key coverage)");
    std::puts("desktop_controls: OK (enable/disable suppresses/resumes triggering, master gain and stereo width multiply/clamp correctly)");
}
