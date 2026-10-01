#include "audio_boundary.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace keeby {
namespace {

constexpr pw_stream_events kStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = &AudioBoundary::on_process,
};

} // namespace

AudioBoundary::AudioBoundary(SoundBank samples, MixerVariation variation)
    : mixer_(std::move(samples), variation) {
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
    // RT thread is confirmed stopped now (pw_thread_loop_stop above already
    // joined it, or it was never started): apply any still-pending swap
    // synchronously -- same as swap_bank()'s own "not started" branch --
    // then free every retired bank EXCEPT the one the mixer now actually
    // points to. Freeing that one would leave bank_/active_bank_ dangling
    // across a restart (start()/stop()/start() is supported; see
    // tests/engine_controller_test.cpp), the same class of bug fix 2 in the
    // Step 2.7 review fixed for swap_bank() itself.
    mixer_.sync_bank();
    const SoundBank* active_at_stop = mixer_.active_bank();
    std::erase_if(retired_, [&](const std::unique_ptr<const SoundBank>& b) { return b.get() != active_at_stop; });
    transport_ = nullptr;
    started_ = false;
}

void AudioBoundary::swap_bank(std::unique_ptr<const SoundBank> bank) {
    const SoundBank* raw = bank.get();
    retired_.push_back(std::move(bank)); // keep alive at least until proven safe to drop, below
    mixer_.request_bank(raw);
    if (started_) {
        // Bounded wait for the RT thread's acknowledgement (see sample_mixer.cpp's
        // sync_bank(), called once per mix() callback). A timeout is not an error:
        // see the pruning comment below for what happens then.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (mixer_.active_bank() != raw && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } else {
        // No RT thread running at all: safe to switch synchronously, no wait needed.
        mixer_.sync_bank();
    }
    // Only free anything once active_bank() == raw, i.e. the RT thread has
    // acknowledged THIS exact request. requested_bank_/active_bank_ are a
    // single writer (control thread) / single reader (RT thread) pair, and
    // C++'s coherence rule for a single atomic object guarantees the RT
    // thread's successive loads of requested_bank_ can never go backwards
    // in publish order -- so once it has settled on `raw`, it can never
    // later settle on anything OLDER, meaning every OTHER retired bank
    // (including ones left over from earlier timed-out swaps) is now
    // provably unreachable and safe to free.
    //
    // On a timeout, active_bank() may still be an older, not-yet-loaded
    // request -- NOT necessarily `raw` itself, and not necessarily anything
    // already in `retired_` either. Freeing anything here (even entries
    // that are neither `raw` nor the current active_bank()) risks freeing a
    // bank the RT thread hasn't reached yet but will: it does not skip
    // straight to the latest request, it only ever loads whatever
    // requested_bank_ holds AT THE TIME of its next sync_bank() call, which
    // could still be an intermediate one if this thread published several
    // requests in quick succession. So on timeout we free nothing at all;
    // pruning happens on a later successful (acknowledged) swap, or
    // unconditionally in stop() once the RT thread is confirmed gone.
    if (mixer_.active_bank() == raw)
        std::erase_if(retired_, [&](const std::unique_ptr<const SoundBank>& b) { return b.get() != raw; });
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
// variation:                 <=64 xorshift32 steps + <=32 variant/gain selections
// desktop control state:     <=32 relaxed atomic loads of enabled_ (one per accepted
//                             event, inside handle_event) + <=32 relaxed atomic loads
//                             of stereo_width_ (one per accepted trigger, inside
//                             SampleMixer::trigger) + 1 relaxed atomic load of
//                             master_gain_ per callback (inside mix()); all are plain
//                             std::atomic reads/writes, never a mutex; no GUI/D-Bus call
//                             is ever reachable from here (see tray_service.cpp, which
//                             only ever writes those same atomics from its own thread).
// bank swap (Step 2.7):      1 acquire load of requested_bank_ per callback (inside
//                             mix()'s sync_bank()); on a change (rare): one bulk assignment
//                             of the fixed 32-voice array plus two fixed 256-byte history
//                             arrays (no allocation), then one release store of
//                             active_bank_. swap_bank() itself (allocation, sleep_for,
//                             freeing old banks) runs only on the EngineController/
//                             AudioBoundary owner thread, never here.
// See docs/004-step-2.3-sound-fidelity.md, docs/005-step-2.4-desktop-shell.md, and
// docs/008-step-2.7-sound-packs.md for the complete bound, atomic, and swap-handshake
// inventory.
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
