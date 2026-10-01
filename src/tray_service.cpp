#include "tray_service.hpp"

#include <sdbus-c++/sdbus-c++.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <spawn.h>
#include <string_view>
#include <unistd.h>
#include <utility>

#include "engine_controller.hpp"

extern char** environ;

namespace keeby {
namespace {
// org.kde.StatusNotifierWatcher is conventionally both the well-known bus
// name and the D-Bus interface name for this singleton service.
constexpr const char* kWatcherService = "org.kde.StatusNotifierWatcher";
constexpr const char* kWatcherPath = "/StatusNotifierWatcher";
constexpr const char* kItemInterface = "org.kde.StatusNotifierItem";
constexpr const char* kItemPath = "/StatusNotifierItem";
constexpr const char* kMenuPath = "/MenuBar";
constexpr const char* kFetchHintLabel = "More sounds: run keeby-fetch-packs";
constexpr float kPresetEpsilon = 0.025f;  // "close enough to a preset" for volume and width radios

bool near_preset(float value, float target) { return std::fabs(value - target) <= kPresetEpsilon; }

MenuItem make_radio(int32_t id, std::string label, bool checked) {
    MenuItem item;
    item.id = id;
    item.label = std::move(label);
    item.kind = MenuItem::Kind::Radio;
    item.checked = checked;
    return item;
}

std::string percent_label(std::string_view prefix, float fraction) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*s: %d%%", static_cast<int>(prefix.size()), prefix.data(),
                  static_cast<int>(fraction * 100.0f + 0.5f));
    return buf;
}

// First word of a pack's display name is its brand (e.g. "Cherry MX Blue"
// -> "Cherry"); a name with no space is its own one-word brand.
std::string brand_of(const std::string& name) {
    const auto space = name.find(' ');
    return space == std::string::npos ? name : name.substr(0, space);
}

// One (original-index, pack) pair -- the index into `profiles` is what
// dispatch_tray_menu_click needs, so it must survive the brand grouping/sort.
struct IndexedPack {
    std::size_t index;
    const PackInfo* pack;
};

MenuItem make_header(int32_t id, std::string label) {
    MenuItem item;
    item.id = id;
    item.label = std::move(label);
    item.enabled = false;
    return item;
}
} // namespace

std::vector<MenuItem> build_tray_menu(const TrayMenuState& state) {
    MenuItem sound_on;
    sound_on.id = kTrayIdSoundOn;
    sound_on.label = "Sound on";
    sound_on.kind = MenuItem::Kind::Checkmark;
    sound_on.checked = state.enabled;

    MenuItem volume_menu;
    volume_menu.id = kTrayIdVolumeMenu;
    volume_menu.label = percent_label("Volume", state.volume);
    volume_menu.children = {
        make_radio(kTrayIdVolume25, "25%", near_preset(state.volume, 0.25f)),
        make_radio(kTrayIdVolume50, "50%", near_preset(state.volume, 0.50f)),
        make_radio(kTrayIdVolume75, "75%", near_preset(state.volume, 0.75f)),
        make_radio(kTrayIdVolume100, "100%", near_preset(state.volume, 1.00f)),
    };

    MenuItem width_menu;
    width_menu.id = kTrayIdWidthMenu;
    width_menu.label = percent_label("Stereo width", state.stereo_width);
    width_menu.children = {
        make_radio(kTrayIdWidthMono, "Mono 0%", near_preset(state.stereo_width, 0.0f)),
        make_radio(kTrayIdWidthNarrow, "Narrow 50%", near_preset(state.stereo_width, 0.5f)),
        make_radio(kTrayIdWidthNormal, "Normal 100%", near_preset(state.stereo_width, 1.0f)),
        make_radio(kTrayIdWidthWide, "Wide 150%", near_preset(state.stereo_width, 1.5f)),
        make_radio(kTrayIdWidthExtraWide, "Extra wide 200%", near_preset(state.stereo_width, 2.0f)),
    };

    // Keeby groups packs by brand (the first word of the pack's display
    // name): a disabled header per brand with 2+ packs, sorted by brand;
    // any brand with only one pack is collected into a final sorted
    // "Other" group instead of getting its own one-item header. The
    // built-in "default" entry is never grouped -- it's always first,
    // labeled "Default (built-in)". See docs/011-tone-and-control-api.md.
    MenuItem switch_menu;
    switch_menu.id = kTrayIdSwitchMenu;
    std::string current_name = state.current_profile;
    std::optional<IndexedPack> default_entry;
    std::map<std::string, std::vector<IndexedPack>> by_brand; // sorted by key (brand)
    for (std::size_t i = 0; i < state.profiles.size(); ++i) {
        const PackInfo& pack = state.profiles[i];
        if (pack.id == state.current_profile) current_name = pack.id == "default" ? "Default (built-in)" : pack.name;
        if (pack.id == "default") default_entry = IndexedPack{i, &pack};
        else by_brand[brand_of(pack.name)].push_back(IndexedPack{i, &pack});
    }
    switch_menu.label = "Switches: " + current_name;

    auto push_radio = [&](const IndexedPack& p) {
        switch_menu.children.push_back(make_radio(kTrayIdProfileBase + static_cast<int32_t>(p.index), p.pack->name,
                                                    p.pack->id == state.current_profile));
    };
    auto by_name = [](const IndexedPack& a, const IndexedPack& b) { return a.pack->name < b.pack->name; };

    if (default_entry) {
        switch_menu.children.push_back(make_radio(kTrayIdProfileBase + static_cast<int32_t>(default_entry->index),
                                                    "Default (built-in)",
                                                    default_entry->pack->id == state.current_profile));
    }
    int32_t next_header_id = kTrayIdBrandHeaderBase;
    std::vector<IndexedPack> other_singles;
    for (auto& [brand, packs] : by_brand) { // std::map: iterated in sorted brand order
        if (packs.size() < 2) {
            other_singles.push_back(packs.front());
            continue;
        }
        std::sort(packs.begin(), packs.end(), by_name);
        switch_menu.children.push_back(make_header(next_header_id++, brand));
        for (const auto& p : packs) push_radio(p);
    }
    if (!other_singles.empty()) {
        std::sort(other_singles.begin(), other_singles.end(), by_name);
        switch_menu.children.push_back(make_header(next_header_id++, "Other"));
        for (const auto& p : other_singles) push_radio(p);
    }
    if (state.profiles.size() <= 1) {
        MenuItem hint;
        hint.id = kTrayIdFetchHint;
        hint.label = kFetchHintLabel;
        hint.enabled = false;
        switch_menu.children.push_back(std::move(hint));
    }

    MenuItem visualizer_item;
    visualizer_item.id = kTrayIdVisualizer;
    visualizer_item.label = "Visualizer";
    visualizer_item.kind = MenuItem::Kind::Checkmark;
    visualizer_item.checked = state.visualizer_enabled;

    MenuItem position_menu;
    position_menu.id = kTrayIdVisualizerPositionMenu;
    position_menu.label = "Position";
    auto pos_radio = [&](int32_t id, const char* label, const char* value) {
        return make_radio(id, label, state.visualizer_position == value);
    };
    position_menu.children = {
        pos_radio(kTrayIdVisualizerPosTopLeft, "Top left", "top-left"),
        pos_radio(kTrayIdVisualizerPosTopCenter, "Top center", "top-center"),
        pos_radio(kTrayIdVisualizerPosTopRight, "Top right", "top-right"),
        pos_radio(kTrayIdVisualizerPosBottomLeft, "Bottom left", "bottom-left"),
        pos_radio(kTrayIdVisualizerPosBottomCenter, "Bottom center", "bottom-center"),
        pos_radio(kTrayIdVisualizerPosBottomRight, "Bottom right", "bottom-right"),
        pos_radio(kTrayIdVisualizerPosFollowCursor, "Follow cursor", "follow-cursor"),
    };

    MenuItem sep;
    sep.id = kTrayIdSeparator;
    sep.kind = MenuItem::Kind::Separator;

    MenuItem settings;
    settings.id = kTrayIdSettings;
    settings.label = "Settings…";

    MenuItem quit;
    quit.id = kTrayIdQuit;
    quit.label = "Quit";

    // Keeby's order: Control (sound on, volume), Configure (switches,
    // stereo width, visualizer + its position), App (settings, quit).
    return {sound_on, volume_menu, switch_menu, width_menu, visualizer_item, position_menu, sep, settings, quit};
}

void dispatch_tray_menu_click(int32_t id, const std::vector<PackInfo>& profiles, const TrayActions& actions) {
    switch (id) {
        case kTrayIdSoundOn:
            if (actions.toggle_enabled) actions.toggle_enabled();
            return;
        case kTrayIdVolume25:
            if (actions.set_volume) actions.set_volume(0.25f);
            return;
        case kTrayIdVolume50:
            if (actions.set_volume) actions.set_volume(0.50f);
            return;
        case kTrayIdVolume75:
            if (actions.set_volume) actions.set_volume(0.75f);
            return;
        case kTrayIdVolume100:
            if (actions.set_volume) actions.set_volume(1.00f);
            return;
        case kTrayIdWidthMono:
            if (actions.set_stereo_width) actions.set_stereo_width(0.0f);
            return;
        case kTrayIdWidthNarrow:
            if (actions.set_stereo_width) actions.set_stereo_width(0.5f);
            return;
        case kTrayIdWidthNormal:
            if (actions.set_stereo_width) actions.set_stereo_width(1.0f);
            return;
        case kTrayIdWidthWide:
            if (actions.set_stereo_width) actions.set_stereo_width(1.5f);
            return;
        case kTrayIdWidthExtraWide:
            if (actions.set_stereo_width) actions.set_stereo_width(2.0f);
            return;
        case kTrayIdVisualizer:
            if (actions.toggle_visualizer) actions.toggle_visualizer();
            return;
        case kTrayIdVisualizerPosTopLeft:
            if (actions.set_visualizer_position) actions.set_visualizer_position("top-left");
            return;
        case kTrayIdVisualizerPosTopCenter:
            if (actions.set_visualizer_position) actions.set_visualizer_position("top-center");
            return;
        case kTrayIdVisualizerPosTopRight:
            if (actions.set_visualizer_position) actions.set_visualizer_position("top-right");
            return;
        case kTrayIdVisualizerPosBottomLeft:
            if (actions.set_visualizer_position) actions.set_visualizer_position("bottom-left");
            return;
        case kTrayIdVisualizerPosBottomCenter:
            if (actions.set_visualizer_position) actions.set_visualizer_position("bottom-center");
            return;
        case kTrayIdVisualizerPosBottomRight:
            if (actions.set_visualizer_position) actions.set_visualizer_position("bottom-right");
            return;
        case kTrayIdVisualizerPosFollowCursor:
            if (actions.set_visualizer_position) actions.set_visualizer_position("follow-cursor");
            return;
        case kTrayIdSettings:
            if (actions.open_settings) actions.open_settings();
            return;
        case kTrayIdQuit:
            if (actions.quit) actions.quit();
            return;
        default:
            break;
    }
    if (id >= kTrayIdProfileBase) {
        const std::size_t idx = static_cast<std::size_t>(id - kTrayIdProfileBase);
        if (idx < profiles.size() && actions.set_profile) actions.set_profile(profiles[idx].id);
    }
}

namespace {
// Launches keeby-settings via posix_spawnp: argv only (no shell), and this
// process never waitpid()s on it -- main() ignores SIGCHLD (see main.cpp)
// so the child is reaped by the kernel instead of becoming a zombie. A
// failed spawn is reported to stderr only, never fatal to keeby itself.
void spawn_settings() noexcept {
    char program[] = "keeby-settings";
    char* argv[] = {program, nullptr};
    pid_t pid{};
    if (const int rc = ::posix_spawnp(&pid, program, nullptr, nullptr, argv, environ); rc != 0) {
        std::fprintf(stderr, "keeby: failed to launch keeby-settings: %s\n", std::strerror(rc));
    }
}
} // namespace

TrayService::TrayService(sdbus::IConnection& connection, EngineController& controller,
                          std::function<void()> on_quit, std::function<void()> on_change)
    : controller_(controller), on_quit_(std::move(on_quit)), on_change_(std::move(on_change)),
      connection_(connection) {}

TrayService::~TrayService() { stop(); }

std::string TrayService::title() const {
    char buf[80];
    std::snprintf(buf, sizeof buf, "KEEBY - %s, vol %d%% (%s)",
                  controller_.enabled() ? "enabled" : "disabled",
                  static_cast<int>(controller_.master_gain() * 100.0f + 0.5f),
                  controller_.profile().c_str());
    return buf;
}

void TrayService::notify_changed() noexcept {
    if (!object_) return;
    try {
        object_->emitSignal("NewTitle").onInterface(kItemInterface);
    } catch (...) {
        // Best-effort UI refresh only; never let a signal-emission failure
        // propagate out of a control-state change.
    }
}

void TrayService::refresh() {
    if (!menu_) return;
    last_profiles_ = controller_.available_profiles();
    TrayMenuState state{controller_.enabled(), controller_.master_gain(), controller_.stereo_width(),
                         controller_.profile(), last_profiles_};
    state.visualizer_enabled = controller_.visualizer_enabled();
    state.visualizer_position = viz::to_string(controller_.visualizer_position());
    menu_->set_items(build_tray_menu(state));
    notify_changed();
}

void TrayService::handle_menu_click(int32_t id) {
    TrayActions actions;
    actions.toggle_enabled = [this] { controller_.set_enabled(!controller_.enabled()); };
    actions.set_volume = [this](float v) { controller_.set_master_gain(v); };
    actions.set_stereo_width = [this](float w) { controller_.set_stereo_width(w); };
    actions.set_profile = [this](const std::string& profile_id) {
        if (auto res = controller_.set_profile(profile_id); !res) {
            std::fprintf(stderr, "keeby: profile switch failed: %s\n", res.error().c_str());
        }
    };
    actions.toggle_visualizer = [this] { controller_.set_visualizer_enabled(!controller_.visualizer_enabled()); };
    actions.set_visualizer_position = [this](const std::string& pos) {
        if (auto p = viz::position_from_string(pos)) controller_.set_visualizer_position(*p);
    };
    actions.open_settings = [] { spawn_settings(); };
    actions.quit = [this] {
        if (on_quit_) on_quit_();
    };
    dispatch_tray_menu_click(id, last_profiles_, actions);
    refresh();
    if (on_change_) on_change_();
}

std::expected<void, std::string> TrayService::start() {
    const std::string service_name = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-1";
    try {
        connection_.requestName(sdbus::ServiceName{service_name});
        object_ = sdbus::createObject(connection_, sdbus::ObjectPath{kItemPath});
        object_->addVTable(
            sdbus::registerMethod("Activate").implementedAs([this](int32_t, int32_t) {
                controller_.set_enabled(!controller_.enabled());
                refresh();
                if (on_change_) on_change_();
            }),
            sdbus::registerMethod("SecondaryActivate").implementedAs([this](int32_t, int32_t) {
                if (on_quit_) on_quit_();
            }),
            sdbus::registerMethod("Scroll").implementedAs([this](int32_t delta, const std::string&) {
                const float step = delta > 0 ? 0.05f : -0.05f;
                controller_.set_master_gain(controller_.master_gain() + step);
                refresh();
                if (on_change_) on_change_();
            }),
            sdbus::registerMethod("ContextMenu").implementedAs([](int32_t, int32_t) {}),
            sdbus::registerProperty("Category").withGetter([] { return std::string("ApplicationStatus"); }),
            sdbus::registerProperty("Id").withGetter([] { return std::string("keeby"); }),
            sdbus::registerProperty("Title").withGetter([this] { return title(); }),
            sdbus::registerProperty("Status").withGetter([] { return std::string("Active"); }),
            sdbus::registerProperty("IconName").withGetter([] { return std::string("input-keyboard"); }),
            sdbus::registerProperty("ItemIsMenu").withGetter([] { return false; }),
            sdbus::registerProperty("Menu").withGetter([] { return sdbus::ObjectPath{kMenuPath}; }),
            sdbus::registerSignal("NewTitle"))
            .forInterface(kItemInterface);

        menu_ = std::make_unique<DBusMenu>(connection_, sdbus::ObjectPath{kMenuPath},
                                            [this](int32_t id) { handle_menu_click(id); });
    } catch (const std::exception& e) {
        menu_.reset();
        object_.reset();
        return std::unexpected(std::string("tray: failed to publish StatusNotifierItem: ") + e.what());
    }

    refresh();  // populate the menu with the current engine/settings state before anyone can query it

    try {
        auto watcher = sdbus::createProxy(connection_, sdbus::ServiceName{kWatcherService},
                                           sdbus::ObjectPath{kWatcherPath});
        watcher->callMethod("RegisterStatusNotifierItem")
            .onInterface(kWatcherService)
            .withArguments(service_name);
    } catch (const std::exception& e) {
        // No host (e.g. Waybar) is watching yet. Non-fatal: the item stays
        // published on the bus and most hosts pick up existing items when
        // they start; the engine must not depend on a tray host existing.
        std::fprintf(stderr,
                      "keeby: tray: no StatusNotifierWatcher host registered (%s); "
                      "item is published, will show once a compatible host starts\n",
                      e.what());
    }
    return {};
}

void TrayService::stop() {
    menu_.reset();
    object_.reset();
}

} // namespace keeby
