"""The site's assets/hero/manifest.json, version 2.

  {"version": 2, "fps": 60.0988, "aspect": [4, 3],
   "presets": [{id, name, blurb}], "games": [{id, title, scene, default_preset}],
   "clips": {game: {preset: {
     "poster": [{src, width, height}, ...],                    (one per stage size)
     "stage": [{src, type, hdr, width, height, bytes}, ...],
     "lens":  [the same shape at 3840x2880],                   (lens shots only)
     "still": {hdr, sdr, width, height, frame},
     "hdr":   {white_nits, headroom, max_cll, max_fall}}}}}

Each clip has one directory, assets/hero/<game>/<preset>/, with a
subdirectory per render size. Each stage size has its own poster.webp, taken
from the render of that size, and "poster" lists them with their sizes: the
switcher shows a poster only at its own pixel size, so a single path (which
the site also reads) would be scaled on a display that plays the other size.
"type" is 'video/mp4; codecs="..."' for the video stream only, which is what
MediaCapabilities accepts; stage files also carry AAC-LC audio.

Merging never drops a preset, game or clip this run did not produce, keeps
top-level keys it does not know (a v1 "features" map, for example), and
replaces a produced clip's v1 keys (video, still_size, full) with v2 ones.
Stage and lens lists merge by "src". The site reads the version once for the
whole file, so a version 1 clip the run did not produce is rewritten in
version 2 form (upgrade_clip): its video becomes a one-entry SDR stage list,
its still or full image a still of frame 0, a poster path a poster list."""
from __future__ import annotations

import copy
import json
from pathlib import Path
from typing import Callable, Sequence

from . import recipes
from .shots import Shot, ShotList, preset_name

VERSION = 2
HERO_PREFIX = "assets/hero"
V1_CLIP_KEYS = ("video", "still_size", "full")
CLIP_KEYS = ("poster", "stage", "lens", "still", "hdr")
SOURCE_KEYS = ("src", "type", "hdr", "width", "height", "bytes")
STILL_KEYS = ("hdr", "sdr", "width", "height", "frame")
HDR_KEYS = ("white_nits", "headroom", "max_cll", "max_fall")


def clip_dir(shot_id: str, preset: str) -> str:
    return f"{HERO_PREFIX}/{shot_id}/{preset}"


def clip_path(shot_id: str, preset: str, size: Sequence[int], name: str) -> str:
    return f"{clip_dir(shot_id, preset)}/{recipes.size_string(size)}/{name}"


def preset_entry(preset: str, meta: dict, existing: dict | None, presets_dir: Path | None = None) -> dict:
    """name/blurb: shots.json overrides, else the existing manifest, else the preset file."""
    entry = dict(existing or {})
    entry["id"] = preset
    override = meta.get(preset, {}) if meta else {}
    name = override.get("name") or entry.get("name") or (
        preset_name(preset, presets_dir) if presets_dir else preset)
    blurb = override.get("blurb", entry.get("blurb", ""))
    entry.update(name=name, blurb=blurb)
    return entry


def game_entry(shot: Shot, existing: dict | None) -> dict:
    entry = dict(existing or {})
    entry.update(id=shot.id, title=shot.title, scene=shot.scene, default_preset=shot.default_preset)
    return entry


def _ordered_merge(existing: Sequence[dict], produced: Sequence[dict]) -> list[dict]:
    """Existing order first (entries updated in place), new entries appended."""
    by_id = {e["id"]: dict(e) for e in existing if isinstance(e, dict) and "id" in e}
    order = [e["id"] for e in existing if isinstance(e, dict) and "id" in e]
    for p in produced:
        if p["id"] in by_id:
            by_id[p["id"]].update(p)
        else:
            by_id[p["id"]] = dict(p)
            order.append(p["id"])
    return [by_id[i] for i in order]


def _merge_sources(existing, produced: list[dict]) -> list[dict]:
    """Produced sources in their order, then existing ones with another src."""
    mine = {s["src"] for s in produced}
    kept = [s for s in (existing or []) if isinstance(s, dict) and s.get("src") not in mine]
    return [dict(s) for s in produced] + kept


# describe(src, kind) gives what the site file says about itself: for kind
# "video" {type, width, height, bytes}, for "image" {width, height}; {} when
# the file is missing or unreadable.
Describe = Callable[[str, str], dict]


def is_v1_clip(clip) -> bool:
    """A clip with keys the site reads only from a version 1 manifest. A
    poster path is not one: the site reads it in either version."""
    return isinstance(clip, dict) and (any(k in clip for k in V1_CLIP_KEYS) or isinstance(clip.get("still"), str))


def v1_keys(clip: dict) -> list[str]:
    return [k for k in ("video", "full", "still_size") if k in clip] + (
        ["still"] if isinstance(clip.get("still"), str) else [])


def upgrade_clip(clip: dict, describe: Describe | None = None) -> dict:
    """A version 1 clip in version 2 form, other keys kept.

    video -> stage [{src, type, hdr: false, width, height, bytes}]; full (the
    lossless PNG) or else still, with still_size -> still {hdr: null, sdr,
    width, height, frame: 0}; a poster path -> [{src, width, height}]. Sizes,
    byte counts and the codecs string come from ``describe`` when the file
    is on the site; version 1 held none of them."""
    def about(src: str, kind: str) -> dict:
        return dict(describe(src, kind) or {}) if describe else {}

    def dims(src: str) -> dict:
        return {k: v for k, v in about(src, "image").items() if k in ("width", "height")}

    out = {k: copy.deepcopy(v) for k, v in clip.items() if k not in V1_CLIP_KEYS and k not in ("still", "poster")}
    video = clip.get("video")
    if isinstance(video, str) and video and "stage" not in clip:
        out["stage"] = [{"src": video, "type": "video/mp4", "hdr": False, **about(video, "video")}]
    full, still = clip.get("full"), clip.get("still")
    image = full if isinstance(full, str) and full else still if isinstance(still, str) and still else None
    if image:
        size = clip.get("still_size")
        known = {"width": size[0], "height": size[1]} if isinstance(size, list) and len(size) == 2 else dims(image)
        out["still"] = {"hdr": None, "sdr": image, **known, "frame": 0}
    elif isinstance(still, dict):
        out["still"] = copy.deepcopy(still)
    poster = clip.get("poster")
    if isinstance(poster, str) and poster:
        out["poster"] = [{"src": poster, **dims(poster)}]
    elif poster is not None and not isinstance(poster, str):
        out["poster"] = copy.deepcopy(poster)
    return out


def merge_clip(existing: dict | None, produced: dict) -> dict:
    clip = dict(existing or {})
    for key in V1_CLIP_KEYS:
        clip.pop(key, None)
    if not isinstance(clip.get("still"), dict):
        clip.pop("still", None)  # v1 kept a path string here
    for key in CLIP_KEYS:
        if key not in produced:
            continue
        if key in ("stage", "lens"):
            clip[key] = _merge_sources(clip.get(key), produced[key])
        else:
            clip[key] = copy.deepcopy(produced[key])
    return clip


def build(shot_list: ShotList, produced: dict[str, dict[str, dict]], *,
          existing: dict | None = None, presets_dir: Path | None = None, region: str = "ntsc",
          describe: Describe | None = None) -> dict:
    """Merge what this run installed into ``existing`` (a v1 or v2 manifest, or None).

    ``produced`` is {shot_id: {preset: clip}} with clip keys from CLIP_KEYS.
    Every other clip still in version 1 form is upgraded (upgrade_clip), so
    the version 2 file holds no version 1 clip."""
    old = copy.deepcopy(existing) if existing else {}
    meta = shot_list.preset_meta
    old_presets = {e["id"]: e for e in old.get("presets", []) if isinstance(e, dict) and "id" in e}
    presets = _ordered_merge(
        old.get("presets", []),
        [preset_entry(p, meta, old_presets.get(p), presets_dir) for p in shot_list.all_presets()])
    old_games = {e["id"]: e for e in old.get("games", []) if isinstance(e, dict) and "id" in e}
    games = _ordered_merge(
        old.get("games", []),
        [game_entry(s, old_games.get(s.id)) for s in shot_list.shots if s.id in produced])
    clips = old.get("clips", {})
    for shot_id, by_preset in produced.items():
        game = clips.setdefault(shot_id, {})
        for preset, entry in by_preset.items():
            game[preset] = merge_clip(game.get(preset), entry)
    for by_preset in clips.values():
        for preset, clip in by_preset.items():
            if is_v1_clip(clip):
                by_preset[preset] = upgrade_clip(clip, describe)
    out = {"version": VERSION, "fps": old.get("fps", recipes.FPS_BY_REGION[region]),
           "aspect": old.get("aspect", [4, 3]), "presets": presets, "games": games, "clips": clips}
    for key, value in old.items():
        if key not in out:
            out[key] = value
    return out


def load(path: Path | str) -> dict | None:
    path = Path(path)
    if not path.exists():
        return None
    data = json.loads(path.read_text())
    if not isinstance(data, dict):
        raise ValueError(f"{path}: manifest must be a JSON object")
    return data


def dump(manifest: dict) -> str:
    return json.dumps(manifest, indent=2, ensure_ascii=False) + "\n"


def validate(manifest: dict) -> list[str]:
    """Problems a site would trip over; empty when the manifest is consistent."""
    problems = []
    preset_ids = {p.get("id") for p in manifest.get("presets", [])}
    game_ids = {g.get("id") for g in manifest.get("games", [])}
    for g in manifest.get("games", []):
        if g.get("default_preset") not in preset_ids:
            problems.append(f"game {g.get('id')!r}: default_preset {g.get('default_preset')!r} not in presets")
    v2 = manifest.get("version") == VERSION
    for game, presets in manifest.get("clips", {}).items():
        if game not in game_ids:
            problems.append(f"clips for unknown game {game!r}")
        for preset, clip in presets.items():
            where = f"clip {game}/{preset}"
            if preset not in preset_ids:
                problems.append(f"{where}: preset not listed")
            if v2 and is_v1_clip(clip):
                problems.append(f"{where}: version 1 keys in a version 2 manifest, which the site ignores: "
                                + ", ".join(v1_keys(clip)))
            if "stage" not in clip:
                continue
            if "poster" not in clip:
                problems.append(f"{where}: no 'poster'")
            posters = clip.get("poster")
            if isinstance(posters, list):
                for i, poster in enumerate(posters):
                    if not isinstance(poster, dict) or not all(k in poster for k in ("src", "width", "height")):
                        problems.append(f"{where}: poster[{i}] needs src, width and height")
            hdr_sources = False
            for key in ("stage", "lens"):
                for i, source in enumerate(clip.get(key, [])):
                    missing = [k for k in SOURCE_KEYS if k not in source]
                    if missing:
                        problems.append(f"{where}: {key}[{i}] lacks {', '.join(missing)}")
                    elif 'codecs="' not in source["type"]:
                        problems.append(f"{where}: {key}[{i}] type has no codecs")
                    hdr_sources = hdr_sources or source.get("hdr") is True
            if not clip["stage"]:
                problems.append(f"{where}: empty stage list")
            if "still" in clip:
                missing = [k for k in STILL_KEYS if k not in (clip["still"] or {})]
                if missing:
                    problems.append(f"{where}: still lacks {', '.join(missing)}")
            if "hdr" in clip:
                missing = [k for k in HDR_KEYS if k not in clip["hdr"]]
                if missing:
                    problems.append(f"{where}: hdr lacks {', '.join(missing)}")
            elif hdr_sources:
                problems.append(f"{where}: HDR sources but no 'hdr'")
    return problems
