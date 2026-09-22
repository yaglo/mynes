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
Stage and lens lists merge by "src"."""
from __future__ import annotations

import copy
import json
from pathlib import Path
from typing import Sequence

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
          existing: dict | None = None, presets_dir: Path | None = None, region: str = "ntsc") -> dict:
    """Merge what this run installed into ``existing`` (a v1 or v2 manifest, or None).

    ``produced`` is {shot_id: {preset: clip}} with clip keys from CLIP_KEYS."""
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
    """Problems a site would trip over; empty when the manifest is consistent.
    v1 clips (no "stage") are left to the site's v1 reader."""
    problems = []
    preset_ids = {p.get("id") for p in manifest.get("presets", [])}
    game_ids = {g.get("id") for g in manifest.get("games", [])}
    for g in manifest.get("games", []):
        if g.get("default_preset") not in preset_ids:
            problems.append(f"game {g.get('id')!r}: default_preset {g.get('default_preset')!r} not in presets")
    for game, presets in manifest.get("clips", {}).items():
        if game not in game_ids:
            problems.append(f"clips for unknown game {game!r}")
        for preset, clip in presets.items():
            where = f"clip {game}/{preset}"
            if preset not in preset_ids:
                problems.append(f"{where}: preset not listed")
            if "stage" not in clip:
                continue
            for key in ("poster", "still", "hdr"):
                if key not in clip:
                    problems.append(f"{where}: no {key!r}")
            posters = clip.get("poster")
            if isinstance(posters, list):
                for i, poster in enumerate(posters):
                    if not isinstance(poster, dict) or not all(k in poster for k in ("src", "width", "height")):
                        problems.append(f"{where}: poster[{i}] needs src, width and height")
            for key in ("stage", "lens"):
                for i, source in enumerate(clip.get(key, [])):
                    missing = [k for k in SOURCE_KEYS if k not in source]
                    if missing:
                        problems.append(f"{where}: {key}[{i}] lacks {', '.join(missing)}")
                    elif 'codecs="' not in source["type"]:
                        problems.append(f"{where}: {key}[{i}] type has no codecs")
            if not clip["stage"]:
                problems.append(f"{where}: empty stage list")
            still = clip.get("still", {})
            missing = [k for k in STILL_KEYS if k not in still]
            if missing:
                problems.append(f"{where}: still lacks {', '.join(missing)}")
            missing = [k for k in HDR_KEYS if k not in clip.get("hdr", {})]
            if missing:
                problems.append(f"{where}: hdr lacks {', '.join(missing)}")
    return problems
