"""The site's assets/hero/manifest.json.

Schema (documented in the site's assets/hero/README.md):
  {"fps": 60.0988, "aspect": [4, 3],
   "presets": [{id, name, blurb}], "games": [{id, title, scene, default_preset}],
   "clips": {game: {preset: {video, poster, still, still_size, full}}},
   "features": {id: {video, poster, caption}}}          (added by this pipeline)
Merging never drops an entry this run did not produce."""
from __future__ import annotations

import copy
import json
from pathlib import Path
from typing import Sequence

from . import recipes
from .shots import Shot, ShotList, preset_name

HERO_PREFIX = "assets/hero"
CLIP_KEYS = ("video", "poster", "still", "still_size", "full")


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


def install_paths(shot_id: str, preset: str) -> dict[str, str]:
    base = f"{HERO_PREFIX}/{shot_id}/{preset}"
    return {"video": f"{base}.mp4", "poster": f"{base}.poster.webp",
            "still": f"{base}.4k.webp", "full": f"{base}.4k.png"}


def feature_paths(feature_id: str) -> dict[str, str]:
    base = f"{HERO_PREFIX}/features/{feature_id}"
    return {"video": f"{base}.mp4", "poster": f"{base}.poster.webp"}


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


def build(shot_list: ShotList, produced: dict[str, dict[str, dict]], *,
          existing: dict | None = None, features: dict[str, dict] | None = None,
          presets_dir: Path | None = None, region: str = "ntsc") -> dict:
    """Merge what this run installed into ``existing`` (a manifest dict or None).

    ``produced`` is {shot_id: {preset: {video, poster, still, still_size, full}}}
    with only the keys whose files were installed; ``features`` is
    {feature_id: {video, poster, caption}}."""
    out = copy.deepcopy(existing) if existing else {}
    out.setdefault("fps", recipes.FPS_BY_REGION[region])
    out.setdefault("aspect", [4, 3])
    meta = shot_list.preset_meta
    old_presets = {e["id"]: e for e in out.get("presets", []) if isinstance(e, dict) and "id" in e}
    wanted = [p for p in shot_list.all_presets()]
    out["presets"] = _ordered_merge(
        out.get("presets", []),
        [preset_entry(p, meta, old_presets.get(p), presets_dir) for p in wanted])
    old_games = {e["id"]: e for e in out.get("games", []) if isinstance(e, dict) and "id" in e}
    out["games"] = _ordered_merge(
        out.get("games", []),
        [game_entry(s, old_games.get(s.id)) for s in shot_list.shots if s.id in produced])
    clips = out.setdefault("clips", {})
    for shot_id, presets in produced.items():
        game = clips.setdefault(shot_id, {})
        for preset, entry in presets.items():
            clip = game.setdefault(preset, {})
            for key in CLIP_KEYS:
                if key in entry:
                    clip[key] = entry[key]
    if features:
        feats = out.setdefault("features", {})
        for fid, entry in features.items():
            feats.setdefault(fid, {}).update(entry)
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
    for game, presets in manifest.get("clips", {}).items():
        if game not in game_ids:
            problems.append(f"clips for unknown game {game!r}")
        for preset in presets:
            if preset not in preset_ids:
                problems.append(f"clip {game}/{preset}: preset not listed")
    return problems
