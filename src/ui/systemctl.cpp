#include "systemctl.hpp"

#include <glib.h>

#include <cstdio>

namespace keeby::ui {

bool systemctl_user_is_enabled(const std::string& unit) {
    const char* argv[] = {"systemctl", "--user", "is-enabled", unit.c_str(), nullptr};
    gchar* out = nullptr;
    gint exit_status = 0;
    GError* error = nullptr;
    const gboolean ok = g_spawn_sync(nullptr, const_cast<char**>(argv), nullptr,
                                      static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL),
                                      nullptr, nullptr, &out, nullptr, &exit_status, &error);
    if (error) {
        std::fprintf(stderr, "keeby-settings: systemctl is-enabled failed: %s\n", error->message);
        g_error_free(error);
    }
    bool enabled = false;
    if (ok && out) enabled = g_str_has_prefix(out, "enabled");
    if (out) g_free(out);
    return enabled;
}

void systemctl_user_async(const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    argv.push_back("systemctl");
    argv.push_back("--user");
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    GError* error = nullptr;
    if (!g_spawn_async(nullptr, const_cast<char**>(argv.data()), nullptr,
                        static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                                                  G_SPAWN_STDERR_TO_DEV_NULL),
                        nullptr, nullptr, nullptr, &error)) {
        if (error) {
            std::fprintf(stderr, "keeby-settings: systemctl spawn failed: %s\n", error->message);
            g_error_free(error);
        }
    }
}

} // namespace keeby::ui
