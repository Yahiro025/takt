#pragma once

#include <cstdlib>
#include <string>

namespace keeby::paths {

// Built-in sound asset directory (click/release WAVs baked into "default").
// $KEEBY_ASSET_DIR env var wins when set -- dev iteration, and how the Arch
// package's check() points tests at the source tree before anything is
// installed -- else the compiled-in default: the source assets/ dir in a
// dev build, /usr/share/keeby/assets once packaged (see CMakeLists.txt's
// KEEBY_ASSET_DIR cache variable, compiled in as KEEBY_ASSET_DIR_DEFAULT).
inline std::string asset_dir() {
    if (const char* env = std::getenv("KEEBY_ASSET_DIR"); env && *env) return env;
    return KEEBY_ASSET_DIR_DEFAULT;
}

} // namespace keeby::paths
