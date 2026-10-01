#include "settings.hpp"
#include "visualizer_wire.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace keeby {
namespace {

std::string trim(const std::string& s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    auto begin = std::find_if(s.begin(), s.end(), not_space);
    auto end = std::find_if(s.rbegin(), s.rend(), not_space).base();
    return begin < end ? std::string(begin, end) : std::string();
}

bool has_control_char(const std::string& s) {
    return std::any_of(s.begin(), s.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

bool parse_bool(const std::string& v, bool& out) {
    if (v == "true") { out = true; return true; }
    if (v == "false") { out = false; return true; }
    return false;
}

// Locale-independent (unlike strtof, which reads the "C" library's current
// locale for the decimal point). Accepts anything from_chars accepts,
// including "nan"/"inf" (finiteness is checked by the caller, since "parsed
// but non-finite" and "didn't parse" get different warnings).
bool parse_float(const std::string& v, float& out) {
    if (v.empty()) return false;
    const auto result = std::from_chars(v.data(), v.data() + v.size(), out);
    return result.ec == std::errc() && result.ptr == v.data() + v.size();
}

void set_clamped_float(const std::string& key, const std::string& value, float lo, float hi, float& field,
                        std::vector<std::string>* warnings) {
    auto warn = [&](std::string msg) { if (warnings) warnings->push_back(std::move(msg)); };
    float parsed;
    if (!parse_float(value, parsed)) {
        warn("settings: '" + key + "' is not a number: " + value);
        return;
    }
    if (!std::isfinite(parsed)) {
        warn("settings: '" + key + "' must be finite: " + value);
        return;
    }
    float clamped = std::clamp(parsed, lo, hi);
    if (clamped != parsed) warn("settings: '" + key + "' out of range, clamped: " + value);
    field = clamped;
}

} // namespace

std::filesystem::path default_settings_path() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "keeby" / "settings.ini";
    }
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : "") / ".config" / "keeby" / "settings.ini";
}

Settings load_settings(const std::filesystem::path& path, std::vector<std::string>* warnings) {
    Settings settings;
    auto warn = [&](std::string msg) { if (warnings) warnings->push_back(std::move(msg)); };

    errno = 0;
    std::ifstream in(path);
    if (!in) {
        const int open_errno = errno; // read before any other call can clobber it
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            warn("settings: '" + path.string() + "' exists but could not be opened: " + std::strerror(open_errno));
        }
        return settings; // missing file -> defaults, no warning; unreadable file -> defaults, with a warning
    }

    std::string raw_line;
    while (std::getline(in, raw_line)) {
        const std::string line = trim(raw_line);
        if (line.empty() || line[0] == '#') continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            warn("settings: malformed line (no '='): " + line);
            continue;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));

        if (key == "enabled") {
            bool b;
            if (parse_bool(value, b)) settings.enabled = b;
            else warn("settings: 'enabled' must be true/false: " + value);
        } else if (key == "volume") {
            set_clamped_float(key, value, 0.0f, 1.0f, settings.volume, warnings);
        } else if (key == "stereo_width") {
            set_clamped_float(key, value, 0.0f, 2.0f, settings.stereo_width, warnings);
        } else if (key == "tone_x") {
            set_clamped_float(key, value, -1.0f, 1.0f, settings.tone_x, warnings);
        } else if (key == "tone_y") {
            set_clamped_float(key, value, -1.0f, 1.0f, settings.tone_y, warnings);
        } else if (key == "profile") {
            if (value.size() <= 128 && !has_control_char(value)) settings.profile = value;
            else warn("settings: invalid 'profile' (too long or has control characters)");
        } else if (key == "visualizer") {
            bool b;
            if (parse_bool(value, b)) settings.visualizer = b;
            else warn("settings: 'visualizer' must be true/false: " + value);
        } else if (key == "visualizer_position") {
            if (viz::position_from_string(value)) settings.visualizer_position = value;
            else warn("settings: invalid 'visualizer_position': " + value);
        } else if (key == "visualizer_dismiss_ms") {
            int parsed = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (result.ec != std::errc() || result.ptr != value.data() + value.size()) {
                warn("settings: 'visualizer_dismiss_ms' is not an integer: " + value);
            } else {
                const int clamped = std::clamp(parsed, static_cast<int>(viz::kMinDismissMs),
                                                static_cast<int>(viz::kMaxDismissMs));
                if (clamped != parsed) warn("settings: 'visualizer_dismiss_ms' out of range, clamped: " + value);
                settings.visualizer_dismiss_ms = static_cast<uint16_t>(clamped);
            }
        } else if (key == "visualizer_follow_speed") {
            set_clamped_float(key, value, 0.1f, 4.0f, settings.visualizer_follow_speed, warnings);
        } else {
            warn("settings: unknown key ignored: " + key);
        }
    }
    return settings;
}

std::expected<void, std::string> save_settings(const std::filesystem::path& path, const Settings& settings) {
    std::filesystem::path dir = path.parent_path();
    if (dir.empty()) dir = ".";

    std::error_code ec;
    if (!std::filesystem::exists(dir)) {
        std::filesystem::create_directories(dir, ec);
        if (ec) return std::unexpected("settings: failed to create " + dir.string() + ": " + ec.message());
        if (::chmod(dir.c_str(), 0700) != 0) {
            return std::unexpected(std::string("settings: chmod on ") + dir.string() + " failed: " + std::strerror(errno));
        }
    }

    std::ostringstream body;
    body.imbue(std::locale::classic());
    constexpr int kFloatDigits = std::numeric_limits<float>::max_digits10;
    body << "enabled = " << (settings.enabled ? "true" : "false") << "\n";
    body << "volume = " << std::setprecision(kFloatDigits) << settings.volume << "\n";
    body << "stereo_width = " << std::setprecision(kFloatDigits) << settings.stereo_width << "\n";
    body << "tone_x = " << std::setprecision(kFloatDigits) << settings.tone_x << "\n";
    body << "tone_y = " << std::setprecision(kFloatDigits) << settings.tone_y << "\n";
    body << "profile = " << settings.profile << "\n";
    body << "visualizer = " << (settings.visualizer ? "true" : "false") << "\n";
    body << "visualizer_position = " << settings.visualizer_position << "\n";
    body << "visualizer_dismiss_ms = " << settings.visualizer_dismiss_ms << "\n";
    body << "visualizer_follow_speed = " << std::setprecision(kFloatDigits) << settings.visualizer_follow_speed << "\n";
    const std::string content = body.str();

    std::string tmp_template = (dir / path.filename()).string() + ".XXXXXX";
    const int fd = ::mkstemp(tmp_template.data());
    if (fd < 0) return std::unexpected(std::string("settings: mkstemp failed: ") + std::strerror(errno));

    std::FILE* f = ::fdopen(fd, "w");
    if (!f) {
        const std::string err = std::strerror(errno);
        ::close(fd);
        ::unlink(tmp_template.c_str());
        return std::unexpected("settings: fdopen failed: " + err);
    }
    const bool write_ok = std::fwrite(content.data(), 1, content.size(), f) == content.size();
    const bool sync_ok = write_ok && std::fflush(f) == 0 && ::fsync(fileno(f)) == 0;
    const std::string io_err = sync_ok ? std::string() : std::strerror(errno);
    std::fclose(f); // also closes fd
    if (!sync_ok) {
        ::unlink(tmp_template.c_str());
        return std::unexpected("settings: write failed: " + io_err);
    }

    if (::rename(tmp_template.c_str(), path.c_str()) != 0) {
        const std::string err = std::strerror(errno);
        ::unlink(tmp_template.c_str());
        return std::unexpected("settings: rename failed: " + err);
    }

    // Durability, not correctness: the rename already succeeded, so a failed
    // directory fsync doesn't make this save() report failure -- it only
    // affects how quickly the rename is safe against a crash/power loss.
    const int dir_fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        ::fsync(dir_fd);
        ::close(dir_fd);
    }
    return {};
}

} // namespace keeby
