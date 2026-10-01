#pragma once

#include <string>
#include <vector>

namespace keeby::ui {

// `systemctl --user is-enabled <unit>` via g_spawn (argv, no shell), sync:
// only called once per tab build, so a brief blocking spawn is acceptable.
// Returns false on any spawn/parse failure as well as "disabled".
bool systemctl_user_is_enabled(const std::string& unit);

// `systemctl --user <args...>` via g_spawn (argv, no shell), fire-and-forget
// (e.g. enable/disable/start); errors are not surfaced beyond a log line.
void systemctl_user_async(const std::vector<std::string>& args);

} // namespace keeby::ui
