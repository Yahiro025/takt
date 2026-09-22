#include "audio_boundary.hpp"

#include <pipewire/pipewire.h>
#include <linux/input-event-codes.h>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>

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
        keeby::AudioBoundary audio({keeby::Sample{{0.1f, -0.1f, 0.2f, -0.2f}},
                                   keeby::Sample{{0.6f, -0.6f}}});
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
        assert(samples[0] == 0.3f && samples[1] == -0.3f);
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
        assert(samples[0] == 0.05f && samples[1] == -0.05f);
        audio.stop();
        assert(audio.start(events));
        keeby::AudioBoundary::on_process(&audio);
        assert(samples[0] == 0 && samples[1] == 0); // stop/restart resets voices
        audio.stop();
        available = nullptr;
    }
    std::puts("audio_boundary_test: OK (budget, deferred FIFO, buffers, lifecycle failures, mapped PCM, voice drops, repeat/release, frame cap, restart)");
}
