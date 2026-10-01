#!/usr/bin/env python3
"""Copy hash-checked Keeby 1.10.4 sound folders into local user packs.

The source tree must be extracted from the official DMG. This script does not
download, transcode, or add recordings to the source tree or package. It writes
only to the user's sound-pack directory and skips every pack ID that exists.
"""

from __future__ import annotations

import argparse
import ctypes
import errno
import hashlib
import json
import math
import os
import re
import shutil
import stat
import sys
import tempfile
import wave
from pathlib import Path, PurePosixPath


MANIFEST_PATH = Path(__file__).with_name("keeby-native-sound-manifest.json")
WEB_BROWN_CONFIG_PATH = Path(__file__).with_name("keeby-web-keychron-k2-max-brown.json")
MAX_FILE_BYTES = 1_000_000
MAX_PACK_BYTES = 16_000_000
WEB_BROWN_BYTES = 1_941_031
WEB_BROWN_SHA256 = "10390a4325fe218b005c509cac28c90bdd21c16bfcc303fe57d51d2588efd047"
WEB_BROWN_CONFIG_SHA256 = "ae724987139b13fabc88dfed11a4066dcf944ff74a0d8ce8b46a9183c6d9591e"
PACK_ID_RE = re.compile(r"[a-z0-9][a-z0-9-]*\Z")
HASH_RE = re.compile(r"[0-9a-f]{64}\Z")

# Linux evdev codes. Unmapped keys use the profile's alpha recording.
KEY_CATEGORIES = {
    "arrow": (103, 108, 105, 106),
    "backspace": (14, 111),  # Backspace and forward Delete.
    "caps_lock": (58,),
    "command": (125, 126),
    "control": (29, 97),
    "enter": (28, 96),  # Enter and keypad Enter.
    "escape": (1,),
    "fn": (464,),
    "function": (59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 87, 88),
    "number": (2, 3, 4, 5, 6, 7, 8, 9, 10, 11),
    "option": (56, 100),
    "shift": (42, 54),
    "space": (57,),
    "tab": (15,),
}
MODIFIER_CODES = (42, 54, 29, 97, 56, 100, 125, 126)
MOUSE_CODES = {"272": "left", "273": "right", "274": "middle"}


class ImportErrorDetail(Exception):
    pass


def safe_relative_path(value: str) -> PurePosixPath:
    path = PurePosixPath(value)
    if not value or path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
        raise ImportErrorDetail(f"unsafe source path: {value!r}")
    return path


def validate_manifest(manifest: object) -> dict:
    if not isinstance(manifest, dict) or manifest.get("format") != 1:
        raise ImportErrorDetail("unsupported native sound manifest")
    inventory = manifest.get("audio_inventory")
    profiles = manifest.get("profiles")
    generic_mouse = manifest.get("global_mouse_assets")
    mouse_profile = manifest.get("mouse_snappy_profile")
    web_brown = manifest.get("optional_website_brown_sprite")
    if not isinstance(inventory, list) or not isinstance(profiles, list):
        raise ImportErrorDetail("manifest is missing its audio inventory or profiles")
    if not isinstance(generic_mouse, list) or not isinstance(mouse_profile, dict):
        raise ImportErrorDetail("manifest is missing its mouse recordings")
    if not isinstance(web_brown, dict):
        raise ImportErrorDetail("manifest is missing the optional website Brown sprite")
    if (web_brown.get("id") != "keeby-web-keychron-k2-max-brown"
            or web_brown.get("name") != "Keychron K2 Max · K Pro Brown (website sprite)"
            or web_brown.get("source_url") != "https://getkeeby.com/sounds/sound.ogg"
            or web_brown.get("source_filename") != "sound.ogg"
            or web_brown.get("format") != "ogg"
            or web_brown.get("bytes") != WEB_BROWN_BYTES
            or web_brown.get("sha256") != WEB_BROWN_SHA256
            or web_brown.get("config_file") != WEB_BROWN_CONFIG_PATH.name
            or web_brown.get("config_sha256") != WEB_BROWN_CONFIG_SHA256
            or web_brown.get("source_sprite_keys_per_direction") != 85
            or web_brown.get("linux_keycodes_per_direction") != 85
            or web_brown.get("define_entries") != 170
            or web_brown.get("fn_code") != 464
            or web_brown.get("fn_press") != [8036, 92]
            or web_brown.get("fn_release") != [8128, 76]
            or web_brown.get("mouse_mapping") != "none"):
        raise ImportErrorDetail("optional website Brown sprite metadata does not match the pinned source")

    known = {}
    for item in inventory:
        if not isinstance(item, dict):
            raise ImportErrorDetail("invalid audio inventory record")
        rel = item.get("source_path")
        safe_relative_path(rel if isinstance(rel, str) else "")
        digest = item.get("sha256")
        size = item.get("bytes")
        if rel in known or not isinstance(digest, str) or not HASH_RE.fullmatch(digest):
            raise ImportErrorDetail(f"invalid or duplicate inventory record: {rel!r}")
        if not isinstance(size, int) or size < 1 or size > MAX_FILE_BYTES:
            raise ImportErrorDetail(f"invalid or oversized inventory record: {rel}")
        if item.get("format") not in {"wav", "mp3"}:
            raise ImportErrorDetail(f"unsupported source format: {rel}")
        known[rel] = item

    for profile in profiles:
        if not isinstance(profile, dict) or not PACK_ID_RE.fullmatch(str(profile.get("id", ""))):
            raise ImportErrorDetail("invalid profile ID in manifest")
        if not isinstance(profile.get("name"), str) or not isinstance(profile.get("keyboard_assets"), list):
            raise ImportErrorDetail(f"invalid keyboard profile: {profile.get('id')}")
        for rel in profile["keyboard_assets"]:
            if rel not in known or known[rel]["format"] != "wav":
                raise ImportErrorDetail(f"profile references an unlisted WAV: {rel}")

    for rel in generic_mouse:
        if rel not in known or known[rel]["format"] != "wav":
            raise ImportErrorDetail(f"mouse profile references an unlisted WAV: {rel}")
    cream = mouse_profile.get("keyboard_source_profile")
    if not isinstance(cream, str) or not any(p.get("source_id") == cream for p in profiles):
        raise ImportErrorDetail("Mouse Snappy references a missing keyboard profile")
    for rel in mouse_profile.get("snappy_left_assets", []):
        if rel not in known or known[rel]["format"] != "wav":
            raise ImportErrorDetail(f"Mouse Snappy references an unlisted WAV: {rel}")
    return manifest


def validate_website_brown_config(config: object, spec: dict) -> dict:
    if not isinstance(config, dict) or config.get("version") != 2:
        raise ImportErrorDetail("unsupported website Brown config")
    if (config.get("id") != spec["id"] or config.get("name") != spec["name"]
            or config.get("key_define_type") != "single"
            or config.get("sound") != spec["source_filename"]):
        raise ImportErrorDetail("website Brown config identity or sprite reference is invalid")
    defines = config.get("defines")
    if not isinstance(defines, dict) or len(defines) != spec["define_entries"]:
        raise ImportErrorDetail("website Brown config does not have 85 press/release pairs")
    codes: set[str] = set()
    for key, value in defines.items():
        if not isinstance(key, str):
            raise ImportErrorDetail("website Brown config has a non-string define key")
        code = key[:-3] if key.endswith("-up") else key
        if not code.isdecimal():
            raise ImportErrorDetail(f"website Brown config has a non-numeric key: {key!r}")
        numeric = int(code)
        if not (0 <= numeric <= 255 or numeric == spec["fn_code"]):
            raise ImportErrorDetail(f"website Brown config has an unsupported key code: {code}")
        if isinstance(value, bool) or not isinstance(value, list) or len(value) != 2:
            raise ImportErrorDetail(f"website Brown config slice is invalid: {key}")
        start_ms, duration_ms = value
        if (type(start_ms) not in (int, float) or type(duration_ms) not in (int, float)
                or not math.isfinite(start_ms) or not math.isfinite(duration_ms)
                or start_ms < 0 or duration_ms <= 0):
            raise ImportErrorDetail(f"website Brown config slice is invalid: {key}")
        codes.add(code)
    for code in codes:
        if code not in defines or f"{code}-up" not in defines:
            raise ImportErrorDetail(f"website Brown config is missing a down/up pair: {code}")
    if (len(codes) != spec["linux_keycodes_per_direction"] or "464" not in codes
            or defines["464"] != spec["fn_press"]
            or defines["464-up"] != spec["fn_release"]):
        raise ImportErrorDetail("website Brown config Fn mapping or key count is invalid")
    if {"272", "273", "274"} & codes:
        raise ImportErrorDetail("website Brown sprite must not invent mouse mappings")
    return config


def read_website_brown_config(spec: dict) -> tuple[bytes, dict]:
    if spec.get("config_file") != WEB_BROWN_CONFIG_PATH.name or WEB_BROWN_CONFIG_PATH.is_symlink():
        raise ImportErrorDetail("website Brown config path is invalid")
    try:
        config_stat = WEB_BROWN_CONFIG_PATH.stat(follow_symlinks=False)
        if not stat.S_ISREG(config_stat.st_mode) or config_stat.st_size > 1024 * 1024:
            raise ImportErrorDetail("website Brown config is not a regular bounded file")
        raw = WEB_BROWN_CONFIG_PATH.read_bytes()
    except OSError as exc:
        raise ImportErrorDetail(f"cannot read website Brown config: {exc}") from exc
    if hashlib.sha256(raw).hexdigest() != spec.get("config_sha256"):
        raise ImportErrorDetail("website Brown config hash does not match manifest")
    try:
        config = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise ImportErrorDetail(f"website Brown config is invalid JSON: {exc}") from exc
    return raw, validate_website_brown_config(config, spec)


def verified_bytes(root: Path, rel: str, item: dict) -> bytes:
    parts = safe_relative_path(rel).parts
    source = root
    for part in parts:
        source = source / part
        if source.is_symlink():
            raise ImportErrorDetail(f"source contains a symlink: {rel}")
    try:
        mode = source.stat(follow_symlinks=False).st_mode
        if not stat.S_ISREG(mode):
            raise ImportErrorDetail(f"source is not a regular file: {rel}")
        if source.stat().st_size != item["bytes"] or item["bytes"] > MAX_FILE_BYTES:
            raise ImportErrorDetail(f"source size does not match manifest: {rel}")
        data = source.read_bytes()
    except OSError as exc:
        raise ImportErrorDetail(f"cannot read source {rel}: {exc}") from exc
    if len(data) != item["bytes"] or hashlib.sha256(data).hexdigest() != item["sha256"]:
        raise ImportErrorDetail(f"source hash does not match manifest: {rel}")
    if item["format"] == "wav":
        if len(data) < 44 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
            raise ImportErrorDetail(f"source is not a RIFF/WAVE file: {rel}")
        try:
            with wave.open(str(source), "rb") as audio:
                if audio.getnframes() < 1 or audio.getnchannels() not in (1, 2):
                    raise ImportErrorDetail(f"unsupported WAV audio data: {rel}")
        except (wave.Error, EOFError) as exc:
            raise ImportErrorDetail(f"invalid WAV audio data: {rel}: {exc}") from exc
    return data


def output_name(source_path: str, prefix: str = "") -> str:
    name = PurePosixPath(source_path).name
    match = re.fullmatch(r"(.+)_([0-9]+)\.wav", name)
    if not match:
        raise ImportErrorDetail(f"source WAV name has no numbered variant: {source_path}")
    return f"{prefix}{match.group(1)}_{int(match.group(2))}.wav"


def group_filename(groups: dict, category: str, direction: str, prefix: str = "") -> str | None:
    variants = sorted(groups.get((category, direction), []))
    if not variants:
        return None
    if variants != list(range(variants[0], variants[-1] + 1)):
        raise ImportErrorDetail(f"non-contiguous {category}_{direction} variants")
    base = f"{prefix}{category}_{direction}_"
    if len(variants) == 1:
        return f"{base}{variants[0]}.wav"
    return f"{base}{{{variants[0]}-{variants[-1]}}}.wav"


def category_from_source(source_path: str) -> tuple[str, str, int]:
    name = PurePosixPath(source_path).name
    match = re.fullmatch(r"(.+)_([0-9]+)\.wav", name)
    if not match:
        raise ImportErrorDetail(f"invalid numbered source WAV name: {source_path}")
    category, variant = match.groups()
    if "_" not in category:
        raise ImportErrorDetail(f"source WAV has no direction: {source_path}")
    stem, direction = category.rsplit("_", 1)
    if direction not in {"down", "up"}:
        raise ImportErrorDetail(f"source WAV has no press/release direction: {source_path}")
    return stem, direction, int(variant)


def build_config(profile: dict, copied: dict[str, str], *, snappy: bool = False) -> dict:
    groups: dict[tuple[str, str], list[int]] = {}
    for source_path in copied:
        category, direction, variant = category_from_source(source_path)
        if source_path.startswith("Sounds/snappy/"):
            category = "snappy_" + category
        groups.setdefault((category, direction), []).append(variant)

    # The named default sounds are the exact native alpha recordings.
    press_default = group_filename(groups, "alpha", "down")
    if press_default is None:
        raise ImportErrorDetail(f"profile has no alpha press recording: {profile['id']}")
    release_default = group_filename(groups, "alpha", "up")
    defines: dict[str, str] = {}

    def add(category: str, codes: tuple[int, ...], direction: str, prefix: str = "") -> set[int]:
        filename = group_filename(groups, category, direction, prefix)
        if filename is None:
            return set()
        suffix = "-up" if direction == "up" else ""
        for code in codes:
            defines[f"{code}{suffix}"] = filename
        return set(codes)

    for direction in ("down", "up"):
        mapped_modifiers: set[int] = set()
        for category, codes in KEY_CATEGORIES.items():
            mapped_modifiers |= add(category, codes, direction)
        # A generic modifier recording covers only modifier keys without their
        # own exact category recording in the source pack.
        add("modifier", tuple(code for code in MODIFIER_CODES if code not in mapped_modifiers), direction)

        mouse_filename = group_filename(groups, "mouse", direction)
        if mouse_filename:
            for code in MOUSE_CODES:
                defines[f"{code}{'-up' if direction == 'up' else ''}"] = mouse_filename

        if snappy:
            left_filename = group_filename(groups, "snappy_left", direction)
            if left_filename:
                defines[f"272{'-up' if direction == 'up' else ''}"] = left_filename

    config = {
        "version": 2,
        "id": profile["id"],
        "name": profile["name"],
        "key_define_type": "multi",
        "defines": dict(sorted(defines.items(), key=lambda pair: (int(pair[0].removesuffix("-up")), pair[0].endswith("-up")))),
        "sound": press_default,
    }
    if release_default is not None:
        config["soundup"] = release_default
    return config


def publish_without_replace(staged_pack: Path, target: Path) -> bool:
    # Linux renameat2(RENAME_NOREPLACE) publishes a complete directory without
    # replacing even an empty destination created after the earlier existence check.
    libc = ctypes.CDLL(None, use_errno=True)
    renameat2 = getattr(libc, "renameat2", None)
    if renameat2 is None:
        raise ImportErrorDetail("libc does not provide renameat2; cannot safely publish a pack")
    renameat2.argtypes = (ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint)
    renameat2.restype = ctypes.c_int
    result = renameat2(-100, os.fsencode(staged_pack), -100, os.fsencode(target), 1)
    if result == 0:
        return True
    error = ctypes.get_errno()
    if error in (errno.EEXIST, errno.ENOTEMPTY):
        return False
    raise OSError(error, os.strerror(error), str(target))


def verified_website_brown_bytes(source_path: Path, spec: dict) -> bytes:
    source = Path(os.path.abspath(source_path.expanduser()))
    for candidate in (source, *source.parents):
        if candidate.is_symlink():
            raise ImportErrorDetail(f"website Brown source path contains a symlink: {candidate}")
    try:
        before = source.stat(follow_symlinks=False)
        if not stat.S_ISREG(before.st_mode) or before.st_size != spec["bytes"]:
            raise ImportErrorDetail("website Brown sprite is not a regular file of the pinned size")
        if not hasattr(os, "O_NOFOLLOW"):
            raise ImportErrorDetail("cannot safely open website Brown sprite without O_NOFOLLOW")
        fd = os.open(source, os.O_RDONLY | os.O_NOFOLLOW | getattr(os, "O_CLOEXEC", 0))
        with os.fdopen(fd, "rb") as stream:
            opened = os.fstat(stream.fileno())
            if not stat.S_ISREG(opened.st_mode) or opened.st_size != spec["bytes"]:
                raise ImportErrorDetail("website Brown sprite changed while it was opened")
            data = stream.read(spec["bytes"] + 1)
    except OSError as exc:
        raise ImportErrorDetail(f"cannot read website Brown sprite: {exc}") from exc
    if len(data) != spec["bytes"] or hashlib.sha256(data).hexdigest() != spec["sha256"]:
        raise ImportErrorDetail("website Brown sprite size or hash does not match manifest")
    if not data.startswith(b"OggS"):
        raise ImportErrorDetail("website Brown source does not have an OggS header")
    return data


def install_website_brown_sprite(source_path: Path, dest_root: Path, spec: dict) -> bool:
    pack_id = spec.get("id")
    if not isinstance(pack_id, str) or not PACK_ID_RE.fullmatch(pack_id):
        raise ImportErrorDetail(f"invalid website Brown pack ID: {pack_id!r}")
    target = dest_root / pack_id
    if os.path.lexists(target):
        print(f"skip {pack_id}: destination already exists")
        return False

    sprite = verified_website_brown_bytes(source_path, spec)
    config_bytes, _ = read_website_brown_config(spec)
    dest_root.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f".{pack_id}.tmp-", dir=dest_root))
    payload = stage / "pack"
    payload.mkdir()
    try:
        with (payload / spec["source_filename"]).open("xb") as output:
            output.write(sprite)
        with (payload / "config.json").open("xb") as output:
            output.write(config_bytes)
        if not publish_without_replace(payload, target):
            print(f"skip {pack_id}: destination appeared during import")
            return False
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    print(f"import {pack_id}: exact website OGG sprite, 85 press/release pairs")
    return True


def install_pack(source_root: Path, dest_root: Path, profile: dict, generic_mouse: list[str], inventory: dict,
                 *, snappy_assets: list[str] | None = None) -> bool:
    pack_id = profile.get("id")
    if not isinstance(pack_id, str) or not PACK_ID_RE.fullmatch(pack_id):
        raise ImportErrorDetail(f"invalid pack ID: {pack_id!r}")
    target = dest_root / pack_id
    if os.path.lexists(target):
        print(f"skip {pack_id}: destination already exists")
        return False

    source_paths = list(profile["keyboard_assets"])
    if snappy_assets is not None:
        source_paths += snappy_assets
    source_paths += generic_mouse
    if len(set(source_paths)) != len(source_paths):
        raise ImportErrorDetail(f"duplicate source path in pack {pack_id}")

    contents: dict[str, bytes] = {}
    total = 0
    for rel in source_paths:
        item = inventory.get(rel)
        if item is None:
            raise ImportErrorDetail(f"source is missing from manifest inventory: {rel}")
        data = verified_bytes(source_root, rel, item)
        total += len(data)
        if total > MAX_PACK_BYTES:
            raise ImportErrorDetail(f"pack exceeds {MAX_PACK_BYTES} byte limit: {pack_id}")
        contents[rel] = data

    if snappy_assets is not None:
        rewritten = {
            path: output_name(path, prefix="snappy_") if path.startswith("Sounds/snappy/") else output_name(path)
            for path in source_paths
        }
    else:
        rewritten = {path: output_name(path) for path in source_paths}
    if len(set(rewritten.values())) != len(rewritten):
        raise ImportErrorDetail(f"source files map to duplicate pack filenames: {pack_id}")

    # `mkdtemp` places staging on the destination filesystem, so the finished
    # directory appears in one rename after every file and config is complete.
    dest_root.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f".{pack_id}.tmp-", dir=dest_root))
    payload = stage / "pack"
    payload.mkdir()
    try:
        for rel, data in contents.items():
            path = payload / rewritten[rel]
            with path.open("xb") as output:
                output.write(data)

        copied = {rel: rewritten[rel] for rel in profile["keyboard_assets"]}
        if snappy_assets is not None:
            copied.update({rel: rewritten[rel] for rel in snappy_assets})
        copied.update({rel: rewritten[rel] for rel in generic_mouse})
        config = build_config(profile, copied, snappy=snappy_assets is not None)
        config_path = payload / "config.json"
        encoded = (json.dumps(config, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
        if len(encoded) > 1024 * 1024:
            raise ImportErrorDetail(f"generated config exceeds 1 MiB: {pack_id}")
        with config_path.open("xb") as output:
            output.write(encoded)
        if not publish_without_replace(payload, target):
            print(f"skip {pack_id}: destination appeared during import")
            return False
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    print(f"import {pack_id}: {len(source_paths)} source WAVs")
    return True


def make_test_wav() -> bytes:
    import struct

    pcm = struct.pack("<h", 1)
    return b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " + struct.pack(
        "<IHHIIHH", 16, 1, 1, 48000, 96000, 2, 16
    ) + b"data" + struct.pack("<I", len(pcm)) + pcm


def self_check() -> None:
    import tempfile

    wav = make_test_wav()
    with tempfile.TemporaryDirectory(prefix="keeby-import-check-") as temporary:
        root = Path(temporary)
        source = root / "Resources"
        dest = root / "packs"
        sources = {
            "Sounds/demo/alpha_down_01.wav": wav,
            "Sounds/demo/alpha_up_01.wav": wav,
            "Sounds/demo/fn_down_01.wav": wav,
            "Sounds/demo/fn_up_01.wav": wav,
            "Sounds/mouse_down_01.wav": wav,
            "Sounds/mouse_up_01.wav": wav,
        }
        records = {}
        for rel, data in sources.items():
            path = source / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            records[rel] = {"source_path": rel, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "format": "wav"}
        profile = {
            "id": "keeby-native-demo", "name": "Demo",
            "keyboard_assets": [
                "Sounds/demo/alpha_down_01.wav", "Sounds/demo/alpha_up_01.wav",
                "Sounds/demo/fn_down_01.wav", "Sounds/demo/fn_up_01.wav",
            ],
        }
        mouse = ["Sounds/mouse_down_01.wav", "Sounds/mouse_up_01.wav"]
        assert install_pack(source, dest, profile, mouse, records)
        pack = dest / profile["id"]
        config = json.loads((pack / "config.json").read_text())
        assert (pack / "alpha_down_1.wav").read_bytes() == wav
        assert config["defines"]["272"] == "mouse_down_1.wav"
        assert config["defines"]["274-up"] == "mouse_up_1.wav"
        assert config["defines"]["464"] == "fn_down_1.wav"
        assert config["defines"]["464-up"] == "fn_up_1.wav"
        marker = pack / "user-file"
        marker.write_text("keep")
        assert not install_pack(source, dest, profile, mouse, records)
        assert marker.read_text() == "keep"

        empty_dest = root / "empty-packs"
        empty_target = empty_dest / "keeby-native-demo"
        empty_target.mkdir(parents=True)
        staged = root / "staged-pack"
        staged.mkdir()
        assert not publish_without_replace(staged, empty_target)
        assert empty_target.is_dir() and not list(empty_target.iterdir())
        assert staged.is_dir()

        bad_source = root / "bad-Resources"
        bad_dest = root / "bad-packs"
        html = b"<!doctype html><title>not audio</title>"
        rel = "Sounds/demo/alpha_down_01.wav"
        bad_path = bad_source / rel
        bad_path.parent.mkdir(parents=True, exist_ok=True)
        bad_path.write_bytes(html)
        bad_records = {rel: {"source_path": rel, "bytes": len(html), "sha256": hashlib.sha256(html).hexdigest(), "format": "wav"}}
        bad_profile = {"id": "keeby-native-bad", "name": "Bad", "keyboard_assets": [rel]}
        try:
            install_pack(bad_source, bad_dest, bad_profile, [], bad_records)
        except ImportErrorDetail:
            pass
        else:
            raise AssertionError("HTML response was accepted as WAV")
        assert not (bad_dest / bad_profile["id"]).exists()
        assert not list(bad_dest.glob(".*.tmp-*"))

        stage_failure_source = root / "stage-failure-Resources"
        stage_failure_dest = root / "stage-failure-packs"
        stage_failure_assets = {
            "Sounds/demo/alpha_down_01.wav": wav,
            "Sounds/demo/alpha_down_03.wav": wav,
        }
        stage_failure_records = {}
        for rel, data in stage_failure_assets.items():
            path = stage_failure_source / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            stage_failure_records[rel] = {
                "source_path": rel, "bytes": len(data),
                "sha256": hashlib.sha256(data).hexdigest(), "format": "wav"
            }
        stage_failure_profile = {
            "id": "keeby-native-stage-failure", "name": "Stage failure",
            "keyboard_assets": list(stage_failure_assets),
        }
        try:
            install_pack(stage_failure_source, stage_failure_dest, stage_failure_profile, [], stage_failure_records)
        except ImportErrorDetail:
            pass
        else:
            raise AssertionError("non-contiguous variants were accepted")
        assert not (stage_failure_dest / stage_failure_profile["id"]).exists()
        assert not list(stage_failure_dest.glob(".*.tmp-*"))

        manifest = validate_manifest(json.loads(MANIFEST_PATH.read_text(encoding="utf-8")))
        brown_spec = manifest["optional_website_brown_sprite"]
        brown_config_bytes, brown_config = read_website_brown_config(brown_spec)
        assert len(brown_config["defines"]) == 170
        assert brown_config["defines"]["464"] == [8036, 92]
        assert brown_config["defines"]["464-up"] == [8128, 76]
        brown_codes = {key.removesuffix("-up") for key in brown_config["defines"]}
        assert len(brown_codes) == 85 and not ({"272", "273", "274"} & brown_codes)

        brown_bytes = b"OggS" + b"offline-fixture"
        brown_source = root / "website-brown.ogg"
        brown_source.write_bytes(brown_bytes)
        test_brown_spec = dict(brown_spec)
        test_brown_spec["bytes"] = len(brown_bytes)
        test_brown_spec["sha256"] = hashlib.sha256(brown_bytes).hexdigest()
        brown_dest = root / "website-brown-packs"
        assert install_website_brown_sprite(brown_source, brown_dest, test_brown_spec)
        brown_pack = brown_dest / brown_spec["id"]
        assert (brown_pack / "sound.ogg").read_bytes() == brown_bytes
        assert (brown_pack / "config.json").read_bytes() == brown_config_bytes
        brown_marker = brown_pack / "user-file"
        brown_marker.write_text("keep")
        assert not install_website_brown_sprite(root / "missing.ogg", brown_dest, test_brown_spec)
        assert brown_marker.read_text() == "keep"

        html_source = root / "website-brown.html"
        html_source.write_bytes(b"<!doctype html>")
        bad_brown_spec = dict(test_brown_spec)
        bad_brown_spec["bytes"] = html_source.stat().st_size
        bad_brown_spec["sha256"] = hashlib.sha256(html_source.read_bytes()).hexdigest()
        html_dest = root / "website-brown-html-packs"
        try:
            install_website_brown_sprite(html_source, html_dest, bad_brown_spec)
        except ImportErrorDetail:
            pass
        else:
            raise AssertionError("HTML response was accepted as OGG")
        assert not (html_dest / brown_spec["id"]).exists()
        assert not list(html_dest.glob(".*.tmp-*"))

        link = root / "website-brown-link.ogg"
        link.symlink_to(brown_source)
        try:
            install_website_brown_sprite(link, root / "website-brown-link-packs", test_brown_spec)
        except ImportErrorDetail:
            pass
        else:
            raise AssertionError("symlink OGG source was accepted")
    print("self-check: exact WAV/OGG copies, 85 Brown key pairs with Fn, mouse map exclusion, skip-existing, HTML rejection, symlink rejection, atomic cleanup: OK")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=MANIFEST_PATH)
    parser.add_argument("--native-resources", type=Path, help="extracted app Contents/Resources directory")
    parser.add_argument("--web-brown-sprite", type=Path, help="optional hash-pinned website Brown sound.ogg")
    parser.add_argument("--dest", type=Path, help="pack directory (default: $XDG_DATA_HOME/keeby/packs)")
    parser.add_argument("--self-check", action="store_true", help="run the offline importer safety check")
    args = parser.parse_args()
    if args.self_check:
        self_check()
        return 0
    if args.native_resources is None:
        parser.error("--native-resources is required unless --self-check is used")

    try:
        manifest = validate_manifest(json.loads(args.manifest.read_text(encoding="utf-8")))
        source_root = args.native_resources.resolve(strict=True)
        if not source_root.is_dir():
            raise ImportErrorDetail("--native-resources must be a directory")
        dest_root = args.dest or Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "keeby/packs"
        dest_root = dest_root.expanduser()
        inventory = {item["source_path"]: item for item in manifest["audio_inventory"]}
        imported = 0
        skipped = 0
        for profile in manifest["profiles"]:
            if install_pack(source_root, dest_root, profile, manifest["global_mouse_assets"], inventory):
                imported += 1
            else:
                skipped += 1

        mouse = manifest["mouse_snappy_profile"]
        keyboard = next(p for p in manifest["profiles"] if p["source_id"] == mouse["keyboard_source_profile"])
        mouse_pack = {
            "id": mouse["id"],
            "name": mouse["name"],
            "keyboard_assets": keyboard["keyboard_assets"],
        }
        if install_pack(source_root, dest_root, mouse_pack, mouse["generic_mouse_assets"], inventory,
                        snappy_assets=mouse["snappy_left_assets"]):
            imported += 1
        else:
            skipped += 1
        if args.web_brown_sprite is not None:
            if install_website_brown_sprite(
                    args.web_brown_sprite, dest_root, manifest["optional_website_brown_sprite"]):
                imported += 1
            else:
                skipped += 1
        print(f"done: {imported} imported, {skipped} already present; audio bytes copied without transcoding")
        return 0
    except (OSError, json.JSONDecodeError, ImportErrorDetail) as exc:
        print(f"keeby: import failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
