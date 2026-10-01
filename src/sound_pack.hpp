#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "sample_mixer.hpp"

namespace keeby {

// One discoverable sound pack (see docs/008-step-2.7-sound-packs.md).
// `dir` is empty for the synthetic built-in "default" entry, which is
// backed by load_default_bank rather than a pack directory.
struct PackInfo {
    std::string id;
    std::string name;
    std::filesystem::path dir;
};

// Off-RT only. Parses dir/config.json -- Mechvibes v1/v2, which also
// serves as KEEBY's own recording format -- and decodes/resamples/
// normalizes every referenced audio file into a SoundBank ready for
// SampleMixer/AudioBoundary::swap_bank. The directory is treated as
// untrusted input: never throws, and a malformed pack, oversize config,
// escaping path, or unsupported version (v3/v4) is a clean
// std::unexpected, never a crash.
//
// Tolerant of two real-world Mechvibes quirks instead of hard-erroring:
// a `null` define value (silent in single mode and v1 multi; in v2 multi
// it falls back to the pack's default sound/soundup like an undefined
// key), and a per-key file that resolves but doesn't exist on disk (that
// key/direction is skipped -- v2 multi falls back to the default, single
// mode/v1 multi/no-default just stays silent). Still hard errors: a
// missing single-mode sprite, a missing v2 default `sound`/`soundup` when
// referenced, any other resolution failure (absolute path, `..`, symlink
// escape, non-regular file used as the reason isn't "doesn't exist"), and
// a pack that ends up with no press sound for any key at all.
//
// If `warnings` is non-null, non-fatal diagnostics (skipped unknown key
// codes, skipped missing per-key files -- repeats of the same file
// collapsed to one entry) are appended to it instead of being printed;
// the caller is then responsible for printing them (prefixed "keeby: ").
// If `warnings` is null, load_pack prints them itself.
std::expected<SoundBank, std::string> load_pack(const std::filesystem::path& dir,
                                                 std::vector<std::string>* warnings = nullptr);

// Off-RT, cheap (no audio decoding, id/name only). Lists every pack under
// $XDG_DATA_HOME/keeby/packs (default ~/.local/share/keeby/packs), then
// each $XDG_DATA_DIRS entry + /keeby/packs (default
// /usr/local/share:/usr/share) -- first id wins -- plus the built-in
// "default", which always appears first. A pack directory literally
// named "default" is reserved and skipped with a warning.
std::vector<PackInfo> list_packs();

} // namespace keeby
