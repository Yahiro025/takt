#include "audio_boundary.hpp"

#include <pipewire/pipewire.h>
#include <linux/input-event-codes.h>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

// Replace only stream I/O and selected failure points. The production
// start/stop/process bodies run unchanged; no server or keyboard is needed.
namespace {
int stream_token;
pw_buffer* available = nullptr;
int submitted = 0, destroyed = 0, loops_destroyed = 0, contexts_destroyed = 0;
bool fail_connect = false, fail_loop_start = false;
}
extern "C" {
pw_stream* __wrap_pw_stream_new_simple(pw_loop*, const char*, pw_properties* props,
                                       const pw_stream_events*, void*) {
    pw_properties_free(props);
    return reinterpret_cast<pw_stream*>(&stream_token);
}
int __wrap_pw_stream_connect(pw_stream*, pw_direction, uint32_t, pw_stream_flags,
                             const spa_pod**, uint32_t) { return fail_connect ? -EIO : 0; }
void __wrap_pw_stream_destroy(pw_stream*) { ++destroyed; }
pw_buffer* __wrap_pw_stream_dequeue_buffer(pw_stream*) { return available; }
int __wrap_pw_stream_queue_buffer(pw_stream*, pw_buffer* b) {
    assert(b == available);
    ++submitted;
    return 0;
}
int __real_pw_thread_loop_start(pw_thread_loop*);
int __wrap_pw_thread_loop_start(pw_thread_loop* loop) {
    return fail_loop_start ? -EIO : __real_pw_thread_loop_start(loop);
}
void __real_pw_thread_loop_destroy(pw_thread_loop*);
void __wrap_pw_thread_loop_destroy(pw_thread_loop* loop) {
    ++loops_destroyed;
    __real_pw_thread_loop_destroy(loop);
}
void __real_pw_context_destroy(pw_context*);
void __wrap_pw_context_destroy(pw_context* context) {
    ++contexts_destroyed;
    __real_pw_context_destroy(context);
}
}

int main() {
    keeby::KeyEventTransport transport;
    {
        keeby::AudioBoundary audio;
        assert(audio.start(transport));
        assert(!audio.start(transport)); // no second consumer
        for (uint64_t i = 0; i < transport.capacity(); ++i)
            assert(transport.try_push({static_cast<uint16_t>(i), keeby::KeyEventKind::Down, i}));

        keeby::AudioBoundary::on_process(&audio); // no buffer: no consumption
        assert(audio.counters().consumed_total == 0);
        std::array<float, 256> samples;
        samples.fill(1.0f);
        spa_chunk chunk{};
        spa_data data{};
        data.data = samples.data();
        data.maxsize = sizeof(samples);
        data.chunk = &chunk;
        spa_buffer buffer{};
        buffer.n_datas = 1;
        buffer.datas = &data;
        pw_buffer output{};
        output.buffer = &buffer;
        // Must compare at full width before narrowing to uint32_t.
        output.requested = (uint64_t{1} << 32) + 1;
        available = &output;
        keeby::AudioBoundary::on_process(&audio);
        assert(audio.counters().consumed_total == 32);
        assert(audio.counters().last_code == 31);
        assert(audio.counters().budget_exhausted == 1);
        assert(chunk.size == sizeof(samples));
        for (float sample : samples) assert(sample == 0.0f);
        // Refill the 32 freed slots. Each later callback still consumes <=32.
        for (uint64_t i = 255; i < 287; ++i)
            assert(transport.try_push({static_cast<uint16_t>(i), keeby::KeyEventKind::Down, i}));
        for (int i = 0; i < 8; ++i) {
            const auto before = audio.counters().consumed_total.load();
            keeby::AudioBoundary::on_process(&audio);
            assert(audio.counters().consumed_total - before <= 32);
        }
        assert(audio.counters().consumed_total == 287);
        assert(audio.counters().presses == 287);
        assert(audio.counters().last_code == 286);
        assert(transport.dropped_count() == 0); // budget deferred, never discarded
        assert(audio.counters().budget_exhausted == 8);
        buffer.n_datas = 0;
        keeby::AudioBoundary::on_process(&audio); // invalid buffer safely returned
        assert(submitted == 10);
        audio.stop();
        audio.stop();
    }
    assert(destroyed == 1 && loops_destroyed == 1 && contexts_destroyed == 1);
    for (int failure = 0; failure < 2; ++failure) {
        fail_connect = failure == 0;
        fail_loop_start = failure == 1;
        {
            keeby::AudioBoundary audio;
            assert(!audio.start(transport));
        } // destructor must clean partial startup, too
        assert(destroyed == failure + 2);
        assert(loops_destroyed == failure + 2);
        assert(contexts_destroyed == failure + 2);
    }
    fail_connect = fail_loop_start = false;
    {
        keeby::KeyEventTransport events;
        // KEY_A gets no release range: this block's later Repeat/Up checks
        // deliberately reuse the pre-Step-2.6 "never triggers" expectation.
        keeby::SoundBank bank;
        bank.samples = {keeby::Sample{{0.1f, -0.1f, 0.2f, -0.2f}}, keeby::Sample{{0.6f, -0.6f}}};
        bank.press[KEY_A] = {0, 1};
        bank.press[KEY_ENTER] = {1, 1};
        keeby::AudioBoundary audio(std::move(bank),
                                  {.min_gain = keeby::kVoiceGain, .max_gain = keeby::kVoiceGain});
        assert(audio.start(events));
        std::array<float, (keeby::kMaxFramesPerCallback + 1) * 2> samples;
        samples.fill(99);
        spa_chunk chunk{};
        spa_data data{};
        data.data = samples.data();
        data.maxsize = sizeof(samples);
        data.chunk = &chunk;
        spa_buffer buffer{};
        buffer.n_datas = 1;
        buffer.datas = &data;
        pw_buffer output{};
        output.buffer = &buffer;
        output.requested = 1;
        available = &output;
        for (int i = 0; i < 35; ++i)
            assert(events.try_push({KEY_A, keeby::KeyEventKind::Down, 0}));
        keeby::AudioBoundary::on_process(&audio);
        assert(audio.counters().consumed_total == 32);
        assert(audio.counters().voices_started == 32);
        assert(samples[0] == keeby::kOutputPeak && samples[1] == -keeby::kOutputPeak);
        assert(samples[2] == 99 && chunk.size == 2 * sizeof(float));
        keeby::AudioBoundary::on_process(&audio);
        assert(audio.counters().consumed_total == 35);
        assert(audio.counters().voices_dropped == 3); // separate from transport drops
        assert(audio.counters().voices_started == 32);
        assert(events.dropped_count() == 0);
        assert(samples[0] == keeby::kOutputPeak && samples[1] == -keeby::kOutputPeak);
        assert(events.try_push({KEY_A, keeby::KeyEventKind::Repeat, 0}));
        assert(events.try_push({KEY_A, keeby::KeyEventKind::Up, 0}));
        keeby::AudioBoundary::on_process(&audio);
        assert(audio.counters().repeats == 1 && audio.counters().releases == 1);
        assert(audio.counters().consumed_total == 37);
        assert(audio.counters().voices_started == 32);
        assert(samples[0] == 0 && samples[1] == 0);
        assert(events.try_push({KEY_ENTER, keeby::KeyEventKind::Down, 0}));
        keeby::AudioBoundary::on_process(&audio);
        assert(samples[0] == 0.6f && samples[1] == -0.6f);
        assert(audio.counters().voices_started == 33);

        output.requested = 0; // server offers >4096 frames; fixed work cap wins
        keeby::AudioBoundary::on_process(&audio);
        assert(chunk.size == keeby::kMaxFramesPerCallback * 2 * sizeof(float));
        for (std::size_t i = 0; i < keeby::kMaxFramesPerCallback * 2; ++i)
            assert(samples[i] == 0);
        assert(samples[keeby::kMaxFramesPerCallback * 2] == 99);
        assert(samples.back() == 99);
        output.requested = 1;
        assert(events.try_push({KEY_A, keeby::KeyEventKind::Down, 0}));
        keeby::AudioBoundary::on_process(&audio); // leaves one frame in a voice
        assert(samples[0] == 0.1f && samples[1] == -0.1f);
        audio.stop();
        assert(audio.start(events));
        keeby::AudioBoundary::on_process(&audio);
        assert(samples[0] == 0 && samples[1] == 0); // stop/restart resets voices
        audio.stop();
        available = nullptr;
    }
    {
        keeby::SoundBank bank;
        for (std::size_t v = 0; v < 3; ++v)
            bank.samples.push_back(keeby::Sample{
                std::vector<float>((keeby::kMaxFramesPerCallback + 1) * 2, 0.001f * (v + 1))});
        bank.press[KEY_A] = {0, 3};
        keeby::SampleMixer reference(bank);
        keeby::AudioBoundary audio(std::move(bank));
        keeby::KeyEventTransport events;
        assert(audio.start(events));
        std::array<float, (keeby::kMaxFramesPerCallback + 1) * 2> samples;
        samples.fill(99);
        spa_chunk chunk{};
        spa_data data{};
        data.data = samples.data(); data.maxsize = sizeof(samples); data.chunk = &chunk;
        spa_buffer buffer{};
        buffer.n_datas = 1; buffer.datas = &data;
        pw_buffer output{};
        output.buffer = &buffer;
        available = &output;
        for (int i = 0; i < 33; ++i)
            assert(events.try_push({KEY_A, keeby::KeyEventKind::Down, 0}));
        keeby::AudioBoundary::on_process(&audio);
        assert(audio.counters().consumed_total == 32 && audio.counters().voices_started == 32);
        assert(audio.counters().budget_exhausted == 1);
        assert(chunk.size == keeby::kMaxFramesPerCallback * 2 * sizeof(float));
        const auto sum = samples[0];
        assert(sum > 0 && sum < keeby::kOutputPeak);
        for (std::size_t i = 0; i < keeby::kMaxFramesPerCallback * 2; ++i) assert(samples[i] == sum);
        assert(samples.back() == 99); // cap applies with all 32 varied voices active
        keeby::AudioBoundary::on_process(&audio);
        assert(audio.counters().consumed_total == 33 && audio.counters().voices_dropped == 1);
        assert(events.dropped_count() == 0);
        assert(samples[0] == sum && samples[1] == sum); // exact last frame
        for (std::size_t i = 2; i < keeby::kMaxFramesPerCallback * 2; ++i) assert(samples[i] == 0);
        audio.stop();
        assert(audio.start(events));
        assert(events.try_push({KEY_A, keeby::KeyEventKind::Down, 0}));
        assert(reference.handle_event({KEY_A, keeby::KeyEventKind::Down, 0}) == keeby::TriggerResult::Started);
        std::array<float, 2> expected;
        reference.mix(expected.data(), 1);
        output.requested = 1;
        keeby::AudioBoundary::on_process(&audio);
        assert(samples[0] == expected[0] && samples[1] == expected[1]); // seed/history reset
        audio.stop();
        available = nullptr;
    }
    { // Step 2.7: swap_bank() while NOT started must free immediately, never wait
        auto make_bank = [](float v) {
            keeby::SoundBank bank;
            bank.samples.push_back(keeby::Sample{{v, -v, v, -v}});
            bank.press[KEY_A] = {0, 1};
            return std::make_unique<const keeby::SoundBank>(std::move(bank));
        };
        keeby::AudioBoundary audio;
        const auto start = std::chrono::steady_clock::now();
        audio.swap_bank(make_bank(0.2f));
        audio.swap_bank(make_bank(0.3f));
        const auto elapsed = std::chrono::steady_clock::now() - start;
        // The "started" path polls for an ack for up to 500ms; two of those
        // would take >= 1s. A stopped-stream swap takes microseconds.
        assert(elapsed < std::chrono::milliseconds(100));
    }
    { // Step 2.7: swap_bank() while started, racing a simulated RT thread
      // that keeps calling on_process() (i.e. mixer_.mix()'s sync_bank()) --
      // exercises the real retired_ list/acknowledgement path, not just
      // SampleMixer's raw atomics (see sound_pack_test's swap safety test).
        auto make_bank = [](float v) {
            keeby::SoundBank bank;
            bank.samples.push_back(keeby::Sample{{v, -v, v, -v}});
            bank.press[KEY_A] = {0, 1};
            return std::make_unique<const keeby::SoundBank>(std::move(bank));
        };
        keeby::KeyEventTransport events;
        keeby::AudioBoundary audio;
        assert(audio.start(events));

        std::array<float, 16> samples{};
        spa_chunk chunk{};
        spa_data data{};
        data.data = samples.data();
        data.maxsize = sizeof(samples);
        data.chunk = &chunk;
        spa_buffer buffer{};
        buffer.n_datas = 1;
        buffer.datas = &data;
        pw_buffer output{};
        output.buffer = &buffer;
        output.requested = 1;
        available = &output;

        std::atomic<bool> stop{false};
        std::thread rt([&] {
            while (!stop.load(std::memory_order_relaxed))
                keeby::AudioBoundary::on_process(&audio);
        });
        for (int i = 0; i < 20; ++i)
            audio.swap_bank(make_bank(0.01f * static_cast<float>(i + 1))); // each call waits for its own ack
        stop.store(true, std::memory_order_relaxed);
        rt.join();
        audio.stop();
        available = nullptr;
    }
    { // Step 2.7 review fix 2: a bank swapped in before stop() must survive a
      // stop()/start() restart. stop() used to unconditionally free every
      // retired bank via retired_.clear(), leaving the mixer's OWN active
      // bank dangling -- a real UAF on the very next trigger+mix after
      // restart, which is exactly what this test drives (would abort under
      // ASAN before the fix).
        auto make_bank = [](float v) {
            keeby::SoundBank bank;
            bank.samples.push_back(keeby::Sample{{v, -v, v, -v}});
            bank.press[KEY_A] = {0, 1};
            return std::make_unique<const keeby::SoundBank>(std::move(bank));
        };
        keeby::KeyEventTransport events;
        // Fixed gain (no randomized range) so the mixed sample is an exact,
        // predictable value -- proof the data came from the swapped-in bank,
        // not zeroed/garbage memory.
        keeby::AudioBoundary audio(keeby::SoundBank{}, {.min_gain = keeby::kVoiceGain, .max_gain = keeby::kVoiceGain});
        assert(audio.start(events));

        std::array<float, 16> samples{};
        spa_chunk chunk{};
        spa_data data{};
        data.data = samples.data();
        data.maxsize = sizeof(samples);
        data.chunk = &chunk;
        spa_buffer buffer{};
        buffer.n_datas = 1;
        buffer.datas = &data;
        pw_buffer output{};
        output.buffer = &buffer;
        output.requested = 1;
        available = &output;

        std::atomic<bool> stop_flag{false};
        std::thread rt([&] {
            while (!stop_flag.load(std::memory_order_relaxed))
                keeby::AudioBoundary::on_process(&audio);
        });
        audio.swap_bank(make_bank(0.4f)); // acknowledged while the RT stand-in is running
        stop_flag.store(true, std::memory_order_relaxed);
        rt.join();

        audio.stop();               // before the fix: freed the bank the mixer still pointed to
        assert(audio.start(events)); // restart after stop is supported (engine_controller_test does this)

        samples.fill(99.0f);
        assert(events.try_push({KEY_A, keeby::KeyEventKind::Down, 0}));
        keeby::AudioBoundary::on_process(&audio); // mixes from the still-owned 0.4f bank, not freed memory
        // make_bank's Sample is {v, -v, v, -v} (see above): R is the negated L.
        assert(samples[0] == 2.0f * keeby::kVoiceGain * 0.4f && samples[1] == -(2.0f * keeby::kVoiceGain * 0.4f));

        audio.stop();
        available = nullptr;
    }
    { // Step 2.7 review fix 1: a timed-out swap frees NOTHING (an
      // unacknowledged request may not even be the latest one the RT thread
      // has seen yet); only a later ACKNOWLEDGED swap prunes everything
      // strictly older, including whatever an earlier timeout left behind.
        auto make_bank = [](float v) {
            keeby::SoundBank bank;
            bank.samples.push_back(keeby::Sample{{v, -v, v, -v}});
            bank.press[KEY_A] = {0, 1};
            return std::make_unique<const keeby::SoundBank>(std::move(bank));
        };
        keeby::KeyEventTransport events;
        keeby::AudioBoundary audio;
        assert(audio.start(events));
        assert(audio.retired_bank_count() == 0);

        // No RT stand-in thread running yet: on_process() never runs, so
        // this swap cannot be acknowledged and must time out (~500ms).
        audio.swap_bank(make_bank(0.1f));
        assert(audio.retired_bank_count() == 1); // timed out: nothing freed

        std::array<float, 16> samples{};
        spa_chunk chunk{};
        spa_data data{};
        data.data = samples.data();
        data.maxsize = sizeof(samples);
        data.chunk = &chunk;
        spa_buffer buffer{};
        buffer.n_datas = 1;
        buffer.datas = &data;
        pw_buffer output{};
        output.buffer = &buffer;
        output.requested = 1;
        available = &output;

        std::atomic<bool> stop_flag{false};
        std::thread rt([&] {
            while (!stop_flag.load(std::memory_order_relaxed))
                keeby::AudioBoundary::on_process(&audio);
        });
        audio.swap_bank(make_bank(0.2f)); // callbacks running now: acknowledged quickly
        stop_flag.store(true, std::memory_order_relaxed);
        rt.join();
        assert(audio.retired_bank_count() == 1); // only the now-active (0.2f) bank remains

        audio.stop();
        available = nullptr;
    }
    std::puts("audio_boundary_test: OK (budget, deferred FIFO, buffers, lifecycle failures, mapped PCM, voice drops, repeat/release, frame cap, restart)");
    std::puts("sound_fidelity boundary: OK (32 varied voices x 4096 frames, deferred event, retirement, seed/history reset)");
    std::puts("sound_pack swap: OK (stopped-stream immediate free, concurrent RT-ack under a real swap_bank/retired_ path, "
               "restart-after-swap survives stop(), timeout frees nothing then prunes on next ack)");
}
