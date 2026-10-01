#include "settings.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using keeby::Settings;

fs::path make_tmp_dir(const char* tag) {
    fs::path dir = fs::temp_directory_path() / (std::string("keeby_settings_test_") + tag + "_" +
                                                 std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

mode_t file_mode(const fs::path& p) {
    struct stat st{};
    const int rc = ::stat(p.c_str(), &st);
    assert(rc == 0);
    return st.st_mode & 07777;
}

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

void test_missing_file_defaults() {
    fs::path dir = make_tmp_dir("missing");
    std::vector<std::string> warnings;
    Settings s = keeby::load_settings(dir / "nope" / "settings.ini", &warnings);
    assert(s.enabled == true);
    assert(s.volume == 1.0f);
    assert(s.stereo_width == 1.0f);
    assert(s.profile == "default");
    assert(s.tone_x == 0.0f);
    assert(s.tone_y == 0.0f);
    assert(s.visualizer == true);
    assert(s.visualizer_position == "follow-cursor");
    assert(s.visualizer_dismiss_ms == 1000);
    assert(s.visualizer_follow_speed == 1.0f);
    assert(warnings.empty());
    fs::remove_all(dir);
}

void test_round_trip() {
    fs::path dir = make_tmp_dir("roundtrip");
    fs::path path = dir / "cfg" / "settings.ini";
    Settings original;
    original.enabled = false;
    original.volume = 0.1f;       // not exactly representable in binary: stresses round-trip precision
    original.stereo_width = 1.5f;
    original.profile = "clicky-blue";
    original.tone_x = -0.3f;   // not exactly representable in binary either
    original.tone_y = 0.7f;
    original.visualizer = false;
    original.visualizer_position = "top-right";
    original.visualizer_dismiss_ms = 2500;
    original.visualizer_follow_speed = 2.5f;

    auto saved = keeby::save_settings(path, original);
    assert(saved.has_value());

    Settings loaded = keeby::load_settings(path);
    assert(loaded.enabled == original.enabled);
    assert(loaded.volume == original.volume);
    assert(loaded.stereo_width == original.stereo_width);
    assert(loaded.profile == original.profile);
    assert(loaded.tone_x == original.tone_x);
    assert(loaded.tone_y == original.tone_y);
    assert(loaded.visualizer == original.visualizer);
    assert(loaded.visualizer_position == original.visualizer_position);
    assert(loaded.visualizer_dismiss_ms == original.visualizer_dismiss_ms);
    assert(loaded.visualizer_follow_speed == original.visualizer_follow_speed);
    fs::remove_all(dir);
}

void test_visualizer_validation() {
    fs::path dir = make_tmp_dir("visualizer");
    fs::path path = dir / "settings.ini";

    write_file(path, "visualizer = false\nvisualizer_position = not-a-position\nvisualizer_dismiss_ms = 99999\n");
    std::vector<std::string> warnings;
    Settings s = keeby::load_settings(path, &warnings);
    assert(s.visualizer == false);
    assert(s.visualizer_position == "follow-cursor"); // invalid: default kept
    assert(s.visualizer_dismiss_ms == 5000);           // clamped
    assert(warnings.size() == 2);

    write_file(path, "visualizer_follow_speed = 10.0\n");
    warnings.clear();
    Settings s4 = keeby::load_settings(path, &warnings);
    assert(s4.visualizer_follow_speed == 4.0f); // clamped from above
    assert(warnings.size() == 1);

    write_file(path, "visualizer_follow_speed = 0.0\n");
    warnings.clear();
    Settings s5 = keeby::load_settings(path, &warnings);
    assert(s5.visualizer_follow_speed == 0.1f); // clamped from below
    assert(warnings.size() == 1);

    write_file(path, "visualizer_dismiss_ms = 10\nvisualizer = maybe\n");
    warnings.clear();
    Settings s2 = keeby::load_settings(path, &warnings);
    assert(s2.visualizer_dismiss_ms == 250); // clamped from below
    assert(s2.visualizer == true);           // unparseable bool: default kept
    assert(warnings.size() == 2);

    write_file(path, "visualizer_dismiss_ms = not_a_number\n");
    warnings.clear();
    Settings s3 = keeby::load_settings(path, &warnings);
    assert(s3.visualizer_dismiss_ms == 1000); // default kept: unparseable
    assert(warnings.size() == 1);
    fs::remove_all(dir);
}

void test_tone_clamping() {
    fs::path dir = make_tmp_dir("tone_clamp");
    fs::path path = dir / "settings.ini";
    write_file(path, "tone_x = 5.0\ntone_y = -3\n");

    std::vector<std::string> warnings;
    Settings s = keeby::load_settings(path, &warnings);
    assert(s.tone_x == 1.0f);   // clamped from 5.0
    assert(s.tone_y == -1.0f);  // clamped from -3
    assert(warnings.size() == 2);
    fs::remove_all(dir);
}

void test_clamping_and_nonfinite() {
    fs::path dir = make_tmp_dir("clamp");
    fs::path path = dir / "settings.ini";
    write_file(path, "enabled = true\nvolume = 5.0\nstereo_width = -3\nprofile = default\n");

    std::vector<std::string> warnings;
    Settings s = keeby::load_settings(path, &warnings);
    assert(s.volume == 1.0f);        // clamped from 5.0
    assert(s.stereo_width == 0.0f);  // clamped from -3
    assert(warnings.size() == 2);

    write_file(path, "volume = nan\nstereo_width = not_a_number\n");
    warnings.clear();
    Settings g = keeby::load_settings(path, &warnings);
    assert(g.volume == 1.0f);        // default kept: non-finite
    assert(g.stereo_width == 1.0f);  // default kept: unparseable
    assert(warnings.size() == 2);
    fs::remove_all(dir);
}

void test_unknown_keys_comments_blank_lines() {
    fs::path dir = make_tmp_dir("unknown");
    fs::path path = dir / "settings.ini";
    write_file(path,
               "# a comment\n"
               "\n"
               "enabled = false\n"
               "mystery_key = 42\n"
               "   \n"
               "profile = clacky\n");

    std::vector<std::string> warnings;
    Settings s = keeby::load_settings(path, &warnings);
    assert(s.enabled == false);
    assert(s.profile == "clacky");
    assert(warnings.size() == 1); // only the unknown key
    fs::remove_all(dir);
}

void test_profile_validation() {
    fs::path dir = make_tmp_dir("profile");
    fs::path path = dir / "settings.ini";

    write_file(path, "profile = " + std::string(200, 'x') + "\n");
    std::vector<std::string> warnings;
    Settings s = keeby::load_settings(path, &warnings);
    assert(s.profile == "default");
    assert(!warnings.empty());

    write_file(path, std::string("profile = bad\x01name\n"));
    warnings.clear();
    Settings s2 = keeby::load_settings(path, &warnings);
    assert(s2.profile == "default");
    assert(!warnings.empty());
    fs::remove_all(dir);
}

void test_atomic_save_no_temp_left() {
    fs::path dir = make_tmp_dir("atomic");
    fs::path path = dir / "cfg" / "settings.ini";
    auto res = keeby::save_settings(path, Settings{});
    assert(res.has_value());

    int total = 0;
    int ini_files = 0;
    for (auto& entry : fs::directory_iterator(path.parent_path())) {
        ++total;
        if (entry.path().filename() == "settings.ini") ++ini_files;
    }
    assert(total == 1);
    assert(ini_files == 1);
    fs::remove_all(dir);
}

void test_permissions() {
    fs::path dir = make_tmp_dir("perm");
    fs::path path = dir / "secure" / "settings.ini";
    auto res = keeby::save_settings(path, Settings{});
    assert(res.has_value());
    assert(file_mode(path) == 0600);
    assert(file_mode(path.parent_path()) == 0700);
    fs::remove_all(dir);
}

void test_default_path_respects_xdg() {
    const char* old_xdg = std::getenv("XDG_CONFIG_HOME");
    const bool had_xdg = old_xdg != nullptr;
    const std::string old_xdg_copy = had_xdg ? old_xdg : "";
    const char* old_home = std::getenv("HOME");
    const bool had_home = old_home != nullptr;
    const std::string old_home_copy = had_home ? old_home : "";

    ::setenv("XDG_CONFIG_HOME", "/tmp/keeby_xdg_test", 1);
    assert(keeby::default_settings_path() == fs::path("/tmp/keeby_xdg_test/keeby/settings.ini"));

    ::unsetenv("XDG_CONFIG_HOME");
    ::setenv("HOME", "/tmp/keeby_home_test", 1);
    assert(keeby::default_settings_path() == fs::path("/tmp/keeby_home_test/.config/keeby/settings.ini"));

    if (had_xdg) ::setenv("XDG_CONFIG_HOME", old_xdg_copy.c_str(), 1);
    else ::unsetenv("XDG_CONFIG_HOME");
    if (had_home) ::setenv("HOME", old_home_copy.c_str(), 1);
    else ::unsetenv("HOME");
}

} // namespace

int main() {
    test_missing_file_defaults();
    test_round_trip();
    test_visualizer_validation();
    test_clamping_and_nonfinite();
    test_tone_clamping();
    test_unknown_keys_comments_blank_lines();
    test_profile_validation();
    test_atomic_save_no_temp_left();
    test_permissions();
    test_default_path_respects_xdg();

    std::puts("settings_test: OK (defaults, round trip, clamping, unknown keys, "
              "comments, profile validation, visualizer validation, atomic save, permissions, XDG path)");
    return 0;
}
