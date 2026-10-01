#include "dbus_menu.hpp"

#include <utility>

namespace keeby {
namespace {
constexpr const char* kMenuInterface = "com.canonical.dbusmenu";
constexpr const char* kIdNotFoundError = "com.canonical.dbusmenu.Error.IdNotFound";

const MenuItem* find_item(const MenuItem& node, int32_t id) {
    if (node.id == id) return &node;
    for (const auto& child : node.children) {
        if (const MenuItem* hit = find_item(child, id)) return hit;
    }
    return nullptr;
}

MenuLayout layout_of(const MenuItem& item, int32_t depth, const std::vector<std::string>& names) {
    std::vector<sdbus::Variant> children;
    if (depth != 0) {
        children.reserve(item.children.size());
        for (const auto& child : item.children) {
            children.emplace_back(layout_of(child, depth < 0 ? -1 : depth - 1, names));
        }
    }
    return MenuLayout{item.id, menu_item_properties(item, names), std::move(children)};
}
} // namespace

std::map<std::string, sdbus::Variant> menu_item_properties(const MenuItem& item,
                                                            const std::vector<std::string>& property_names) {
    std::map<std::string, sdbus::Variant> all;
    if (item.kind == MenuItem::Kind::Separator) {
        all.emplace("type", sdbus::Variant{std::string("separator")});
    } else {
        if (!item.label.empty()) all.emplace("label", sdbus::Variant{item.label});
        all.emplace("enabled", sdbus::Variant{item.enabled});
        all.emplace("visible", sdbus::Variant{item.visible});
        if (item.kind == MenuItem::Kind::Checkmark || item.kind == MenuItem::Kind::Radio) {
            all.emplace("toggle-type",
                        sdbus::Variant{std::string(item.kind == MenuItem::Kind::Checkmark ? "checkmark" : "radio")});
            all.emplace("toggle-state", sdbus::Variant{int32_t{item.checked ? 1 : 0}});
        }
        if (!item.children.empty()) all.emplace("children-display", sdbus::Variant{std::string("submenu")});
    }

    if (property_names.empty()) return all;
    std::map<std::string, sdbus::Variant> filtered;
    for (const auto& name : property_names) {
        auto it = all.find(name);
        if (it != all.end()) filtered.emplace(*it);
    }
    return filtered;
}

MenuLayout build_menu_layout(const MenuItem& root, int32_t parent_id, int32_t recursion_depth,
                             const std::vector<std::string>& property_names) {
    const MenuItem* item = find_item(root, parent_id);
    if (!item) {
        throw sdbus::Error(sdbus::Error::Name{kIdNotFoundError},
                            "com.canonical.dbusmenu: no item with id " + std::to_string(parent_id));
    }
    return layout_of(*item, recursion_depth, property_names);
}

DBusMenu::DBusMenu(sdbus::IConnection& connection, sdbus::ObjectPath path, ClickHandler on_click)
    : on_click_(std::move(on_click)) {
    object_ = sdbus::createObject(connection, std::move(path));
    object_->addVTable(
        sdbus::registerMethod("GetLayout")
            .implementedAs([this](int32_t parentId, int32_t recursionDepth,
                                   const std::vector<std::string>& propertyNames) {
                return get_layout(parentId, recursionDepth, propertyNames);
            }),
        sdbus::registerMethod("GetGroupProperties")
            .implementedAs([this](const std::vector<int32_t>& ids, const std::vector<std::string>& propertyNames) {
                return get_group_properties(ids, propertyNames);
            }),
        sdbus::registerMethod("GetProperty")
            .implementedAs([this](int32_t id, const std::string& name) { return get_property(id, name); }),
        // No .withNoReply(): a host that calls Event and waits for a reply
        // (some do) would time out waiting for one that never comes. An
        // empty reply once on_event() returns is always safe to send.
        sdbus::registerMethod("Event")
            .implementedAs([this](int32_t id, const std::string& eventId, const sdbus::Variant& data,
                                   uint32_t timestamp) { on_event(id, eventId, data, timestamp); }),
        sdbus::registerMethod("EventGroup")
            .implementedAs(
                [this](const std::vector<sdbus::Struct<int32_t, std::string, sdbus::Variant, uint32_t>>& events) {
                    return on_event_group(events);
                }),
        sdbus::registerMethod("AboutToShow").implementedAs([this](int32_t id) { return about_to_show(id); }),
        sdbus::registerMethod("AboutToShowGroup")
            .implementedAs([this](const std::vector<int32_t>& ids) { return about_to_show_group(ids); }),
        sdbus::registerSignal("LayoutUpdated").withParameters<uint32_t, int32_t>(),
        // Declared for interface completeness only: this minimal server has
        // no per-property-update or server-initiated-activation entry
        // point, so these two are never actually emitted.
        sdbus::registerSignal("ItemsPropertiesUpdated")
            .withParameters<std::vector<sdbus::Struct<int32_t, std::map<std::string, sdbus::Variant>>>,
                            std::vector<sdbus::Struct<int32_t, std::vector<std::string>>>>(),
        sdbus::registerSignal("ItemActivationRequested").withParameters<int32_t, uint32_t>(),
        sdbus::registerProperty("Version").withGetter([] { return uint32_t{3}; }),
        sdbus::registerProperty("TextDirection").withGetter([] { return std::string("ltr"); }),
        sdbus::registerProperty("Status").withGetter([] { return std::string("normal"); }),
        sdbus::registerProperty("IconThemePath").withGetter([] { return std::vector<std::string>{}; }))
        .forInterface(kMenuInterface);
}

DBusMenu::~DBusMenu() = default;

void DBusMenu::set_items(std::vector<MenuItem> top_level) {
    uint32_t new_revision;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        root_.children = std::move(top_level);
        new_revision = ++revision_;
    }
    try {
        object_->emitSignal("LayoutUpdated").onInterface(kMenuInterface).withArguments(new_revision, int32_t{0});
    } catch (...) {
        // Best-effort UI refresh only (mirrors TrayService::notify_changed).
    }
}

uint32_t DBusMenu::revision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return revision_;
}

std::tuple<uint32_t, MenuLayout> DBusMenu::get_layout(int32_t parent_id, int32_t recursion_depth,
                                                       const std::vector<std::string>& property_names) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {revision_, build_menu_layout(root_, parent_id, recursion_depth, property_names)};
}

std::vector<sdbus::Struct<int32_t, std::map<std::string, sdbus::Variant>>> DBusMenu::get_group_properties(
    const std::vector<int32_t>& ids, const std::vector<std::string>& property_names) const {
    std::vector<sdbus::Struct<int32_t, std::map<std::string, sdbus::Variant>>> result;
    std::lock_guard<std::mutex> lock(mutex_);
    for (int32_t id : ids) {
        if (const MenuItem* item = find_item(root_, id)) {
            result.emplace_back(id, menu_item_properties(*item, property_names));
        }
    }
    return result;
}

sdbus::Variant DBusMenu::get_property(int32_t id, const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const MenuItem* item = find_item(root_, id);
    if (!item) {
        throw sdbus::Error(sdbus::Error::Name{kIdNotFoundError},
                            "com.canonical.dbusmenu: no item with id " + std::to_string(id));
    }
    auto props = menu_item_properties(*item, {name});
    auto it = props.find(name);
    return it != props.end() ? it->second : sdbus::Variant{};
}

void DBusMenu::on_event(int32_t id, const std::string& event_id, const sdbus::Variant&, uint32_t) {
    bool should_click;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const MenuItem* item = find_item(root_, id);
        should_click = item && item->enabled && event_id == "clicked";
    }
    if (!should_click || !on_click_) return;
    try {
        on_click_(id);
    } catch (...) {
        // A user-supplied handler must never throw across D-Bus.
    }
}

std::vector<int32_t> DBusMenu::on_event_group(
    const std::vector<sdbus::Struct<int32_t, std::string, sdbus::Variant, uint32_t>>& events) {
    std::vector<int32_t> errors;
    std::vector<int32_t> to_click;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& e : events) {
            const int32_t id = std::get<0>(e);
            const MenuItem* item = find_item(root_, id);
            if (!item) {
                errors.push_back(id);
                continue;
            }
            if (item->enabled && std::get<1>(e) == "clicked") to_click.push_back(id);
        }
    }
    if (on_click_) {
        for (int32_t id : to_click) {
            try {
                on_click_(id);
            } catch (...) {
            }
        }
    }
    return errors;
}

bool DBusMenu::about_to_show(int32_t) const {
    return false;  // no lazily-populated submenus in this minimal server
}

std::tuple<std::vector<int32_t>, std::vector<int32_t>> DBusMenu::about_to_show_group(
    const std::vector<int32_t>& ids) const {
    std::vector<int32_t> errors;
    std::vector<int32_t> updates_needed;  // always empty: see about_to_show()
    std::lock_guard<std::mutex> lock(mutex_);
    for (int32_t id : ids) {
        if (!find_item(root_, id)) errors.push_back(id);
    }
    return {updates_needed, errors};
}

} // namespace keeby
