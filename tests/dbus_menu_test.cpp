#include "dbus_menu.hpp"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>

// Pure-model tests exercise menu_item_properties/build_menu_layout directly
// (no bus needed). The live round-trip test needs a real session bus, so it
// only runs when DBUS_SESSION_BUS_ADDRESS is set; otherwise this reports
// SKIP and exits 77 (ctest SKIP_RETURN_CODE, set in CMakeLists.txt).
namespace {
using keeby::MenuItem;
using keeby::MenuLayout;

MenuItem make_tree() {
    MenuItem quit;
    quit.id = 2;
    quit.label = "Quit";

    MenuItem file;
    file.id = 1;
    file.label = "File";
    file.children = {quit};

    MenuItem sep;
    sep.id = 3;
    sep.kind = MenuItem::Kind::Separator;

    MenuItem sound;
    sound.id = 4;
    sound.label = "Sound";
    sound.kind = MenuItem::Kind::Checkmark;
    sound.checked = true;

    MenuItem root;
    root.id = 0;
    root.children = {file, sep, sound};
    return root;
}

void test_layout_depths() {
    const MenuItem root = make_tree();

    auto l0 = keeby::build_menu_layout(root, 0, 0, {});
    assert(std::get<0>(l0) == 0);
    assert(std::get<2>(l0).empty());  // depth 0: no children array contents

    auto l1 = keeby::build_menu_layout(root, 0, 1, {});
    assert(std::get<2>(l1).size() == 3);
    auto file1 = std::get<2>(l1)[0].get<MenuLayout>();
    assert(std::get<0>(file1) == 1);
    assert(std::get<2>(file1).empty());  // depth exhausted: Quit not expanded

    auto lAll = keeby::build_menu_layout(root, 0, -1, {});
    auto fileAll = std::get<2>(lAll)[0].get<MenuLayout>();
    assert(std::get<2>(fileAll).size() == 1);
    auto quit = std::get<2>(fileAll)[0].get<MenuLayout>();
    assert(std::get<0>(quit) == 2);

    // GetLayout targeting a leaf directly: its own layout, no children.
    auto leaf = keeby::build_menu_layout(root, 2, -1, {});
    assert(std::get<0>(leaf) == 2);
    assert(std::get<2>(leaf).empty());
}

void test_unknown_parent_error() {
    const MenuItem root = make_tree();
    bool threw = false;
    try {
        keeby::build_menu_layout(root, 999, -1, {});
    } catch (const sdbus::Error& e) {
        threw = true;
        assert(e.getName() == "com.canonical.dbusmenu.Error.IdNotFound");
    }
    assert(threw);
}

void test_properties() {
    MenuItem leaf;
    leaf.id = 1;
    leaf.label = "File";

    auto props_leaf = keeby::menu_item_properties(leaf, {});
    assert(props_leaf.at("label").get<std::string>() == "File");
    assert(props_leaf.at("enabled").get<bool>() == true);
    assert(props_leaf.at("visible").get<bool>() == true);
    assert(!props_leaf.count("children-display"));
    assert(!props_leaf.count("type"));

    MenuItem child;
    child.id = 2;
    child.label = "Quit";
    MenuItem parent;
    parent.id = 1;
    parent.label = "File";
    parent.children = {child};
    auto props_submenu = keeby::menu_item_properties(parent, {});
    assert(props_submenu.at("children-display").get<std::string>() == "submenu");

    MenuItem sep;
    sep.id = 3;
    sep.kind = MenuItem::Kind::Separator;
    auto props_sep = keeby::menu_item_properties(sep, {});
    assert(props_sep.size() == 1);
    assert(props_sep.at("type").get<std::string>() == "separator");

    MenuItem check;
    check.id = 4;
    check.label = "Sound";
    check.kind = MenuItem::Kind::Checkmark;
    check.checked = true;
    auto props_check = keeby::menu_item_properties(check, {});
    assert(props_check.at("toggle-type").get<std::string>() == "checkmark");
    assert(props_check.at("toggle-state").get<int32_t>() == 1);

    MenuItem radio;
    radio.id = 5;
    radio.label = "A";
    radio.kind = MenuItem::Kind::Radio;
    radio.checked = false;
    auto props_radio = keeby::menu_item_properties(radio, {});
    assert(props_radio.at("toggle-type").get<std::string>() == "radio");
    assert(props_radio.at("toggle-state").get<int32_t>() == 0);

    auto filtered = keeby::menu_item_properties(leaf, {"label"});
    assert(filtered.size() == 1);
    assert(filtered.at("label").get<std::string>() == "File");

    auto filtered_missing = keeby::menu_item_properties(leaf, {"nonexistent"});
    assert(filtered_missing.empty());
}

bool has_session_bus() {
    const char* addr = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    return addr != nullptr && *addr != '\0';
}

void test_live_roundtrip() {
    constexpr const char* kServiceName = "keeby.test.dbusmenu";
    constexpr const char* kMenuPath = "/MenuBar";
    constexpr const char* kMenuInterface = "com.canonical.dbusmenu";

    std::mutex click_mutex;
    std::condition_variable click_cv;
    int32_t clicked_id = -1;

    auto server_conn = sdbus::createSessionBusConnection();
    server_conn->requestName(sdbus::ServiceName{kServiceName});
    server_conn->enterEventLoopAsync();

    keeby::DBusMenu menu(*server_conn, sdbus::ObjectPath{kMenuPath}, [&](int32_t id) {
        std::lock_guard<std::mutex> lk(click_mutex);
        clicked_id = id;
        click_cv.notify_one();
    });

    MenuItem quit;
    quit.id = 2;
    quit.label = "Quit";
    MenuItem file;
    file.id = 1;
    file.label = "File";
    file.children = {quit};
    menu.set_items({file});
    assert(menu.revision() == 1);

    auto client_conn = sdbus::createSessionBusConnection();
    client_conn->enterEventLoopAsync();
    auto proxy = sdbus::createProxy(*client_conn, sdbus::ServiceName{kServiceName}, sdbus::ObjectPath{kMenuPath});

    std::mutex signal_mutex;
    std::condition_variable signal_cv;
    uint32_t signaled_revision = 0;
    bool signaled = false;
    proxy->uponSignal("LayoutUpdated").onInterface(kMenuInterface).call([&](uint32_t revision, int32_t) {
        std::lock_guard<std::mutex> lk(signal_mutex);
        signaled_revision = revision;
        signaled = true;
        signal_cv.notify_one();
    });

    uint32_t revision{};
    MenuLayout layout;
    proxy->callMethod("GetLayout")
        .onInterface(kMenuInterface)
        .withArguments(int32_t{0}, int32_t{-1}, std::vector<std::string>{})
        .storeResultsTo(revision, layout);
    assert(revision == 1);
    assert(std::get<2>(layout).size() == 1);  // one top-level item: File

    std::vector<sdbus::Struct<int32_t, std::map<std::string, sdbus::Variant>>> group_props;
    proxy->callMethod("GetGroupProperties")
        .onInterface(kMenuInterface)
        .withArguments(std::vector<int32_t>{1, 2}, std::vector<std::string>{})
        .storeResultsTo(group_props);
    assert(group_props.size() == 2);

    sdbus::Variant label;
    proxy->callMethod("GetProperty")
        .onInterface(kMenuInterface)
        .withArguments(int32_t{1}, std::string("label"))
        .storeResultsTo(label);
    assert(label.get<std::string>() == "File");

    bool need_update = true;
    proxy->callMethod("AboutToShow")
        .onInterface(kMenuInterface)
        .withArguments(int32_t{1})
        .storeResultsTo(need_update);
    assert(need_update == false);

    proxy->callMethod("Event")
        .onInterface(kMenuInterface)
        .withArguments(int32_t{2}, std::string("clicked"), sdbus::Variant{std::string{}}, uint32_t{0})
        .dontExpectReply();

    {
        std::unique_lock<std::mutex> lk(click_mutex);
        bool ok = click_cv.wait_for(lk, std::chrono::seconds(2), [&] { return clicked_id != -1; });
        assert(ok);
        assert(clicked_id == 2);
    }

    MenuItem quit_only;
    quit_only.id = 2;
    quit_only.label = "Quit";
    menu.set_items({quit_only});
    {
        std::unique_lock<std::mutex> lk(signal_mutex);
        bool ok = signal_cv.wait_for(lk, std::chrono::seconds(2), [&] { return signaled; });
        assert(ok);
        assert(signaled_revision == 2);
    }
}
} // namespace

int main() {
    test_layout_depths();
    test_unknown_parent_error();
    test_properties();

    if (!has_session_bus()) {
        std::puts("dbus_menu_test: SKIP (no DBUS_SESSION_BUS_ADDRESS)");
        return 77;
    }
    test_live_roundtrip();

    std::puts(
        "dbus_menu_test: OK (layout depths, unknown-parent error, property maps, "
        "filtering, live GetLayout/GetGroupProperties/GetProperty/Event/AboutToShow/LayoutUpdated)");
    return 0;
}
