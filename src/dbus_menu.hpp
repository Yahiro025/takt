#pragma once

#include <sdbus-c++/sdbus-c++.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace keeby {

struct MenuItem {
    enum class Kind { Standard, Separator, Checkmark, Radio };
    int32_t id = 0;                  // unique, > 0 (0 is reserved for the implicit root)
    std::string label;
    Kind kind = Kind::Standard;
    bool enabled = true;
    bool visible = true;
    bool checked = false;            // Checkmark/Radio only
    std::vector<MenuItem> children;  // non-empty => submenu
};

// D-Bus (ia{sv}av) layout struct. Each child is boxed in a Variant holding
// its own MenuLayout, since sdbus-c++ needs a concrete, non-recursive C++
// type to describe a recursive D-Bus type.
using MenuLayout = sdbus::Struct<int32_t, std::map<std::string, sdbus::Variant>, std::vector<sdbus::Variant>>;

// Property map for one item (dbusmenu property keys: type, label, enabled,
// visible, toggle-type, toggle-state, children-display). Empty
// property_names returns everything; non-empty filters to those keys.
// Exposed alongside build_menu_layout so this logic is unit-testable
// without a live D-Bus connection; DBusMenu's own method handlers call
// these same functions.
std::map<std::string, sdbus::Variant> menu_item_properties(
    const MenuItem& item, const std::vector<std::string>& property_names = {});

// Builds the layout for the item `parent_id` within `root`'s tree.
// recursion_depth: -1 = all levels, 0 = just this item (empty children
// array), n = n levels of children.
// Throws sdbus::Error with name "com.canonical.dbusmenu.Error.IdNotFound"
// if parent_id is not found anywhere in the tree.
MenuLayout build_menu_layout(const MenuItem& root, int32_t parent_id, int32_t recursion_depth,
                             const std::vector<std::string>& property_names = {});

// Exposes com.canonical.dbusmenu on an existing connection (meant to be
// shared with a StatusNotifierItem so a host can reach the menu through the
// item's Menu property). Does not own or drive the connection's event loop.
class DBusMenu {
public:
    using ClickHandler = std::function<void(int32_t id)>;

    // `connection` must outlive this object; its event loop must be running
    // (or about to start) for method calls to be served.
    DBusMenu(sdbus::IConnection& connection, sdbus::ObjectPath path, ClickHandler on_click);
    ~DBusMenu();
    DBusMenu(const DBusMenu&) = delete;
    DBusMenu& operator=(const DBusMenu&) = delete;

    // May be called from any thread, concurrently with the connection's
    // event-loop thread serving method calls: root_/revision_ are
    // mutex-guarded, and sdbus-c++ documents IObject signal emission as
    // thread-safe by design (IObject.h: "creating and emitting signals is
    // thread-safe"), so LayoutUpdated is emitted directly, no marshaling
    // onto the event-loop thread required.
    void set_items(std::vector<MenuItem> top_level);
    uint32_t revision() const;

private:
    std::tuple<uint32_t, MenuLayout> get_layout(int32_t parent_id, int32_t recursion_depth,
                                                 const std::vector<std::string>& property_names) const;
    std::vector<sdbus::Struct<int32_t, std::map<std::string, sdbus::Variant>>> get_group_properties(
        const std::vector<int32_t>& ids, const std::vector<std::string>& property_names) const;
    sdbus::Variant get_property(int32_t id, const std::string& name) const;
    void on_event(int32_t id, const std::string& event_id, const sdbus::Variant& data, uint32_t timestamp);
    std::vector<int32_t> on_event_group(
        const std::vector<sdbus::Struct<int32_t, std::string, sdbus::Variant, uint32_t>>& events);
    bool about_to_show(int32_t id) const;
    std::tuple<std::vector<int32_t>, std::vector<int32_t>> about_to_show_group(
        const std::vector<int32_t>& ids) const;

    mutable std::mutex mutex_;  // guards root_ and revision_
    MenuItem root_{};           // id 0; children are the top-level items from set_items
    uint32_t revision_ = 0;
    ClickHandler on_click_;     // set once at construction; safe to read unlocked afterward
    std::unique_ptr<sdbus::IObject> object_;
};

} // namespace keeby
