#include "sound_pack.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <regex>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <linux/input-event-codes.h>
#include <nlohmann/json.hpp>
#include <samplerate.h>
#include <sndfile.h>

#include "key_layout.hpp"

namespace keeby {
namespace {

namespace fs = std::filesystem;

constexpr std::uintmax_t kMaxConfigBytes = 1024 * 1024;       // config.json <= 1 MiB
constexpr long kMaxSourceSeconds = 600;                       // any one decoded file <= 10 minutes
constexpr std::size_t kMaxPackPcmBytes = 256 * 1024 * 1024;   // whole pack's decoded float PCM <= 256 MiB
constexpr std::size_t kMaxSampleTableSize = 65536;            // KeySound::first must fit uint16_t

// A file's audio decoded to float PCM at its OWN sample rate, mono already
// duplicated to stereo. Not yet resampled to kNativeSampleRate.
struct RawAudio {
    std::vector<float> pcm;
    uint32_t frames = 0;
    uint32_t rate = 0;
    bool wav = false;
};

float catalog_profile_trim(std::string_view id) noexcept {
    constexpr std::pair<std::string_view, float> trims[] = {
        {"aflion-carrot", 0.88f}, {"akko-cs-jelly-black", 0.97f},
        {"akko-v3-pro-cream-yellow", 0.88f}, {"akko-clicky-pink", 0.88f},
        {"akko-piano-pro", 1.30f}, {"alps-skcm-blue", 0.88f},
        {"drop-holy-panda", 0.88f}, {"durock-alpaca", 0.88f},
        {"gateron-ink-black", 0.88f}, {"gateron-ink-red", 0.88f},
        {"gateron-turquoise-tealios", 0.88f}, {"ibm-buckling-spring", 0.88f},
        {"iqunix-mq80", 0.88f}, {"kailh-box-navy", 0.88f},
        {"keychron-k2-max-red", 0.88f}, {"keychron-k2-max-brown", 0.88f},
        {"lizard", 0.88f}, {"lofree-flow-2-surfer", 1.47f},
        {"lofree-flow-2-void", 2.02f}, {"lofree-flow-2-pulse", 1.34f},
        {"novelkeys-cream", 0.88f}, {"topre-classic", 0.88f},
    };
    constexpr std::string_view native_prefix = "keeby-native-";
    constexpr std::string_view web_prefix = "keeby-web-";
    if (id.starts_with(native_prefix)) id.remove_prefix(native_prefix.size());
    else if (id.starts_with(web_prefix)) id.remove_prefix(web_prefix.size());
    for (const auto& [name, trim] : trims)
        if (id == name) return trim;
    return 1.0f;
}

void normalize_catalog_wav(Sample& sample) noexcept {
    const std::size_t frames = sample.pcm.size() / kNativeChannels;
    if (frames == 0) return;
    const std::size_t window = std::min<std::size_t>(
        std::max<std::size_t>(64, kNativeSampleRate / 100), frames);
    double sum = 0.0;
    for (std::size_t f = 0; f < window; ++f) {
        const double l = sample.pcm[f * kNativeChannels];
        const double r = sample.pcm[f * kNativeChannels + 1];
        sum += l * l + r * r;
    }
    double best = sum;
    for (std::size_t start = 1; start + window <= frames; ++start) {
        const std::size_t old_i = (start - 1) * kNativeChannels;
        const std::size_t new_i = (start + window - 1) * kNativeChannels;
        for (std::size_t c = 0; c < kNativeChannels; ++c) {
            const double old_v = sample.pcm[old_i + c];
            const double new_v = sample.pcm[new_i + c];
            sum += new_v * new_v - old_v * old_v;
        }
        best = std::max(best, sum);
    }
    const double rms = std::sqrt(best / (window * kNativeChannels));
    if (rms <= 0.001) return;
    const float gain = static_cast<float>(std::clamp(0.32 / rms, 0.55, 16.0));
    if (std::abs(gain - 1.0f) <= 0.005f) return;
    for (float& value : sample.pcm) value *= gain;
}

// --- iohook/libuiohook code -> evdev KEY_* or supported BTN_* mapping ---
std::optional<uint16_t> map_mechvibes_code(long raw) noexcept {
    switch (raw) {
        case 3612: return static_cast<uint16_t>(KEY_KPENTER);
        case 3613: return static_cast<uint16_t>(KEY_RIGHTCTRL);
        case 3637: return static_cast<uint16_t>(KEY_KPSLASH);
        case 3639: return static_cast<uint16_t>(KEY_SYSRQ);
        case 3640: return static_cast<uint16_t>(KEY_RIGHTALT);
        case 3653: return static_cast<uint16_t>(KEY_PAUSE);
        case 3655: return static_cast<uint16_t>(KEY_HOME);
        case 3657: return static_cast<uint16_t>(KEY_PAGEUP);
        case 3663: return static_cast<uint16_t>(KEY_END);
        case 3665: return static_cast<uint16_t>(KEY_PAGEDOWN);
        case 3666: return static_cast<uint16_t>(KEY_INSERT);
        case 3667: return static_cast<uint16_t>(KEY_DELETE);
        case 3675: return static_cast<uint16_t>(KEY_LEFTMETA);
        case 3676: return static_cast<uint16_t>(KEY_RIGHTMETA);
        case 3677: return static_cast<uint16_t>(KEY_COMPOSE);
        case 3597: return static_cast<uint16_t>(KEY_KPEQUAL);
        case 57416: return static_cast<uint16_t>(KEY_UP);
        case 57419: return static_cast<uint16_t>(KEY_LEFT);
        case 57421: return static_cast<uint16_t>(KEY_RIGHT);
        case 57424: return static_cast<uint16_t>(KEY_DOWN);
        default:
            // Unprefixed codes equal evdev KEY_* codes directly.
            if (raw >= 0 && raw < static_cast<long>(kKeyCodeCount) &&
                is_supported_input_code(static_cast<uint16_t>(raw)))
                return static_cast<uint16_t>(raw);
            return std::nullopt; // unknown, or outside the supported event codes
    }
}

struct DefineKey {
    long code = 0;
    bool is_up = false;
};

// "<int>" -> a press define; "<int>-up" -> a release define. Anything else
// (non-numeric key) is not a code this loader understands.
std::optional<DefineKey> parse_define_key(const std::string& key) {
    std::string_view sv(key);
    bool is_up = false;
    if (sv.size() > 3 && sv.substr(sv.size() - 3) == "-up") {
        is_up = true;
        sv.remove_suffix(3);
    }
    if (sv.empty()) return std::nullopt;
    long value = 0;
    const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value);
    if (ec != std::errc{} || ptr != sv.data() + sv.size()) return std::nullopt;
    return DefineKey{value, is_up};
}

// Expands one `{lo-hi}` range in a filename into every literal file name it
// covers (KEEBY plays all of them as that key's variant set, rather than
// Mechvibes' pick-one-at-load-time). No braces -> the name is returned
// unchanged, as the sole element. An invalid or absurd range (lo > hi, or
// spanning >= 4096 files) returns empty, which callers treat as an error.
std::vector<std::string> expand_braces(const std::string& name) {
    static const std::regex re(R"(\{(\d+)-(\d+)\})");
    std::smatch m;
    if (!std::regex_search(name, m, re)) return {name};
    const long lo = std::stol(m[1].str());
    const long hi = std::stol(m[2].str());
    std::vector<std::string> out;
    if (lo > hi || hi - lo >= 4096) return out;
    for (long i = lo; i <= hi; ++i)
        out.push_back(name.substr(0, m.position(0)) + std::to_string(i) + name.substr(m.position(0) + m.length(0)));
    return out;
}

// The pack directory is untrusted input: `rel` must be relative, contain no
// ".." component, and resolve (following any symlink) to a regular file
// still inside `pack_dir`.
std::expected<fs::path, std::string> resolve_pack_file(const fs::path& pack_dir, const std::string& rel) {
    const fs::path relp(rel);
    if (relp.is_absolute()) return std::unexpected("sound_pack: absolute path not allowed: " + rel);
    for (const auto& part : relp)
        if (part == "..") return std::unexpected("sound_pack: path escapes pack directory: " + rel);
    std::error_code ec;
    const auto full = fs::weakly_canonical(pack_dir / relp, ec);
    if (ec) return std::unexpected("sound_pack: cannot resolve path: " + rel);
    const auto rel_check = fs::relative(full, pack_dir, ec);
    if (ec || rel_check.empty() || rel_check.begin()->string() == "..")
        return std::unexpected("sound_pack: path escapes pack directory: " + rel);
    if (!fs::is_regular_file(full, ec) || ec)
        return std::unexpected("sound_pack: missing or not a regular file: " + rel);
    return full;
}

// True only for resolve_pack_file's "doesn't exist" outcome, never for a
// security rejection (absolute path, `..`, symlink escape) -- those stay
// hard errors. Real Mechvibes packs sometimes reference a per-key file
// that was never actually shipped (e.g. mxblue-travel's press/BACKSPACE.mp3);
// that single missing key shouldn't sink the whole pack.
bool is_missing_file_error(const std::string& msg) {
    static constexpr std::string_view kPrefix = "sound_pack: missing or not a regular file: ";
    return msg.starts_with(kPrefix);
}

std::expected<RawAudio, std::string> decode_audio_file(const fs::path& path, bool preserve_peak = false) {
    SF_INFO info{};
    std::unique_ptr<SNDFILE, decltype(&sf_close)> file(sf_open(path.c_str(), SFM_READ, &info), &sf_close);
    if (!file) return std::unexpected(path.string() + ": " + sf_strerror(nullptr));
    if (info.channels < 1 || info.channels > 2)
        return std::unexpected(path.string() + ": only mono/stereo audio is supported");
    if (info.frames <= 0) return std::unexpected(path.string() + ": empty audio file");
    if (info.samplerate <= 0) return std::unexpected(path.string() + ": invalid sample rate");
    if (info.frames > static_cast<sf_count_t>(info.samplerate) * kMaxSourceSeconds)
        return std::unexpected(path.string() + ": audio file too long (over 10 minutes)");
    std::vector<float> decoded(static_cast<std::size_t>(info.frames) * static_cast<std::size_t>(info.channels));
    const auto read = sf_readf_float(file.get(), decoded.data(), info.frames);
    if (read != info.frames || sf_error(file.get()) != SF_ERR_NO_ERROR)
        return std::unexpected(path.string() + ": decode failed or short read");
    std::vector<float> stereo;
    if (info.channels == 1) {
        stereo.resize(decoded.size() * 2);
        for (std::size_t i = 0; i < decoded.size(); ++i) {
            stereo[2 * i] = decoded[i];
            stereo[2 * i + 1] = decoded[i];
        }
    } else {
        stereo = std::move(decoded);
    }
    for (float& v : stereo) {
        if (!std::isfinite(v)) return std::unexpected(path.string() + ": non-finite sample in decoded audio");
        if (!preserve_peak) v = std::clamp(v, -1.0f, 1.0f); // legacy codecs can produce tiny overs
    }
    return RawAudio{std::move(stereo), static_cast<uint32_t>(info.frames),
                    static_cast<uint32_t>(info.samplerate),
                    (info.format & SF_FORMAT_TYPEMASK) == SF_FORMAT_WAV};
}

// Used both for source-rate -> 48 kHz conversion (ratio = 48000/source) and
// for pitch variants (ratio = 1/playback_rate); ratio == 1 short-circuits
// to an exact copy so the 48 kHz path stays bit-identical to the source.
std::expected<Sample, std::string> resample_to(const std::vector<float>& stereo_pcm, uint32_t frames,
                                               double ratio, bool preserve_peak = false) {
    if (frames == 0) return std::unexpected("sound_pack: cannot resample zero-length audio");
    if (std::abs(ratio - 1.0) < 1e-9) return Sample{stereo_pcm};
    SRC_DATA data{};
    data.data_in = stereo_pcm.data();
    data.input_frames = static_cast<long>(frames);
    data.src_ratio = ratio;
    data.end_of_input = 1;
    const long estimated = static_cast<long>(std::ceil(static_cast<double>(frames) * ratio)) + 16;
    std::vector<float> out(static_cast<std::size_t>(estimated) * kNativeChannels);
    data.data_out = out.data();
    data.output_frames = estimated;
    const int err = src_simple(&data, SRC_SINC_MEDIUM_QUALITY, kNativeChannels);
    if (err != 0) return std::unexpected(std::string("sound_pack: resample failed: ") + src_strerror(err));
    out.resize(static_cast<std::size_t>(data.output_frames_gen) * kNativeChannels);
    for (float& v : out) {
        if (!std::isfinite(v)) return std::unexpected("sound_pack: resample produced a non-finite sample");
        if (!preserve_peak) v = std::clamp(v, -1.0f, 1.0f); // legacy sinc path clamps small overshoots
    }
    return Sample{std::move(out)};
}

std::expected<Sample, std::string> pitch_variant(const Sample& base, float rate) {
    const auto frames = static_cast<uint32_t>(base.pcm.size() / kNativeChannels);
    return resample_to(base.pcm, frames, 1.0 / rate);
}

// v1/v2 single mode: slice at the sprite's OWN sample rate first (exact
// frame boundaries, no resampling artifacts), then resample only the
// slice. Clamps to the sprite's length; rejects non-positive durations.
std::expected<Sample, std::string> slice_sprite(const RawAudio& sprite, double start_ms,
                                                double duration_ms, bool preserve_peak = false) {
    if (!(duration_ms > 0.0)) return std::unexpected("sound_pack: slice duration must be positive");
    const double rate = sprite.rate;
    const long total = static_cast<long>(sprite.frames);
    long start_frame = static_cast<long>(std::llround(std::max(0.0, start_ms) * rate / 1000.0));
    long length = static_cast<long>(std::llround(duration_ms * rate / 1000.0));
    start_frame = std::clamp<long>(start_frame, 0, total);
    const long end_frame = std::clamp<long>(start_frame + length, start_frame, total);
    length = end_frame - start_frame;
    if (length <= 0) return std::unexpected("sound_pack: slice is empty after clamping to the sprite's length");
    std::vector<float> pcm(static_cast<std::size_t>(length) * kNativeChannels);
    std::copy_n(sprite.pcm.begin() + static_cast<std::size_t>(start_frame) * kNativeChannels, pcm.size(), pcm.begin());
    if (sprite.rate == kNativeSampleRate) return Sample{std::move(pcm)};
    return resample_to(pcm, static_cast<uint32_t>(length),
                       static_cast<double>(kNativeSampleRate) / rate, preserve_peak);
}

struct PackBuildState {
    SoundBank bank;
    std::unordered_map<std::string, KeySound> cache; // dedupes identical clips/slices across keys
    std::size_t pcm_bytes = 0;
    std::size_t skipped_codes = 0;
    PlaybackPolicy playback_policy = PlaybackPolicy::Legacy;
};

std::expected<void, std::string> push_sample(PackBuildState& state, Sample sample) {
    if (state.bank.samples.size() >= kMaxSampleTableSize)
        return std::unexpected("sound_pack: too many samples in this pack");
    state.pcm_bytes += sample.pcm.size() * sizeof(float);
    if (state.pcm_bytes > kMaxPackPcmBytes)
        return std::unexpected("sound_pack: decoded audio exceeds the 256 MiB pack limit");
    state.bank.samples.push_back(std::move(sample));
    return {};
}

// Appends `bases` (one Sample per underlying clip/slice, e.g. a {lo-hi}
// group) plus a low/high pitch variant of EACH, and returns the KeySound
// spanning all of them (>= 3 variants per sounding key, as required).
std::expected<KeySound, std::string> append_variants(PackBuildState& state, std::vector<Sample> bases) {
    if (bases.empty()) return std::unexpected("sound_pack: key has no audio");
    if (state.playback_policy != PlaybackPolicy::Legacy) {
        if (bases.size() > 255)
            return std::unexpected("sound_pack: too many source clips for one key (over 255)");
        const uint16_t first = static_cast<uint16_t>(state.bank.samples.size());
        for (auto& base : bases) {
            if (state.playback_policy == PlaybackPolicy::KeebyCatalogWav) normalize_catalog_wav(base);
            if (auto ok = push_sample(state, std::move(base)); !ok) return std::unexpected(ok.error());
        }
        return KeySound{first, static_cast<uint8_t>(bases.size())};
    }
    if (bases.size() * 3 > 255)
        return std::unexpected("sound_pack: too many variants for one key (" +
                                std::to_string(bases.size() * 3) + " > 255)");
    if (state.bank.samples.size() >= kMaxSampleTableSize)
        return std::unexpected("sound_pack: sample table is full");
    const uint16_t first = static_cast<uint16_t>(state.bank.samples.size());
    for (auto& base : bases) {
        auto low = pitch_variant(base, 0.92f);
        if (!low) return std::unexpected(low.error());
        auto high = pitch_variant(base, 1.08f);
        if (!high) return std::unexpected(high.error());
        if (auto ok = push_sample(state, std::move(base)); !ok) return std::unexpected(ok.error());
        if (auto ok = push_sample(state, std::move(*low)); !ok) return std::unexpected(ok.error());
        if (auto ok = push_sample(state, std::move(*high)); !ok) return std::unexpected(ok.error());
    }
    return KeySound{first, static_cast<uint8_t>(bases.size() * 3)};
}

template <typename BuildFn>
std::expected<KeySound, std::string> get_or_build(PackBuildState& state, const std::string& cache_key,
                                                   BuildFn&& build) {
    if (auto it = state.cache.find(cache_key); it != state.cache.end()) return it->second;
    auto bases = build();
    if (!bases) return std::unexpected(bases.error());
    auto sound = append_variants(state, std::move(*bases));
    if (!sound) return std::unexpected(sound.error());
    state.cache.emplace(cache_key, *sound);
    return *sound;
}

// multi mode: `filename` (possibly a {lo-hi} range) becomes one key's
// variant set. Dedupes on the resolved file list, so e.g. the v2 default
// `sound`/`soundup` fallback -- reused by every undefined key -- is
// decoded and pitch-shifted exactly once.
std::expected<KeySound, std::string> build_multi_group(PackBuildState& state, const fs::path& pack_dir,
                                                        const std::string& filename) {
    auto files = expand_braces(filename);
    if (files.empty()) return std::unexpected("sound_pack: invalid {lo-hi} range in '" + filename + "'");
    std::string cache_key = "multi:";
    for (const auto& f : files) {
        cache_key += f;
        cache_key += '|';
    }
    return get_or_build(state, cache_key, [&]() -> std::expected<std::vector<Sample>, std::string> {
        std::vector<Sample> bases;
        bases.reserve(files.size());
        for (const auto& f : files) {
            auto resolved = resolve_pack_file(pack_dir, f);
            if (!resolved) return std::unexpected(resolved.error());
            const bool preserve_peak = state.playback_policy != PlaybackPolicy::Legacy;
            auto raw = decode_audio_file(*resolved, preserve_peak);
            if (!raw) return std::unexpected(raw.error());
            if (state.playback_policy == PlaybackPolicy::KeebyCatalogWav && !raw->wav)
                return std::unexpected("sound_pack: catalog WAV policy requires WAV audio: " + f);
            if (raw->rate == kNativeSampleRate) {
                bases.push_back(Sample{std::move(raw->pcm)});
                continue;
            }
            auto resampled = resample_to(raw->pcm, raw->frames,
                                         static_cast<double>(kNativeSampleRate) / raw->rate, preserve_peak);
            if (!resampled) return std::unexpected(resampled.error());
            bases.push_back(std::move(*resampled));
        }
        return bases;
    });
}

void normalize_pack_loudness(SoundBank& bank) {
    float peak = 0.0f;
    for (const auto& s : bank.samples)
        for (float v : s.pcm) peak = std::max(peak, std::fabs(v));
    if (peak <= 0.0f) return; // silence: nothing to scale
    const float scale = std::min(4.0f, 0.9f / peak); // boost capped at 4x; never reduces below the 0.9 target
    for (auto& s : bank.samples)
        for (float& v : s.pcm) v *= scale;
}

// Reads only `name` (falling back to `id`) from a pack's config.json --
// used by list_packs(), which must not decode any audio. Malformed JSON
// simply falls back to the directory name; it is not an error at this
// level (load_pack() is what actually validates the pack).
std::string read_pack_name(const fs::path& config_path, const std::string& id) {
    std::error_code ec;
    const auto size = fs::file_size(config_path, ec);
    if (ec || size > kMaxConfigBytes) return id;
    std::ifstream in(config_path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_discarded() && j.is_object() && j.contains("name") && j["name"].is_string())
        return j["name"].get<std::string>();
    return id;
}

void scan_dir_for_packs(const fs::path& base_dir, std::vector<PackInfo>& out, std::vector<std::string>& seen_ids) {
    std::error_code ec;
    if (!fs::is_directory(base_dir, ec)) return;
    try {
        for (const auto& entry : fs::directory_iterator(base_dir, ec)) {
            if (ec) return;
            std::error_code entry_ec;
            if (!entry.is_directory(entry_ec) || entry_ec) continue;
            std::string id = entry.path().filename().string();
            if (id == "default") {
                std::fprintf(stderr, "keeby: pack directory named 'default' in %s is reserved and ignored\n",
                             base_dir.c_str());
                continue;
            }
            if (std::find(seen_ids.begin(), seen_ids.end(), id) != seen_ids.end()) continue; // first wins
            const auto config_path = entry.path() / "config.json";
            if (!fs::is_regular_file(config_path, entry_ec) || entry_ec) continue; // no readable config: skip
            seen_ids.push_back(id);
            out.push_back(PackInfo{id, read_pack_name(config_path, id), entry.path()});
        }
    } catch (const std::exception&) {
        // A directory that becomes unreadable mid-scan shouldn't crash listing.
    }
}

} // namespace

std::expected<SoundBank, std::string> load_pack(const fs::path& dir, std::vector<std::string>* warnings) try {
    std::error_code ec;
    const auto pack_dir = fs::weakly_canonical(dir, ec);
    if (ec || !fs::is_directory(pack_dir)) return std::unexpected("sound_pack: not a directory: " + dir.string());

    const auto config_path = pack_dir / "config.json";
    const auto config_size = fs::file_size(config_path, ec);
    if (ec) return std::unexpected("sound_pack: missing config.json in " + pack_dir.string());
    if (config_size > kMaxConfigBytes) return std::unexpected("sound_pack: config.json exceeds the 1 MiB limit");

    std::ifstream in(config_path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof()) return std::unexpected("sound_pack: could not read config.json");

    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return std::unexpected("sound_pack: config.json is not a valid JSON object");

    const int version = j.value("version", 1);
    if (version >= 3)
        return std::unexpected("sound_pack: config version " + std::to_string(version) + " (v3/v4) is not supported");
    const bool is_v2 = version == 2;
    const bool single = j.value("key_define_type", std::string("single")) != "multi";
    const std::string config_id = j.contains("id") && j["id"].is_string()
        ? j["id"].get<std::string>() : std::string{};
    PlaybackPolicy playback_policy = PlaybackPolicy::Legacy;
    if (config_id == pack_dir.filename().string()) {
        if (is_v2 && !single && config_id.starts_with("keeby-native-"))
            playback_policy = PlaybackPolicy::KeebyCatalogWav;
        else if (is_v2 && single && config_id == "keeby-web-keychron-k2-max-brown")
            playback_policy = PlaybackPolicy::KeebyCatalogSprite;
    }

    if (!j.contains("defines") || !j["defines"].is_object())
        return std::unexpected("sound_pack: config.json is missing object 'defines'");
    const auto& defines = j["defines"];

    PackBuildState state;
    state.playback_policy = playback_policy;
    state.bank.playback_policy = playback_policy;
    if (playback_policy != PlaybackPolicy::Legacy) {
        const float trim = catalog_profile_trim(config_id);
        state.bank.press_gain = 0.28f * trim;
        state.bank.release_gain = 0.16f * trim;
    }
    auto assign = [&](uint16_t code, bool is_up, KeySound sound) {
        (is_up ? state.bank.release : state.bank.press)[code] = sound;
    };
    std::vector<std::string> local_warnings;
    auto add_warning = [&](std::string msg) {
        if (std::find(local_warnings.begin(), local_warnings.end(), msg) == local_warnings.end())
            local_warnings.push_back(std::move(msg));
    };

    if (single) {
        if (!j.contains("sound") || !j["sound"].is_string())
            return std::unexpected("sound_pack: single-mode config.json needs a string 'sound'");
        auto sprite_path = resolve_pack_file(pack_dir, j["sound"].get<std::string>());
        if (!sprite_path) return std::unexpected(sprite_path.error());
        const bool preserve_peak = playback_policy == PlaybackPolicy::KeebyCatalogSprite;
        auto sprite = decode_audio_file(*sprite_path, preserve_peak);
        if (!sprite) return std::unexpected(sprite.error());
        if (playback_policy == PlaybackPolicy::KeebyCatalogSprite && sprite->rate != kNativeSampleRate) {
            const double ratio = static_cast<double>(kNativeSampleRate) / sprite->rate;
            const auto target_frames = static_cast<std::uint64_t>(std::ceil(sprite->frames * ratio)) + 16;
            if (target_frames * kNativeChannels * sizeof(float) > kMaxPackPcmBytes)
                return std::unexpected("sound_pack: catalog sprite exceeds the 256 MiB resampled limit");
            auto resampled = resample_to(sprite->pcm, sprite->frames, ratio, /*preserve_peak=*/true);
            if (!resampled) return std::unexpected(resampled.error());
            sprite->pcm = std::move(resampled->pcm);
            sprite->frames = static_cast<uint32_t>(sprite->pcm.size() / kNativeChannels);
            sprite->rate = kNativeSampleRate;
        }

        for (const auto& [key, value] : defines.items()) {
            const auto parsed = parse_define_key(key);
            if (!parsed) { ++state.skipped_codes; continue; }
            const auto mapped = map_mechvibes_code(parsed->code);
            if (!mapped) { ++state.skipped_codes; continue; }
            if (!is_v2 && parsed->is_up) continue; // v1 has no key-up sounds (see docs/008)
            if (value.is_null()) continue; // explicit "no sound for this key": silent, not an error
            if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number())
                return std::unexpected("sound_pack: defines['" + key + "'] must be [start_ms, duration_ms]");
            const double start_ms = value[0].get<double>();
            const double duration_ms = value[1].get<double>();
            const std::string cache_key = "slice:" + std::to_string(start_ms) + ":" + std::to_string(duration_ms);
            auto sound = get_or_build(state, cache_key, [&]() -> std::expected<std::vector<Sample>, std::string> {
                auto slice = slice_sprite(*sprite, start_ms, duration_ms, preserve_peak);
                if (!slice) return std::unexpected(slice.error());
                std::vector<Sample> one;
                one.push_back(std::move(*slice));
                return one;
            });
            if (!sound) return std::unexpected(sound.error());
            assign(*mapped, parsed->is_up, *sound);
        }
    } else {
        std::optional<KeySound> default_press, default_release;
        if (is_v2 && j.contains("sound") && j["sound"].is_string()) {
            auto group = build_multi_group(state, pack_dir, j["sound"].get<std::string>());
            if (!group) return std::unexpected(group.error());
            default_press = *group;
        }
        if (is_v2 && j.contains("soundup") && j["soundup"].is_string()) {
            auto group = build_multi_group(state, pack_dir, j["soundup"].get<std::string>());
            if (!group) return std::unexpected(group.error());
            default_release = *group;
        }
        for (const auto& [key, value] : defines.items()) {
            const auto parsed = parse_define_key(key);
            if (!parsed) { ++state.skipped_codes; continue; }
            const auto mapped = map_mechvibes_code(parsed->code);
            if (!mapped) { ++state.skipped_codes; continue; }
            if (!is_v2 && parsed->is_up) continue; // v1 has no key-up sounds (see docs/008)
            // null: v2 falls back to the pack default below (same as an
            // undefined key); v1/no-default just stays silent.
            if (value.is_null()) continue;
            if (!value.is_string())
                return std::unexpected("sound_pack: defines['" + key + "'] must be a filename string");
            auto group = build_multi_group(state, pack_dir, value.get<std::string>());
            if (!group) {
                // A file that genuinely doesn't exist on disk skips just this
                // key/direction (with a warning); every other resolution
                // failure (absolute path, "..", symlink escape, decode
                // failure) stays a hard error.
                if (!is_missing_file_error(group.error())) return std::unexpected(group.error());
                add_warning(group.error());
                continue;
            }
            assign(*mapped, parsed->is_up, *group);
        }
        // v2 only: a key with no explicit press/release define falls back
        // to the pack's default sound/soundup. v1 has no such fallback.
        if (is_v2 && (default_press || default_release)) {
            for (std::size_t code = 1; code < kKeyCodeCount; ++code) {
                if (!is_supported_input_code(static_cast<uint16_t>(code))) continue;
                if (default_press && state.bank.press[code].count == 0) state.bank.press[code] = *default_press;
                if (default_release && state.bank.release[code].count == 0)
                    state.bank.release[code] = *default_release;
            }
        }
    }

    // A pack that never sounds on press (every key silent) is useless and
    // almost certainly a config mistake rather than an intentional pack, so
    // this stays a hard error even though individual missing/null keys don't.
    if (bool has_press = std::any_of(state.bank.press.begin(), state.bank.press.end(),
                                      [](const KeySound& s) { return s.count != 0; });
        !has_press)
        return std::unexpected("sound_pack: pack has no press sound for any key");

    for (std::size_t code = 0; code < kKeyCodeCount; ++code)
        state.bank.pan[code] = key_pan(static_cast<uint16_t>(code));

    if (playback_policy == PlaybackPolicy::Legacy) normalize_pack_loudness(state.bank);

    if (state.skipped_codes > 0)
        add_warning("pack '" + pack_dir.filename().string() + "': skipped " +
                    std::to_string(state.skipped_codes) + " unknown/out-of-range key code(s)");

    if (auto ok = validate_sound_bank(state.bank); !ok) return std::unexpected(ok.error());

    if (warnings) {
        for (auto& w : local_warnings) warnings->push_back(std::move(w));
    } else {
        for (const auto& w : local_warnings) std::fprintf(stderr, "keeby: %s\n", w.c_str());
    }
    return std::move(state.bank);
} catch (const std::exception& e) {
    return std::unexpected(std::string("sound_pack: ") + e.what());
} catch (...) {
    return std::unexpected("sound_pack: unknown error while loading pack");
}

std::vector<PackInfo> list_packs() {
    std::vector<PackInfo> result;
    result.push_back(PackInfo{"default", "Default (built-in)", {}});
    std::vector<std::string> seen_ids{"default"};

    fs::path data_home;
    if (const char* xdh = std::getenv("XDG_DATA_HOME"); xdh && *xdh) data_home = xdh;
    else if (const char* home = std::getenv("HOME"); home && *home) data_home = fs::path(home) / ".local/share";
    if (!data_home.empty()) scan_dir_for_packs(data_home / "keeby/packs", result, seen_ids);

    std::string data_dirs = "/usr/local/share:/usr/share";
    if (const char* dd = std::getenv("XDG_DATA_DIRS"); dd && *dd) data_dirs = dd;
    std::size_t pos = 0;
    while (pos <= data_dirs.size()) {
        const auto next = data_dirs.find(':', pos);
        const std::string entry = data_dirs.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        if (!entry.empty()) scan_dir_for_packs(fs::path(entry) / "keeby/packs", result, seen_ids);
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return result;
}

} // namespace keeby
