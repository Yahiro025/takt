#include "tray_service.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

// Pure-logic tests only: build_tray_menu/dispatch_tray_menu_click take no
// D-Bus connection and no EngineController, so this needs neither a session
// bus nor a real engine -- unlike dbus_menu_test/control_service_test, it
// always runs.
namespace {
using keeby::MenuItem;
using keeby::PackInfo;
using keeby::TrayActions;
using keeby::TrayMenuState;

const MenuItem* find(const std::vector<MenuItem>& items, int32_t id) {
    for (const auto& item : items) {
        if (item.id == id) return &item;
        if (const MenuItem* hit = find(item.children, id)) return hit;
    }
    return nullptr;
}

TrayMenuState make_state(float volume, float width, std::string profile, std::vector<PackInfo> profiles) {
    TrayMenuState s;
    s.enabled = true;
    s.volume = volume;
    s.stereo_width = width;
    s.current_profile = std::move(profile);
    s.profiles = std::move(profiles);
    return s;
}

void test_sound_on_checkbox() {
    auto on = keeby::build_tray_menu(make_state(1.0f, 1.0f, "default", {{"default", "Default", {}}}));
    const MenuItem* sound = find(on, keeby::kTrayIdSoundOn);
    assert(sound && sound->kind == MenuItem::Kind::Checkmark);
    assert(sound->label == "Sound on");
    assert(sound->checked);

    auto off_state = make_state(1.0f, 1.0f, "default", {{"default", "Default", {}}});
    off_state.enabled = false;
    auto off = keeby::build_tray_menu(off_state);
    assert(!find(off, keeby::kTrayIdSoundOn)->checked);
}

void test_volume_presets_exact_near_far() {
    auto exact = keeby::build_tray_menu(make_state(0.50f, 1.0f, "default", {{"default", "Default", {}}}));
    const MenuItem* menu = find(exact, keeby::kTrayIdVolumeMenu);
    assert(menu && menu->label == "Volume: 50%");
    for (const auto& child : menu->children) assert(child.kind == MenuItem::Kind::Radio);
    assert(find(exact, keeby::kTrayIdVolume50)->checked);
    assert(!find(exact, keeby::kTrayIdVolume25)->checked);
    assert(!find(exact, keeby::kTrayIdVolume75)->checked);
    assert(!find(exact, keeby::kTrayIdVolume100)->checked);

    // 0.51 is within the 0.025 tolerance of the 0.50 preset.
    auto near = keeby::build_tray_menu(make_state(0.51f, 1.0f, "default", {{"default", "Default", {}}}));
    assert(find(near, keeby::kTrayIdVolume50)->checked);

    // 0.60 is outside tolerance of every preset: none should be checked.
    auto far = keeby::build_tray_menu(make_state(0.60f, 1.0f, "default", {{"default", "Default", {}}}));
    assert(!find(far, keeby::kTrayIdVolume50)->checked);
    assert(!find(far, keeby::kTrayIdVolume75)->checked);
}

void test_width_presets_exact_near_far() {
    auto exact = keeby::build_tray_menu(make_state(1.0f, 1.5f, "default", {{"default", "Default", {}}}));
    const MenuItem* menu = find(exact, keeby::kTrayIdWidthMenu);
    assert(menu && menu->label == "Stereo width: 150%");
    for (const auto& child : menu->children) assert(child.kind == MenuItem::Kind::Radio);
    assert(find(exact, keeby::kTrayIdWidthWide)->checked);
    assert(!find(exact, keeby::kTrayIdWidthNormal)->checked);

    auto near = keeby::build_tray_menu(make_state(1.0f, 1.51f, "default", {{"default", "Default", {}}}));
    assert(find(near, keeby::kTrayIdWidthWide)->checked);

    auto far = keeby::build_tray_menu(make_state(1.0f, 1.2f, "default", {{"default", "Default", {}}}));
    assert(!find(far, keeby::kTrayIdWidthWide)->checked);
    assert(!find(far, keeby::kTrayIdWidthNormal)->checked);
}

void test_switch_menu_default_and_fetch_hint() {
    // Only "default" installed: labeled "Default (built-in)", no brand
    // headers, and the fetch hint appears (single-profile case, unchanged).
    std::vector<PackInfo> only_default = {{"default", "Default", {}}};
    auto default_only = keeby::build_tray_menu(make_state(1.0f, 1.0f, "default", only_default));
    const MenuItem* menu = find(default_only, keeby::kTrayIdSwitchMenu);
    assert(menu && menu->label == "Switches: Default (built-in)");
    assert(menu->children.size() == 2);  // "Default (built-in)" radio + the disabled hint
    assert(menu->children[0].label == "Default (built-in)" && menu->children[0].checked);
    assert(menu->children[0].kind == MenuItem::Kind::Radio);
    const MenuItem* hint = find(default_only, keeby::kTrayIdFetchHint);
    assert(hint && !hint->enabled);
    assert(hint->label == "More sounds: run keeby-fetch-packs");
}

void test_switch_menu_brand_grouping() {
    // Two Cherry packs (grouped, header "Cherry"), one Holy Panda and one NK
    // Cream (singletons -> "Other", sorted by name), plus "default".
    std::vector<PackInfo> profiles = {
        {"default", "Default", {}},         // index 0
        {"cherry-blue", "Cherry MX Blue", {}}, // index 1
        {"cherry-red", "Cherry MX Red", {}},   // index 2
        {"holy-panda", "Holy Panda", {}},      // index 3
        {"nk-cream", "NK Cream", {}},          // index 4
    };
    auto items = keeby::build_tray_menu(make_state(1.0f, 1.0f, "cherry-red", profiles));
    const MenuItem* menu = find(items, keeby::kTrayIdSwitchMenu);
    assert(menu && menu->label == "Switches: Cherry MX Red");

    // Expected order: Default (built-in), [Cherry header, Blue, Red],
    // [Other header, Holy Panda, NK Cream] (sorted alphabetically). No fetch
    // hint: more than one profile is installed.
    assert(menu->children.size() == 7);
    assert(menu->children[0].label == "Default (built-in)" && menu->children[0].kind == MenuItem::Kind::Radio);
    assert(menu->children[1].label == "Cherry" && !menu->children[1].enabled &&
           menu->children[1].kind == MenuItem::Kind::Standard);
    assert(menu->children[2].label == "Cherry MX Blue" && menu->children[2].kind == MenuItem::Kind::Radio &&
           !menu->children[2].checked);
    assert(menu->children[3].label == "Cherry MX Red" && menu->children[3].checked);
    assert(menu->children[4].label == "Other" && !menu->children[4].enabled);
    assert(menu->children[5].label == "Holy Panda");
    assert(menu->children[6].label == "NK Cream");
    assert(find(items, keeby::kTrayIdFetchHint) == nullptr);

    // Ids still address the ORIGINAL profiles[] index, not a group-local one
    // (dispatch_tray_menu_click resolves via that index) -- e.g. "Cherry MX
    // Red" is profiles[2], so its id must be kTrayIdProfileBase + 2 even
    // though it's sorted after "Cherry MX Blue" (profiles[1]) in the menu.
    assert(menu->children[3].id == keeby::kTrayIdProfileBase + 2);
    assert(menu->children[2].id == keeby::kTrayIdProfileBase + 1);
}

void test_separator_settings_and_quit() {
    auto items = keeby::build_tray_menu(make_state(1.0f, 1.0f, "default", {{"default", "Default", {}}}));
    const MenuItem* sep = find(items, keeby::kTrayIdSeparator);
    assert(sep && sep->kind == MenuItem::Kind::Separator);
    const MenuItem* settings = find(items, keeby::kTrayIdSettings);
    assert(settings && settings->label == "Settings…" && settings->kind == MenuItem::Kind::Standard);
    const MenuItem* quit = find(items, keeby::kTrayIdQuit);
    assert(quit && quit->label == "Quit" && quit->kind == MenuItem::Kind::Standard);

    // Keeby's order: Control (sound on, volume), Configure (switches, width,
    // visualizer + its position submenu), App (settings, quit) -- top level,
    // ignoring each submenu's own children.
    assert(items[0].id == keeby::kTrayIdSoundOn);
    assert(items[1].id == keeby::kTrayIdVolumeMenu);
    assert(items[2].id == keeby::kTrayIdSwitchMenu);
    assert(items[3].id == keeby::kTrayIdWidthMenu);
    assert(items[4].id == keeby::kTrayIdVisualizer);
    assert(items[5].id == keeby::kTrayIdVisualizerPositionMenu);
    assert(items[6].id == keeby::kTrayIdSeparator);
    assert(items[7].id == keeby::kTrayIdSettings);
    assert(items[8].id == keeby::kTrayIdQuit);
}

void test_visualizer_checkmark_and_position_submenu() {
    auto state = make_state(1.0f, 1.0f, "default", {{"default", "Default", {}}});
    state.visualizer_enabled = true;
    state.visualizer_position = "bottom-center";
    auto on = keeby::build_tray_menu(state);
    const MenuItem* viz = find(on, keeby::kTrayIdVisualizer);
    assert(viz && viz->kind == MenuItem::Kind::Checkmark && viz->label == "Visualizer" && viz->checked);

    const MenuItem* pos_menu = find(on, keeby::kTrayIdVisualizerPositionMenu);
    assert(pos_menu && pos_menu->label == "Position" && pos_menu->children.size() == 7);
    for (const auto& child : pos_menu->children) assert(child.kind == MenuItem::Kind::Radio);
    assert(find(on, keeby::kTrayIdVisualizerPosBottomCenter)->checked);
    assert(!find(on, keeby::kTrayIdVisualizerPosTopLeft)->checked);
    assert(!find(on, keeby::kTrayIdVisualizerPosTopCenter)->checked);
    assert(!find(on, keeby::kTrayIdVisualizerPosTopRight)->checked);
    assert(!find(on, keeby::kTrayIdVisualizerPosBottomLeft)->checked);
    assert(!find(on, keeby::kTrayIdVisualizerPosBottomRight)->checked);
    assert(!find(on, keeby::kTrayIdVisualizerPosFollowCursor)->checked);

    state.visualizer_position = "follow-cursor";
    auto follow = keeby::build_tray_menu(state);
    assert(find(follow, keeby::kTrayIdVisualizerPosFollowCursor)->checked);
    assert(!find(follow, keeby::kTrayIdVisualizerPosBottomCenter)->checked);

    state.visualizer_enabled = false;
    state.visualizer_position = "top-left";
    auto off = keeby::build_tray_menu(state);
    assert(!find(off, keeby::kTrayIdVisualizer)->checked);
    assert(find(off, keeby::kTrayIdVisualizerPosTopLeft)->checked);
    assert(!find(off, keeby::kTrayIdVisualizerPosBottomCenter)->checked);
}

struct RecordedActions {
    int toggle_calls = 0;
    float volume = -1.0f;
    float width = -1.0f;
    std::string profile;
    int open_settings_calls = 0;
    int quit_calls = 0;
    int toggle_visualizer_calls = 0;
    std::string visualizer_position;
};

TrayActions make_actions(RecordedActions& rec) {
    TrayActions a;
    a.toggle_enabled = [&rec] { ++rec.toggle_calls; };
    a.set_volume = [&rec](float v) { rec.volume = v; };
    a.set_stereo_width = [&rec](float w) { rec.width = w; };
    a.set_profile = [&rec](const std::string& id) { rec.profile = id; };
    a.toggle_visualizer = [&rec] { ++rec.toggle_visualizer_calls; };
    a.set_visualizer_position = [&rec](const std::string& pos) { rec.visualizer_position = pos; };
    a.open_settings = [&rec] { ++rec.open_settings_calls; };
    a.quit = [&rec] { ++rec.quit_calls; };
    return a;
}

void test_dispatch_basic_actions() {
    std::vector<PackInfo> profiles = {{"default", "Default", {}}};
    RecordedActions rec;
    auto actions = make_actions(rec);

    keeby::dispatch_tray_menu_click(keeby::kTrayIdSoundOn, profiles, actions);
    assert(rec.toggle_calls == 1);

    keeby::dispatch_tray_menu_click(keeby::kTrayIdVolume75, profiles, actions);
    assert(rec.volume == 0.75f);

    keeby::dispatch_tray_menu_click(keeby::kTrayIdWidthNarrow, profiles, actions);
    assert(rec.width == 0.5f);

    keeby::dispatch_tray_menu_click(keeby::kTrayIdSettings, profiles, actions);
    assert(rec.open_settings_calls == 1);

    keeby::dispatch_tray_menu_click(keeby::kTrayIdQuit, profiles, actions);
    assert(rec.quit_calls == 1);
}

void test_dispatch_visualizer_actions() {
    std::vector<PackInfo> profiles = {{"default", "Default", {}}};
    RecordedActions rec;
    auto actions = make_actions(rec);

    keeby::dispatch_tray_menu_click(keeby::kTrayIdVisualizer, profiles, actions);
    assert(rec.toggle_visualizer_calls == 1);

    keeby::dispatch_tray_menu_click(keeby::kTrayIdVisualizerPosTopLeft, profiles, actions);
    assert(rec.visualizer_position == "top-left");
    keeby::dispatch_tray_menu_click(keeby::kTrayIdVisualizerPosTopCenter, profiles, actions);
    assert(rec.visualizer_position == "top-center");
    keeby::dispatch_tray_menu_click(keeby::kTrayIdVisualizerPosTopRight, profiles, actions);
    assert(rec.visualizer_position == "top-right");
    keeby::dispatch_tray_menu_click(keeby::kTrayIdVisualizerPosBottomLeft, profiles, actions);
    assert(rec.visualizer_position == "bottom-left");
    keeby::dispatch_tray_menu_click(keeby::kTrayIdVisualizerPosBottomCenter, profiles, actions);
    assert(rec.visualizer_position == "bottom-center");
    keeby::dispatch_tray_menu_click(keeby::kTrayIdVisualizerPosBottomRight, profiles, actions);
    assert(rec.visualizer_position == "bottom-right");
    keeby::dispatch_tray_menu_click(keeby::kTrayIdVisualizerPosFollowCursor, profiles, actions);
    assert(rec.visualizer_position == "follow-cursor");
}

void test_dispatch_ignores_non_actionable_ids() {
    std::vector<PackInfo> profiles = {{"default", "Default", {}}};
    RecordedActions rec;
    auto actions = make_actions(rec);

    keeby::dispatch_tray_menu_click(keeby::kTrayIdVolumeMenu, profiles, actions);  // submenu container
    keeby::dispatch_tray_menu_click(keeby::kTrayIdWidthMenu, profiles, actions);
    keeby::dispatch_tray_menu_click(keeby::kTrayIdSwitchMenu, profiles, actions);
    keeby::dispatch_tray_menu_click(keeby::kTrayIdVisualizerPositionMenu, profiles, actions);  // submenu container
    keeby::dispatch_tray_menu_click(keeby::kTrayIdSeparator, profiles, actions);
    keeby::dispatch_tray_menu_click(keeby::kTrayIdFetchHint, profiles, actions);  // disabled hint
    keeby::dispatch_tray_menu_click(keeby::kTrayIdBrandHeaderBase, profiles, actions);  // disabled brand header
    keeby::dispatch_tray_menu_click(9999, profiles, actions);                    // unknown id

    assert(rec.toggle_calls == 0 && rec.volume == -1.0f && rec.width == -1.0f && rec.profile.empty() &&
           rec.open_settings_calls == 0 && rec.quit_calls == 0);
    assert(rec.toggle_visualizer_calls == 0 && rec.visualizer_position.empty());
}

void test_dispatch_profile_click_uses_build_time_snapshot() {
    // The menu was built from `built_from`; dispatch must resolve the click
    // against that same snapshot even if the "live" list has since changed
    // -- exactly the scenario TrayService::last_profiles_ exists to handle.
    std::vector<PackInfo> built_from = {{"default", "Default", {}}, {"clicky", "Clicky", {}}};
    RecordedActions rec;
    auto actions = make_actions(rec);

    keeby::dispatch_tray_menu_click(keeby::kTrayIdProfileBase + 1, built_from, actions);
    assert(rec.profile == "clicky");

    rec.profile.clear();
    std::vector<PackInfo> changed_live_list = {{"clicky", "Clicky", {}}};  // index 1 no longer exists here
    keeby::dispatch_tray_menu_click(keeby::kTrayIdProfileBase + 1, changed_live_list, actions);
    assert(rec.profile.empty());  // ignored rather than guessing at a stale index
}

} // namespace

int main() {
    test_sound_on_checkbox();
    test_volume_presets_exact_near_far();
    test_width_presets_exact_near_far();
    test_switch_menu_default_and_fetch_hint();
    test_switch_menu_brand_grouping();
    test_separator_settings_and_quit();
    test_visualizer_checkmark_and_position_submenu();
    test_dispatch_basic_actions();
    test_dispatch_visualizer_actions();
    test_dispatch_ignores_non_actionable_ids();
    test_dispatch_profile_click_uses_build_time_snapshot();

    std::puts("tray_menu_test: OK (build_tray_menu ids/labels/checked states, brand grouping, "
              "fetch hint, settings item, visualizer checkmark/position submenu, click dispatch, "
              "profile-snapshot mapping)");
    return 0;
}
