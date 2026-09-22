#include "audio_boundary.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <utility>

namespace keeby {
namespace {

constexpr pw_stream_events kStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = &AudioBoundary::on_process,
};

} // namespace

AudioBoundary::AudioBoundary(SampleBank samples) : mixer_(std::move(samples)) {
    pw_init(nullptr, nullptr);
}

AudioBoundary::~AudioBoundary() {
    stop();
    pw_deinit();
}

std::expected<void, std::string> AudioBoundary::start(KeyEventTransport& transport) {
    if (loop_) return std::unexpected("audio boundary already initialized; stop it first");
    transport_ = &transport;

    loop_ = pw_thread_loop_new("keeby-audio", nullptr);
    if (!loop_) return std::unexpected("pw_thread_loop_new failed");

    pw_thread_loop_lock(loop_);

    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (!context_) {
        pw_thread_loop_unlock(loop_);
        return std::unexpected("pw_context_new failed");
    }

    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Game",
        PW_KEY_NODE_LATENCY, "128/48000",
        nullptr);

    stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(loop_), "keeby", props,
                                    &kStreamEvents, this);
    if (!stream_) {
        pw_thread_loop_unlock(loop_);
        return std::unexpected("pw_stream_new_simple failed");
    }

    uint8_t pod_buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));

    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.channels = kNativeChannels;
    info.rate = kNativeSampleRate;

    const spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    int res = pw_stream_connect(
        stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                      PW_STREAM_FLAG_RT_PROCESS),
        params, 1);

    if (res < 0) {
        std::string err = std::string("pw_stream_connect failed: ") + spa_strerror(res);
        pw_thread_loop_unlock(loop_);
        return std::unexpected(err);
    }

    pw_thread_loop_unlock(loop_);

    if (pw_thread_loop_start(loop_) < 0) {
        return std::unexpected("pw_thread_loop_start failed");
    }

    started_ = true;
    return {};
}

void AudioBoundary::stop() {
    if (loop_) {
        pw_thread_loop_lock(loop_);
        if (stream_) {
            pw_stream_destroy(stream_);
            stream_ = nullptr;
        }
        pw_thread_loop_unlock(loop_);
        if (started_) pw_thread_loop_stop(loop_);
    }
    if (context_) {
        pw_context_destroy(context_);
        context_ = nullptr;
    }
    if (loop_) {
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
    }
    mixer_.reset();
    transport_ = nullptr;
    started_ = false;
}

void AudioBoundary::on_process(void* userdata) noexcept {
    static_cast<AudioBoundary*>(userdata)->process();
}

// --- Real-time safety audit for this function ---
// heap allocation:            none (no new/malloc/std::string/std::vector growth)
// blocking synchronization:   none (no mutex/condvar; transport_ ops are lock-free)
// filesystem access:          none
// console logging:            none (counters_ only; read from a non-RT thread instead)
// blocking channel ops:       none (try_pop is wait-free and bounded)
// unnecessary cloning:        none (KeyEvent is a 16-byte POD copied by value, unavoidable
//                              and cheap for a lock-free SPSC queue)
// panic/exception paths:      none (no throwing operations in this function; PipeWire's C
//                              API reports errors via return codes, not exceptions)
// destruction of large objects: none
// mixer:                     <=32 voice scans per event; <=32*4096*2 sample sums
// external calls:            PipeWire dequeue/queue (documented RT safe), bounded
//                            std::fill_n (compiled to memset); no decoder calls
// See docs/003-step-2.2-sample-mixer.md for the complete bound and atomic inventory.
void AudioBoundary::process() noexcept {
    pw_buffer* b = pw_stream_dequeue_buffer(stream_);
    if (!b) return;

    spa_buffer* buf = b->buffer;
    if (!buf || buf->n_datas == 0 || !buf->datas ||
        !buf->datas[0].data || !buf->datas[0].chunk) {
        pw_stream_queue_buffer(stream_, b);
        return;
    }

    float* dst = static_cast<float*>(buf->datas[0].data);
    uint32_t n_frames = buf->datas[0].maxsize / (sizeof(float) * kNativeChannels);
    if (b->requested != 0 && b->requested < n_frames) {
        n_frames = static_cast<uint32_t>(b->requested);
    }

    // A fixed ceiling also bounds mixing if the server offers a huge buffer.
    // Submit a shorter chunk; advance voices only for frames actually written.
    n_frames = std::min(n_frames, kMaxFramesPerCallback);

    // Drain pending key events, up to kMaxEventsPerCallback. A bare
    // "while (try_pop(...))" is NOT bounded on its own: the transport's
    // finite capacity bounds how much can be queued at one instant, but
    // says nothing about one callback invocation's duration if a
    // concurrently-running producer keeps refilling the queue while
    // this loop drains it — the drain and the refill can interleave
    // indefinitely from this loop's point of view. The explicit counter
    // below is what actually bounds this function's worst-case
    // iteration count, independent of producer behavior.
    KeyEvent ev;
    std::size_t processed = 0;
    while (processed < kMaxEventsPerCallback && transport_->try_pop(ev)) {
        switch (ev.kind) {
            case KeyEventKind::Down: counters_.presses.fetch_add(1, std::memory_order_relaxed); break;
            case KeyEventKind::Up: counters_.releases.fetch_add(1, std::memory_order_relaxed); break;
            case KeyEventKind::Repeat: counters_.repeats.fetch_add(1, std::memory_order_relaxed); break;
        }
        counters_.consumed_total.fetch_add(1, std::memory_order_relaxed);
        counters_.last_code.store(ev.code, std::memory_order_relaxed);
        switch (mixer_.handle_event(ev)) {
            case TriggerResult::Started:
                counters_.voices_started.fetch_add(1, std::memory_order_relaxed); break;
            case TriggerResult::PoolFull:
                counters_.voices_dropped.fetch_add(1, std::memory_order_relaxed); break;
            case TriggerResult::Ignored: break;
        }
        ++processed;
    }
    if (processed == kMaxEventsPerCallback) {
        // Budget reached with the queue possibly still non-empty. Any
        // remaining events are left queued — never dropped for this
        // reason — and will be drained on the next callback instead.
        counters_.budget_exhausted.fetch_add(1, std::memory_order_relaxed);
    }

    mixer_.mix(dst, n_frames);

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = sizeof(float) * kNativeChannels;
    buf->datas[0].chunk->size = n_frames * kNativeChannels * sizeof(float);

    pw_stream_queue_buffer(stream_, b);
}

} // namespace keeby
