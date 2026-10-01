#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "dbus_menu.hpp"
#include "sound_pack.hpp"
#include "visualizer_wire.hpp"

namespace keeby {

class EngineController;

// Everything build_tray_menu() needs to render the tray menu. `profiles` is
// a snapshot of available_profiles() taken at build time -- the same
// snapshot must be handed to dispatch_tray_menu_click() so a profile click
// resolves correctly even if the live list changes in between.
struct TrayMenuState {
    bool enabled = false;
    float volume = 0.0f;        // [0, 1]
    float stereo_width = 0.0f;  // [0, 2]
    std::string current_profile;
    std::vector<PackInfo> profiles;
    bool visualizer_enabled = false;
    std::string visualizer_position = "bottom-center";
};

// Stable menu item ids, documented in docs/009-step-2.8-desktop-control.md
// and referenced directly by tests. kTrayIdProfileBase + i addresses
// TrayMenuState::profiles[i].
inline constexpr int32_t kTrayIdSoundOn = 1;
inline constexpr int32_t kTrayIdVolumeMenu = 10;
inline constexpr int32_t kTrayIdVolume25 = 11;
inline constexpr int32_t kTrayIdVolume50 = 12;
inline constexpr int32_t kTrayIdVolume75 = 13;
inline constexpr int32_t kTrayIdVolume100 = 14;
inline constexpr int32_t kTrayIdWidthMenu = 20;
inline constexpr int32_t kTrayIdWidthMono = 21;
inline constexpr int32_t kTrayIdWidthNarrow = 22;
inline constexpr int32_t kTrayIdWidthNormal = 23;
inline constexpr int32_t kTrayIdWidthWide = 24;
inline constexpr int32_t kTrayIdWidthExtraWide = 25;
inline constexpr int32_t kTrayIdSwitchMenu = 30;
inline constexpr int32_t kTrayIdFetchHint = 31;
inline constexpr int32_t kTrayIdVisualizer = 50;
inline constexpr int32_t kTrayIdVisualizerPositionMenu = 51;
inline constexpr int32_t kTrayIdVisualizerPosTopLeft = 52;
inline constexpr int32_t kTrayIdVisualizerPosTopCenter = 53;
inline constexpr int32_t kTrayIdVisualizerPosTopRight = 54;
inline constexpr int32_t kTrayIdVisualizerPosBottomLeft = 55;
inline constexpr int32_t kTrayIdVisualizerPosBottomCenter = 56;
inline constexpr int32_t kTrayIdVisualizerPosBottomRight = 57;
inline constexpr int32_t kTrayIdVisualizerPosFollowCursor = 58;
inline constexpr int32_t kTrayIdProfileBase = 100;
// Disabled brand-header items inside the Switches submenu (e.g. "Cherry",
// "Other"), one per group; assigned sequentially from this base each time
// build_tray_menu() runs. Never clickable -- see dispatch_tray_menu_click.
inline constexpr int32_t kTrayIdBrandHeaderBase = 1000;
inline constexpr int32_t kTrayIdSeparator = 40;
inline constexpr int32_t kTrayIdSettings = 42;
inline constexpr int32_t kTrayIdQuit = 41;

// Pure and unit-testable: no D-Bus connection, no EngineController.
std::vector<MenuItem> build_tray_menu(const TrayMenuState& state);

// Callbacks a menu click resolves to. Presets pass absolute values (not
// deltas); `set_profile` receives the pack id looked up from the snapshot
// dispatch_tray_menu_click() was given, not a freshly-queried list.
struct TrayActions {
    std::function<void()> toggle_enabled;
    std::function<void(float)> set_volume;
    std::function<void(float)> set_stereo_width;
    std::function<void(const std::string&)> set_profile;
    std::function<void()> toggle_visualizer;
    std::function<void(const std::string&)> set_visualizer_position;
    // Launches the keeby-settings GTK window (posix_spawnp, detached).
    std::function<void()> open_settings;
    std::function<void()> quit;
};

// Resolves one DBusMenu click id to at most one TrayActions callback. Ids
// with no action (submenu containers, the separator, the disabled fetch
// hint, or a profile index no longer present in `profiles`) are ignored.
void dispatch_tray_menu_click(int32_t id, const std::vector<PackInfo>& profiles, const TrayActions& actions);

// Exposes KEEBY through the freedesktop StatusNotifierItem D-Bus protocol
// (org.kde.StatusNotifierItem) — the mechanism Wayland compositors rely on
// in place of a native systray; a compatible host (Waybar, a
// StatusNotifierHost-capable shell, etc.) renders it. No GTK/Qt/AppIndicator
// dependency: this talks D-Bus directly via sdbus-c++. Also owns a
// com.canonical.dbusmenu object (DBusMenu) at /MenuBar, advertised through
// the SNI's Menu property, so hosts that support it show a full menu.
//
// Every handler runs on the shared connection's event-loop thread; nothing
// here ever touches PipeWire, libevdev, or the audio callback thread.
class TrayService {
public:
    // `connection` and `controller` must outlive this object; so must the
    // callbacks. `connection`'s event loop must be started by the caller
    // (enterEventLoopAsync()) only after start() returns, once every other
    // D-Bus object (e.g. ControlService) has also been exported.
    TrayService(sdbus::IConnection& connection, EngineController& controller, std::function<void()> on_quit,
                std::function<void()> on_change);
    ~TrayService();
    TrayService(const TrayService&) = delete;
    TrayService& operator=(const TrayService&) = delete;

    // Publishes the StatusNotifierItem and its /MenuBar menu on the shared
    // connection. A missing/absent tray host (no StatusNotifierWatcher
    // owner) is NOT treated as failure, see the class comment; this only
    // fails if publishing the objects themselves fails (e.g. name clash).
    std::expected<void, std::string> start();
    void stop();

    // Rebuilds the menu from current engine state and re-emits the title.
    // Called internally after Activate/Scroll/menu clicks, and by main()
    // after every ControlService call, so all three surfaces agree.
    void refresh();

private:
    std::string title() const;
    void notify_changed() noexcept;  // emits NewTitle after a control change
    void handle_menu_click(int32_t id);

    EngineController& controller_;
    std::function<void()> on_quit_;
    std::function<void()> on_change_;
    sdbus::IConnection& connection_;
    std::unique_ptr<sdbus::IObject> object_;
    std::unique_ptr<DBusMenu> menu_;
    std::vector<PackInfo> last_profiles_;  // snapshot the current menu was built from
};

} // namespace keeby
