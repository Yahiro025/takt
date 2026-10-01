#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <samplerate.h>
#include <sndfile.h>

#include "key_event.hpp"
#include "sample_mixer.hpp"
#include "sound_pack.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

struct Audio {
    std::vector<float> pcm;
    uint32_t source_rate = 0;
    uint32_t source_frames = 0;
    uint32_t frames = 0;
};

bool load_source(const fs::path& path, Audio& out, bool preserve_peak) {
    SF_INFO info{};
    std::unique_ptr<SNDFILE, decltype(&sf_close)> file(sf_open(path.c_str(), SFM_READ, &info), &sf_close);
    if (!file || info.channels < 1 || info.channels > 2 || info.frames <= 0 || info.samplerate <= 0) return false;
    std::vector<float> decoded(static_cast<size_t>(info.frames) * static_cast<size_t>(info.channels));
    if (sf_readf_float(file.get(), decoded.data(), info.frames) != info.frames) return false;
    if (info.channels == 1) {
        out.pcm.resize(decoded.size() * 2);
        for (size_t i = 0; i < decoded.size(); ++i) out.pcm[2 * i] = out.pcm[2 * i + 1] = decoded[i];
    } else out.pcm = std::move(decoded);
    out.source_rate = static_cast<uint32_t>(info.samplerate);
    out.source_frames = static_cast<uint32_t>(info.frames);
    if (out.source_rate == keeby::kNativeSampleRate) {
        out.frames = out.source_frames;
    } else {
        SRC_DATA data{};
        data.data_in = out.pcm.data();
        data.input_frames = static_cast<long>(out.source_frames);
        data.src_ratio = static_cast<double>(keeby::kNativeSampleRate) / out.source_rate;
        data.end_of_input = 1;
        const long capacity = static_cast<long>(std::ceil(out.source_frames * data.src_ratio)) + 16;
        std::vector<float> resampled(static_cast<size_t>(capacity) * 2);
        data.data_out = resampled.data();
        data.output_frames = capacity;
        if (src_simple(&data, SRC_SINC_MEDIUM_QUALITY, 2) != 0) return false;
        resampled.resize(static_cast<size_t>(data.output_frames_gen) * 2);
        out.pcm = std::move(resampled);
        out.frames = static_cast<uint32_t>(data.output_frames_gen);
    }
    if (!preserve_peak)
        for (float& v : out.pcm) v = std::clamp(v, -1.0f, 1.0f);
    return true;
}

double max_window_rms(const std::vector<float>& pcm, uint32_t frames, uint32_t window) {
    if (frames == 0 || window == 0) return 0.0;
    std::vector<double> prefix(static_cast<size_t>(frames) + 1, 0.0);
    for (uint32_t f = 0; f < frames; ++f)
        prefix[f + 1] = prefix[f] + pcm[2 * f] * pcm[2 * f] + pcm[2 * f + 1] * pcm[2 * f + 1];
    double best = 0.0;
    for (uint32_t start = 0; start + window <= frames; ++start)
        best = std::max(best, (prefix[start + window] - prefix[start]) / (2.0 * window));
    return std::sqrt(best);
}

struct Stats { double rms = 0, peak = 0, max10ms = 0; };
Stats stats(const std::vector<float>& pcm) {
    Stats s;
    if (pcm.empty()) return s;
    double sum = 0.0;
    for (const float v : pcm) { sum += static_cast<double>(v) * v; s.peak = std::max(s.peak, std::abs(static_cast<double>(v))); }
    const uint32_t frames = static_cast<uint32_t>(pcm.size() / 2);
    s.rms = std::sqrt(sum / pcm.size());
    s.max10ms = max_window_rms(pcm, frames, std::min<uint32_t>(480, frames));
    return s;
}

std::vector<std::string> expand(const std::string& filename) {
    const auto open = filename.find('{');
    if (open == std::string::npos) return {filename};
    const auto dash = filename.find('-', open), close = filename.find('}', open);
    if (dash == std::string::npos || close == std::string::npos) return {};
    const int lo = std::stoi(filename.substr(open + 1, dash - open - 1));
    const int hi = std::stoi(filename.substr(dash + 1, close - dash - 1));
    std::vector<std::string> files;
    for (int i = lo; i <= hi; ++i) files.push_back(filename.substr(0, open) + std::to_string(i) + filename.substr(close + 1));
    return files;
}

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: native_wav_audit PACKS_DIR\n"; return 64; }
    std::cout << "profile,key,source_file,source_rate,source_frames,loaded_source_frames,source_rms,source_peak,source_max10ms,b7_gain,b7_rms,b7_peak,linux_pack_scale,source_fit_error,loaded_variant,loaded_rms,loaded_peak,loaded_max10ms\n";
    std::size_t audited_files = 0;
    bool failed = false;
    for (const auto& entry : fs::directory_iterator(argv[1])) {
        if (!entry.is_directory()) continue;
        const auto profile = entry.path().filename().string();
        const bool is_native = profile.rfind("keeby-native-", 0) == 0;
        const bool is_legacy_turquoise = profile == "turquoise";
        if (!is_native && !is_legacy_turquoise) continue;
        const json config = json::parse(std::ifstream(entry.path() / "config.json"));
        if (!config.contains("sound") || !config["sound"].is_string()) { failed = true; continue; }
        std::vector<std::string> warnings;
        auto loaded = keeby::load_pack(entry.path(), &warnings);
        if (!loaded) { std::cerr << "AUDIT_ERROR," << profile << "\n"; failed = true; continue; }
        std::vector<std::pair<std::string, keeby::KeySound>> groups;
        groups.emplace_back("A", loaded->press[KEY_A]);
        if (is_legacy_turquoise) groups.emplace_back("Space", loaded->press[KEY_SPACE]);
        for (const auto& [key, range] : groups) {
            std::string group_spec;
            if (key == "A") group_spec = config["sound"].get<std::string>();
            else {
                const auto it = config["defines"].find("57");
                if (it == config["defines"].end() || !it->is_string()) { failed = true; continue; }
                group_spec = it->get<std::string>();
            }
            const auto sources = expand(group_spec);
            const size_t variants_per_source = is_native ? 1 : 3;
            if (sources.empty() || range.count < sources.size() * variants_per_source) {
                std::cerr << "AUDIT_ERROR," << profile << ',' << key << "\n"; failed = true; continue;
            }
            for (size_t i = 0; i < sources.size(); ++i) {
            Audio source;
            if (!load_source(entry.path() / sources[i], source, is_native)) {
                std::cerr << "SOURCE_ERROR," << profile << ',' << sources[i] << "\n"; failed = true; continue;
            }
            const auto source_stats = stats(source.pcm);
            const uint32_t window = std::min<uint32_t>(480, source.frames);
            const double b7_measure = max_window_rms(source.pcm, source.frames, window);
            double b7_gain = b7_measure <= 0.001 ? 1.0 : std::clamp(0.32 / b7_measure, 0.55, 16.0);
            if (std::abs(b7_gain - 1.0) <= 0.005) b7_gain = 1.0;
            std::vector<float> expected = source.pcm;
            for (float& v : expected) v = static_cast<float>(v * b7_gain);
            const auto b7_stats = stats(expected);
            const auto& base = loaded->samples[range.first + i * variants_per_source].pcm;
            if (base.size() != source.pcm.size()) { std::cerr << "FRAME_MISMATCH," << profile << ',' << sources[i] << '\n'; failed = true; continue; }
            double dot = 0, norm = 0;
            for (size_t n = 0; n < base.size(); ++n) { dot += source.pcm[n] * base[n]; norm += source.pcm[n] * source.pcm[n]; }
            const double scale = norm > 0 ? dot / norm : 0;
            double max_error = 0;
            for (size_t n = 0; n < base.size(); ++n) {
                const double expected_value = is_native ? expected[n] : source.pcm[n] * scale;
                max_error = std::max(max_error, std::abs(static_cast<double>(base[n]) - expected_value));
            }
            if (is_native && (std::abs(scale - b7_gain) > 1e-4 || max_error > 1e-5)) {
                std::cerr << "POLICY_MISMATCH," << profile << ',' << sources[i] << ',' << scale << ',' << b7_gain << ',' << max_error << '\n';
                failed = true;
            }
            for (size_t v = 0; v < variants_per_source; ++v) {
                const auto& loaded_variant = loaded->samples[range.first + i * variants_per_source + v].pcm;
                const auto loaded_stats = stats(loaded_variant);
                const char* label = is_native ? "catalog_source" :
                    (v == 0 ? "legacy_source" : (v == 1 ? "generated_pitch_0.92" : "generated_pitch_1.08"));
                std::cout << profile << ',' << key << ',' << sources[i] << ',' << source.source_rate << ',' << source.source_frames << ',' << source.frames << ','
                          << source_stats.rms << ',' << source_stats.peak << ',' << source_stats.max10ms << ',' << b7_gain << ','
                          << b7_stats.rms << ',' << b7_stats.peak << ',' << scale << ',' << max_error << ',' << label << ','
                          << loaded_stats.rms << ',' << loaded_stats.peak << ',' << loaded_stats.max10ms << '\n';
            }
            ++audited_files;
            }
        }
    }
    std::cerr << "audited_source_wavs=" << audited_files << " failures=" << (failed ? "yes" : "no") << "\n";
    return failed || audited_files == 0 ? 2 : 0;
}
