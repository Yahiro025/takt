#include "sample_mixer.hpp"

#include <linux/input-event-codes.h>
#include <sndfile.h>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <unistd.h>

using namespace keeby;

static TriggerResult press(SampleMixer& mixer, uint16_t code = KEY_A) {
    return mixer.handle_event({code, KeyEventKind::Down, 0});
}

static void test_mix() {
    SampleBank bank{Sample{{0.2f, -0.4f, 0.6f, -0.8f, 1.0f, -1.0f}},
                    Sample{{-0.2f, 0.4f}}};
    SampleMixer mixer(std::move(bank));
    std::array<float, 8> out;
    out.fill(99);
    mixer.mix(out.data(), 4);
    for (auto x : out) assert(x == 0);
    assert(mixer.handle_event({KEY_A, KeyEventKind::Repeat, 0}) == TriggerResult::Ignored);
    assert(mixer.handle_event({KEY_A, KeyEventKind::Up, 0}) == TriggerResult::Ignored);
    assert(press(mixer, KEY_B) == TriggerResult::Ignored);
    mixer.mix(out.data(), 1);
    assert(out[0] == 0 && out[1] == 0);

    assert(press(mixer) == TriggerResult::Started);
    mixer.mix(out.data(), 0); // no advancement
    mixer.mix(out.data(), 1);
    assert(out[0] == 0.1f && out[1] == -0.2f);
    assert(press(mixer, KEY_SPACE) == TriggerResult::Started);
    mixer.mix(out.data(), 1); // second frame + first frame, separate cursors
    assert(std::abs(out[0] - 0.2f) < 1e-6f && out[1] == -0.2f);
    mixer.mix(out.data(), 2);
    assert(out[0] == 0.5f && out[1] == -0.5f);
    assert(out[2] == 0 && out[3] == 0); // retired mid-buffer
    assert(press(mixer, KEY_ENTER) == TriggerResult::Started);
    mixer.mix(out.data(), 2); // reuse shorter sample: no old tail/cursor
    assert(out[0] == -0.1f && out[1] == 0.2f);
    assert(out[2] == 0 && out[3] == 0);

    assert(press(mixer) == TriggerResult::Started);
    mixer.mix(out.data(), 1);
    assert(press(mixer, KEY_S) == TriggerResult::Started); // same sample, overlaps
    mixer.mix(out.data(), 1);
    assert(out[0] == 0.4f && std::abs(out[1] + 0.6f) < 1e-6f);
    mixer.reset();
    mixer.mix(out.data(), 4);
    for (auto x : out) assert(x == 0);
    assert(press(mixer, KEY_D) == TriggerResult::Started);
    mixer.mix(out.data(), 1);
    assert(out[0] == 0.1f && out[1] == -0.2f);
}

static void test_pool_and_clamp() {
    SampleMixer mixer({Sample{{0.01f, -0.01f, 0.02f, -0.02f}}, Sample{{1, 1}}});
    for (std::size_t i = 0; i < kVoicePoolSize; ++i)
        assert(press(mixer) == TriggerResult::Started);
    assert(press(mixer, KEY_SPACE) == TriggerResult::PoolFull);
    std::array<float, 4> out;
    mixer.mix(out.data(), 2);
    assert(std::abs(out[0] - 0.16f) < 1e-6f && std::abs(out[1] + 0.16f) < 1e-6f);
    assert(std::abs(out[2] - 0.32f) < 1e-6f && std::abs(out[3] + 0.32f) < 1e-6f);
    // Full pool retired; every slot, not just the first, becomes reusable.
    for (std::size_t i = 0; i < kVoicePoolSize; ++i)
        assert(press(mixer, KEY_SPACE) == TriggerResult::Started);
    mixer.mix(out.data(), 2);
    assert(out[0] == kOutputPeak && out[1] == kOutputPeak);
    assert(out[2] == 0 && out[3] == 0);
    for (int i = 0; i < 1000; ++i) {
        assert(press(mixer) == TriggerResult::Started);
        mixer.mix(out.data(), 2);
        assert(out[0] == 0.005f && out[3] == -0.01f);
    }
    SampleMixer cancellation({Sample{{1, -1}}, Sample{{-1, 1}}});
    for (int i = 0; i < 16; ++i) assert(press(cancellation) == TriggerResult::Started);
    for (int i = 0; i < 16; ++i) assert(press(cancellation, KEY_SPACE) == TriggerResult::Started);
    cancellation.mix(out.data(), 1);
    assert(out[0] == 0 && out[1] == 0); // clamp only AFTER all voices
    for (int i = 0; i < 32; ++i) assert(press(cancellation) == TriggerResult::Started);
    cancellation.mix(out.data(), 1);
    assert(out[0] == kOutputPeak && out[1] == -kOutputPeak);
}

static void test_loading() {
    for (auto name : {"click_down.wav", "click_up.wav"}) {
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
        try { SampleMixer invalid({Sample{{bad, 0}}, Sample{}}); }
        catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
    bool rejected = false;
    try { SampleMixer invalid({Sample{{0}}, Sample{}}); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    std::filesystem::resize_file(path, 8); // malformed/truncated container
    assert(!load_wav_sample(path));
    std::filesystem::remove_all(directory);
}

int main() {
    test_mix();
    test_pool_and_clamp();
    test_loading();
    std::puts("sample_mixer_test: OK (silence, stereo sum, overlap, advancement, reuse, retrigger, 32-voice drop-newest, clamp, WAV validation)");
}
