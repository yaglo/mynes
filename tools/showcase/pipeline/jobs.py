"""Turn the shot list into record / encode / feature / install jobs.

Output layout (``--out``, default tools/showcase/out):
  <shot>/<preset>/master.mov          the recorder's 4K master (+ record.log/json)
  <shot>/<preset>/hero.mp4            1440x1080 site clip, hero.poster.webp
  <shot>/<preset>/still.4k.png|webp   lens still (thumbnail_frame)
  <shot>/<preset>/reddit.mp4          1440x1080 crf 18 with audio
  <shot>/<preset>/youtube.mp4         the master rewrapped (or crf 14)
  <shot>/<preset>/readme.webp|gif     README animation (presets in "readme")
  <shot>/<preset>/flicker.webp|png    phase flicker crop at 1:1 master pixels
  features/<id>/{youtube,reddit,site}.mp4, poster.webp
"""
from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass, field
from fractions import Fraction
from pathlib import Path

from . import manifest as manifest_mod
from . import recipes
from . import shots as shots_mod
from .runner import (Job, PipelineError, Runner, env_for_capture, find_font, image_info,
                     probe_video, verify_image, verify_video)
from .shots import Feature, Shot, ShotList

SHOWCASE_DIR = shots_mod.SHOWCASE_DIR
ROOT = shots_mod.ROOT
DEFAULT_OUT = SHOWCASE_DIR / "out"
DEFAULT_BUILD = ROOT / "build"
RECORD_TIMEOUT = 40 * 60
ENCODE_TIMEOUT = 2 * 3600
FEATURE_VARIANTS = ("youtube", "reddit", "site")


@dataclass
class Context:
    shot_list: ShotList
    out: Path = DEFAULT_OUT
    build: Path = DEFAULT_BUILD
    binary: Path | None = None
    roms: Path | None = None
    presets_dir: Path = shots_mod.PRESETS_DIR
    states_dir: Path = shots_mod.STATES_DIR
    replays_dir: Path = shots_mod.REPLAYS_DIR
    config_dir: Path | None = None
    flicker_scale: str | None = None
    font: str | None = None
    site: Path | None = None
    rom_cache: dict[str, Path] = field(default_factory=dict)

    def __post_init__(self):
        if self.binary is None:
            self.binary = self.build / "bin" / "mynes_gpu"

    # -- layout -------------------------------------------------------------
    def pair_dir(self, shot: Shot, preset: str) -> Path:
        return self.out / shot.id / preset

    def master(self, shot: Shot, preset: str) -> Path:
        return self.pair_dir(shot, preset) / "master.mov"

    def feature_dir(self, feature: Feature) -> Path:
        return self.out / "features" / feature.id

    def preset_file(self, preset: str) -> Path:
        return shots_mod.preset_file(preset, self.presets_dir)

    def state_path(self, shot: Shot) -> Path:
        return self.states_dir / shot.state

    def replay_path(self, shot: Shot) -> Path | None:
        return self.replays_dir / shot.replay if shot.replay else None

    def rom_for(self, shot: Shot) -> Path:
        if shot.id not in self.rom_cache:
            if not self.roms:
                raise PipelineError("--roms DIR is required to find ROMs")
            self.rom_cache[shot.id] = shots_mod.resolve_rom(self.roms, shot)
        return self.rom_cache[shot.id]

    def preset_label(self, preset: str) -> str:
        meta = self.shot_list.preset_meta.get(preset, {})
        return meta.get("name") or shots_mod.preset_name(preset, self.presets_dir)


# ---------------------------------------------------------------------------
# Save-state guidance

def state_instructions(ctx: Context, shot: Shot) -> str:
    """How to create the missing state file for ``shot``, step by step."""
    rom: Path | None = None
    info = None
    if ctx.roms:
        try:
            rom = ctx.rom_for(shot)
            info = shots_mod.read_rom_info(rom)
        except (shots_mod.ShotListError, OSError):
            rom, info = None, None
    d = ctx.shot_list.defaults
    source = shots_mod.state_source_path(d.state_source, rom, info, d.state_slot,
                                         shots_mod.config_dir(ctx.config_dir))
    rom_arg = f'"{rom}"' if rom else "<path to the ROM>"
    dest = ctx.state_path(shot)
    slot = d.state_slot
    lines = [
        f"{shot.title} - {shot.scene}",
        f"  1. {ctx.binary} {rom_arg}",
        f"  2. Play to: {shot.scene}",
        f"  3. Press F5 with slot {slot} selected"
        + (" (slot 1 is selected at start" if slot == 1 else f" (F6 cycles slots until it shows {slot}")
        + "; the notice says STATE SAVED)",
        f"  4. mkdir -p \"{dest.parent}\" && cp \"{source}\" \"{dest}\"",
    ]
    if shot.replay:
        lines.append(f"  Replay: replays/{shot.replay} runs from that state; re-record it with "
                     f"--load-state \"{dest}\" --input-record replays/{shot.replay} if the scene needs other input.")
    return "\n".join(lines)


def missing_states(ctx: Context, shots: list[Shot] | None = None) -> list[Shot]:
    return [s for s in (shots or ctx.shot_list.shots) if not ctx.state_path(s).exists()]


# ---------------------------------------------------------------------------
# Master facts

@dataclass
class MasterFacts:
    path: Path
    frames: int
    rate: Fraction
    size: tuple[int, int]
    audio: bool
    codec: str
    audio_codec: str | None
    colour: dict
    assumed: bool = False


def master_facts(ctx: Context, runner: Runner, shot: Shot, preset: str) -> MasterFacts:
    """Probe the master; in a dry run without one, assume the shot's numbers."""
    path = ctx.master(shot, preset)
    if path.exists():
        info = probe_video(path)
        if info.frames != shot.frames:
            runner.say(f"note: {path} holds {info.frames} frames, shots.json expects {shot.frames}; "
                       f"outputs follow the master")
        expected = recipes.rate_for(shot.region)
        if abs(float(info.rate) - float(expected)) > 0.001:
            runner.say(f"warning: {path} is timed at {info.rate} ({float(info.rate):.4f} fps), not the "
                       f"{shot.region.upper()} rate {float(expected):.4f}; the recorder should write "
                       f"-framerate {recipes.rate_string(expected)}. Outputs keep the master's timing.")
        return MasterFacts(path, info.frames, info.rate, (info.width, info.height),
                           bool(info.audio_codec), info.codec, info.audio_codec, info.colour)
    if runner.dry_run:
        return MasterFacts(path, shot.frames, recipes.rate_for(shot.region),
                           ctx.shot_list.defaults.offscreen, True, "h264", "aac", {}, assumed=True)
    raise PipelineError(f"{path} is missing: run `showcase.py record --shots {shot.id} --presets {preset}` first")


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------------------
# Record

def record_jobs(ctx: Context, pairs: list[tuple[Shot, str]]) -> list[Job]:
    jobs = []
    for shot, preset in pairs:
        jobs.append(Job(id=f"record:{shot.id}/{preset}", deps=set(),
                        description=f"record {shot.title} on {preset}",
                        run=lambda r, s=shot, p=preset: record_one(ctx, r, s, p)))
    return jobs


def record_one(ctx: Context, runner: Runner, shot: Shot, preset: str) -> dict | None:
    state = ctx.state_path(shot)
    if not state.exists():
        raise PipelineError(f"save state {state} is missing. Create it:\n" + state_instructions(ctx, shot))
    replay = ctx.replay_path(shot)
    if replay:
        if not replay.exists():
            raise PipelineError(f"replay {replay} is missing (shots.json names it for {shot.id})")
        try:
            shots_mod.parse_replay(replay.read_text())
        except shots_mod.ShotListError as e:
            raise PipelineError(f"replay {replay}: {e}") from e
    preset_path = ctx.preset_file(preset)
    if not preset_path.exists():
        raise PipelineError(f"preset file {preset_path} is missing")
    rom = ctx.rom_for(shot)
    runner.say(f"{shot.id}: ROM {rom}")
    if not runner.dry_run and not ctx.binary.exists():
        raise PipelineError(f"{ctx.binary} is missing: build the GPU frontend with the recorder first")
    master = ctx.master(shot, preset)
    inputs = [state, preset_path] + ([replay] if replay else [])
    if runner.up_to_date([master], inputs):
        runner.say(f"up to date: {master}")
        return None
    d = ctx.shot_list.defaults
    cmd = recipes.record_command(
        ctx.binary, rom, preset_path, state, master, shot.seconds, replay=replay,
        record_after=shot.record_after, offscreen=d.offscreen, extra_args=d.record_args)
    pair_dir = ctx.pair_dir(shot, preset)
    config_home = pair_dir / "config"
    if not runner.dry_run:
        pair_dir.mkdir(parents=True, exist_ok=True)
        config_home.mkdir(exist_ok=True)
    env = env_for_capture(config_home=config_home)
    runner.run(cmd, env=env, cwd=ROOT, timeout=RECORD_TIMEOUT, log_file=pair_dir / "record.log",
               what=f"record {shot.id}/{preset}")
    if runner.dry_run:
        runner.say(f"dry-run: would verify {master}: {shot.frames} frames, "
                   f"{recipes.size_string(d.offscreen)}, audio")
        return None
    info = verify_video(master, frames=shot.frames, size=d.offscreen)
    if not info.audio_codec:
        runner.say(f"warning: {master} has no audio stream")
    record = {
        "shot": shot.id, "preset": preset, "command": cmd, "frames": info.frames,
        "rate": recipes.rate_string(info.rate), "size": [info.width, info.height],
        "video_codec": info.codec, "audio_codec": info.audio_codec,
        "rom": str(rom), "rom_sha256": _sha256(rom), "state_sha256": _sha256(state),
        "replay_sha256": _sha256(replay) if replay else None,
        "preset_sha256": _sha256(preset_path),
    }
    runner.write_json(pair_dir / "record.json", record)
    runner.say(f"recorded {master}: {info.frames} frames at {info.rate}")
    return record


# ---------------------------------------------------------------------------
# Encode

def encode_jobs(ctx: Context, pairs: list[tuple[Shot, str]]) -> list[Job]:
    jobs = []
    for shot, preset in pairs:
        key = f"{shot.id}/{preset}"
        for name, fn in (("hero", encode_hero), ("still", encode_still),
                         ("reddit", encode_reddit), ("youtube", encode_youtube)):
            jobs.append(Job(id=f"{name}:{key}", description=f"{name} {key}",
                            run=lambda r, f=fn, s=shot, p=preset: f(ctx, r, s, p)))
        if preset in shot.readme:
            jobs.append(Job(id=f"readme:{key}", description=f"README animation {key}",
                            run=lambda r, s=shot, p=preset: encode_readme(ctx, r, s, p)))
            jobs.append(Job(id=f"flicker:{key}", description=f"phase flicker {key}",
                            run=lambda r, s=shot, p=preset: encode_flicker(ctx, r, s, p)))
    return jobs


def _skip_if_fresh(runner: Runner, outputs: list[Path], inputs: list[Path]) -> bool:
    if runner.up_to_date(outputs, inputs):
        runner.say("up to date: " + ", ".join(str(o) for o in outputs))
        return True
    if not runner.dry_run:
        outputs[0].parent.mkdir(parents=True, exist_ok=True)
    return False


def encode_hero(ctx: Context, runner: Runner, shot: Shot, preset: str) -> dict | None:
    m = master_facts(ctx, runner, shot, preset)
    d = ctx.pair_dir(shot, preset)
    hero, poster = d / "hero.mp4", d / "hero.poster.webp"
    if _skip_if_fresh(runner, [hero, poster], [m.path]):
        return None
    runner.run(recipes.hero_args(m.path, hero, audio=m.audio, colour=m.colour),
               timeout=ENCODE_TIMEOUT, what=f"hero {shot.id}/{preset}")
    runner.run(recipes.poster_args(hero, poster, shot.thumbnail_frame), what="poster")
    if runner.dry_run:
        return None
    info = verify_video(hero, frames=m.frames, size=recipes.HERO_SIZE, rate=m.rate, audio=m.audio)
    verify_image(poster, frames=1, size=recipes.HERO_SIZE)
    return {"frames": info.frames, "rate": str(info.rate)}


def encode_still(ctx: Context, runner: Runner, shot: Shot, preset: str) -> dict | None:
    m = master_facts(ctx, runner, shot, preset)
    d = ctx.pair_dir(shot, preset)
    png, webp = d / "still.4k.png", d / "still.4k.webp"
    if _skip_if_fresh(runner, [png, webp], [m.path]):
        return None
    runner.run(recipes.still_png_args(m.path, png, shot.thumbnail_frame), what="still png")
    runner.run(recipes.still_webp_args(m.path, webp, shot.thumbnail_frame), what="still webp")
    if runner.dry_run:
        return None
    verify_image(png, frames=1, size=m.size)
    verify_image(webp, frames=1, size=m.size)
    return {"size": list(m.size)}


def encode_reddit(ctx: Context, runner: Runner, shot: Shot, preset: str) -> dict | None:
    m = master_facts(ctx, runner, shot, preset)
    out = ctx.pair_dir(shot, preset) / "reddit.mp4"
    if _skip_if_fresh(runner, [out], [m.path]):
        return None
    runner.run(recipes.reddit_args(m.path, out, audio=m.audio, colour=m.colour),
               timeout=ENCODE_TIMEOUT, what=f"reddit {shot.id}/{preset}")
    if runner.dry_run:
        return None
    info = verify_video(out, frames=m.frames, size=recipes.HERO_SIZE, rate=m.rate, audio=m.audio)
    return {"frames": info.frames}


def encode_youtube(ctx: Context, runner: Runner, shot: Shot, preset: str) -> dict | None:
    m = master_facts(ctx, runner, shot, preset)
    out = ctx.pair_dir(shot, preset) / "youtube.mp4"
    if _skip_if_fresh(runner, [out], [m.path]):
        return None
    copied = m.codec in recipes.COPYABLE_VIDEO
    runner.run(recipes.youtube_args(m.path, out, video_codec=m.codec, audio_codec=m.audio_codec),
               timeout=ENCODE_TIMEOUT, what=f"youtube {shot.id}/{preset} ({'copy' if copied else 'libx264'})")
    if runner.dry_run:
        return None
    info = verify_video(out, frames=m.frames, size=m.size, rate=m.rate, audio=m.audio)
    return {"frames": info.frames, "video_copied": copied}


def encode_readme(ctx: Context, runner: Runner, shot: Shot, preset: str) -> dict | None:
    """960x720 30 fps animated WebP under 5 MB and a 640x480 GIF under 8 MB."""
    m = master_facts(ctx, runner, shot, preset)
    d = ctx.pair_dir(shot, preset)
    webp, gif, palette, report = d / "readme.webp", d / "readme.gif", d / "readme.palette.png", d / "readme.json"
    if _skip_if_fresh(runner, [webp, gif, report], [m.path]):
        return None
    source_frames = min(m.frames, shot.readme_frames)
    frames = recipes.readme_frames(source_frames)
    limit_webp, limit_gif = recipes.LIMITS["readme_webp"], recipes.LIMITS["readme_gif"]

    def build_webp(quality: int) -> int:
        runner.run(recipes.readme_webp_args(m.path, webp, source_frames, quality),
                   timeout=ENCODE_TIMEOUT, what=f"readme webp q{quality}")
        return 0 if runner.dry_run else webp.stat().st_size

    def build_gif(colours: int) -> int:
        runner.run(recipes.gif_palette_args(m.path, palette, source_frames, colours),
                   timeout=ENCODE_TIMEOUT, what=f"gif palette {colours} colours")
        runner.run(recipes.gif_args(m.path, palette, gif, source_frames),
                   timeout=ENCODE_TIMEOUT, what="gif")
        return 0 if runner.dry_run else gif.stat().st_size

    q, webp_size = recipes.fit(limit_webp, recipes.README_QUALITIES, build_webp)
    colours, gif_size = recipes.fit(limit_gif, recipes.GIF_COLOURS, build_gif)
    if runner.dry_run:
        runner.say(f"dry-run: would lower WebP quality from {recipes.README_QUALITIES[0]} and GIF colours "
                   f"from {recipes.GIF_COLOURS[0]} until under {limit_webp // recipes.MB} MB / "
                   f"{limit_gif // recipes.MB} MB")
        return None
    hint = f"; lower readme_seconds for shot {shot.id!r} in shots.json (now {shot.readme_seconds:g} s)"
    try:
        verify_image(webp, frames=frames, size=recipes.README_SIZE, limit=limit_webp)
        verify_image(gif, frames=frames, size=recipes.GIF_SIZE, limit=limit_gif)
    except PipelineError as e:
        raise PipelineError(str(e) + hint) from e
    note = {"readme_webp": {"quality": q, "bytes": webp_size, "frames": frames, "fps": recipes.README_FPS,
                            "source_frames": source_frames, "size": list(recipes.README_SIZE)},
            "readme_gif": {"max_colors": colours, "bytes": gif_size, "frames": frames,
                           "size": list(recipes.GIF_SIZE)}}
    existing = json.loads(report.read_text()) if report.exists() else {}
    existing.update(note)
    runner.write_json(report, existing)
    runner.say(f"README animation: WebP quality {q} ({webp_size} bytes), GIF {colours} colours ({gif_size} bytes)")
    return note


def encode_flicker(ctx: Context, runner: Runner, shot: Shot, preset: str) -> dict | None:
    """Eight consecutive frames of the flicker crop at 1:1, lossless when it fits."""
    m = master_facts(ctx, runner, shot, preset)
    d = ctx.pair_dir(shot, preset)
    webp, png, report = d / "flicker.webp", d / "flicker.png", d / "readme.json"
    if _skip_if_fresh(runner, [webp, png], [m.path]):
        return None
    rect = recipes.flicker_geometry(shot.flicker_crop, m.size, ctx.flicker_scale)
    first = shot.flicker_first_frame
    if first + recipes.FLICKER_FRAMES > m.frames:
        raise PipelineError(f"flicker frames {first}..{first + recipes.FLICKER_FRAMES - 1} exceed the master")
    limit = recipes.LIMITS["flicker_webp"]

    def build(quality) -> int:
        runner.run(recipes.flicker_webp_args(m.path, webp, rect, first, quality=quality),
                   timeout=ENCODE_TIMEOUT, what=f"flicker webp {quality}")
        return 0 if runner.dry_run else webp.stat().st_size

    quality, size = recipes.fit(limit, recipes.FLICKER_QUALITIES, build)
    runner.run(recipes.flicker_png_args(m.path, png, rect, first), what="flicker png")
    if runner.dry_run:
        runner.say(f"dry-run: flicker crop {rect} (NES {shot.flicker_crop} at "
                   f"{recipes.nes_scale(m.size, ctx.flicker_scale)}), lossless first, then lossy until under "
                   f"{limit // recipes.MB} MB")
        return None
    verify_image(webp, frames=recipes.FLICKER_FRAMES, size=(rect.w, rect.h), limit=limit)
    verify_image(png, frames=1, size=(rect.w, rect.h))
    note = {"flicker_webp": {"quality": quality, "bytes": size, "frames": recipes.FLICKER_FRAMES,
                             "fps": recipes.FLICKER_FPS, "first_frame": first,
                             "crop_master_px": [rect.x, rect.y, rect.w, rect.h],
                             "crop_nes_px": list(shot.flicker_crop)}}
    existing = json.loads(report.read_text()) if report.exists() else {}
    existing.update(note)
    runner.write_json(report, existing)
    runner.say(f"phase flicker: {quality} ({size} bytes), crop {rect}")
    return note


# ---------------------------------------------------------------------------
# Features

def feature_jobs(ctx: Context, features: list[Feature] | None = None) -> list[Job]:
    jobs = []
    for feature in features if features is not None else ctx.shot_list.features:
        for variant in FEATURE_VARIANTS:
            jobs.append(Job(id=f"feature:{feature.id}/{variant}", description=f"{feature.type} {variant}",
                            run=lambda r, f=feature, v=variant: build_feature(ctx, r, f, v)))
        jobs.append(Job(id=f"feature:{feature.id}/poster", deps={f"feature:{feature.id}/site"},
                        run=lambda r, f=feature: feature_poster(ctx, r, f)))
    return jobs


def _variant_settings(variant: str) -> tuple[tuple[int, int] | None, int]:
    """(size or None for native, crf)."""
    return {"youtube": (None, 14), "reddit": (recipes.HERO_SIZE, 18), "site": (recipes.HERO_SIZE, 20)}[variant]


def build_feature(ctx: Context, runner: Runner, feature: Feature, variant: str) -> dict | None:
    shot = ctx.shot_list.shot(feature.shot)
    facts = [master_facts(ctx, runner, shot, p) for p in feature.presets]
    masters = [f.path for f in facts]
    m0 = facts[0]
    d = ctx.feature_dir(feature)
    out = d / f"{variant}.mp4"
    if _skip_if_fresh(runner, [out], masters):
        return None
    size, crf = _variant_settings(variant)
    frames = shots_mod.feature_frames(feature, shot)
    min_frames = min(f.frames for f in facts)
    font = find_font(ctx.font or ctx.shot_list.defaults.font)
    if feature.type in ("five-televisions", "side-by-side") and not font:
        raise PipelineError("no font for drawtext captions: pass --font FILE (see `check`)")
    labels = feature.labels or [ctx.preset_label(p) for p in feature.presets]
    captions = []
    if feature.type in ("five-televisions", "side-by-side"):
        for i, label in enumerate(labels):
            cap = d / f"caption-{i}.txt"
            runner.write_text(cap, label)
            captions.append(cap)
    audio = m0.audio
    if feature.type == "five-televisions":
        per = recipes.frame_count(feature.seconds_per_preset, shot.region)
        if len(masters) * per > min_frames:
            raise PipelineError(f"{feature.id}: needs {len(masters) * per} frames, masters hold {min_frames}")
        cmd = recipes.five_televisions_args(masters, captions, out, per, m0.rate, size=size, crf=crf,
                                            font=font, audio=audio, master_height=m0.size[1])
        expect_size = size or m0.size
    elif feature.type == "side-by-side":
        frames = min_frames
        cmd = recipes.side_by_side_args(masters, captions, out, frames, m0.rate, master_size=m0.size,
                                        size=size, crf=crf, font=font, audio=audio)
        expect_size = size or m0.size
    elif feature.type == "push-in":
        window = recipes.push_in_window(shot.flicker_crop, m0.size, ctx.flicker_scale)
        if feature.start_frame + frames > min_frames:
            raise PipelineError(f"{feature.id}: needs {feature.start_frame + frames} frames, master holds {min_frames}")
        cmd = recipes.push_in_args(masters[0], out, window, frames, m0.rate, master_size=m0.size,
                                   start_frame=feature.start_frame, size=size, crf=crf, audio=audio)
        expect_size = size or (window.w, window.h)
    else:
        raise PipelineError(f"unknown feature type {feature.type}")
    runner.run(cmd, timeout=ENCODE_TIMEOUT, what=f"feature {feature.id} {variant}")
    if runner.dry_run:
        runner.say(f"dry-run: would verify {out}: {frames} frames, {recipes.size_string(expect_size)}")
        return None
    info = verify_video(out, frames=frames, size=expect_size, rate=m0.rate, audio=audio)
    return {"frames": info.frames, "size": [info.width, info.height]}


def feature_poster(ctx: Context, runner: Runner, feature: Feature) -> None:
    d = ctx.feature_dir(feature)
    site, poster = d / "site.mp4", d / "poster.webp"
    if _skip_if_fresh(runner, [poster], [site]):
        return None
    runner.run(recipes.poster_args(site, poster, 0), what=f"feature poster {feature.id}")
    if not runner.dry_run:
        verify_image(poster, frames=1, size=recipes.HERO_SIZE)
    return None


# ---------------------------------------------------------------------------
# Install

INSTALL_FILES = {"video": "hero.mp4", "poster": "hero.poster.webp",
                 "still": "still.4k.webp", "full": "still.4k.png"}


def install(ctx: Context, runner: Runner, pairs: list[tuple[Shot, str]],
            features: list[Feature] | None = None) -> dict:
    """Copy site outputs into <site>/assets/hero and merge manifest.json."""
    if not ctx.site:
        raise PipelineError("install needs the site directory (mynes-web checkout)")
    site = ctx.site
    if not runner.dry_run and not site.is_dir():
        raise PipelineError(f"site directory {site} does not exist")
    produced: dict[str, dict[str, dict]] = {}
    for shot, preset in pairs:
        d = ctx.pair_dir(shot, preset)
        paths = manifest_mod.install_paths(shot.id, preset)
        entry = {}
        if not runner.dry_run and not any((d / name).exists() for name in INSTALL_FILES.values()):
            runner.say(f"skip {shot.id}/{preset}: nothing encoded in {d}")
            continue
        for key, name in INSTALL_FILES.items():
            src = d / name
            if not src.exists() and not runner.dry_run:
                runner.say(f"skip {shot.id}/{preset} {key}: {src} not built")
                continue
            dst = site / paths[key]
            if not runner.up_to_date([dst], [src]):
                runner.copy(src, dst)
            entry[key] = paths[key]
        if "still" in entry:
            still = d / INSTALL_FILES["still"]
            entry["still_size"] = (list(image_info(still)[1]) if still.exists()
                                   else list(ctx.shot_list.defaults.offscreen))
        if entry:
            produced.setdefault(shot.id, {})[preset] = entry
    feats: dict[str, dict] = {}
    for feature in features if features is not None else ctx.shot_list.features:
        d = ctx.feature_dir(feature)
        paths = manifest_mod.feature_paths(feature.id)
        entry = {}
        for key, name in (("video", "site.mp4"), ("poster", "poster.webp")):
            src = d / name
            if not src.exists() and not runner.dry_run:
                runner.say(f"skip feature {feature.id} {key}: {src} not built")
                continue
            dst = site / paths[key]
            if not runner.up_to_date([dst], [src]):
                runner.copy(src, dst)
            entry[key] = paths[key]
        if entry:
            entry["caption"] = feature.caption
            entry["type"] = feature.type
            feats[feature.id] = entry
    manifest_path = site / manifest_mod.HERO_PREFIX / "manifest.json"
    existing = manifest_mod.load(manifest_path) if manifest_path.exists() else None
    merged = manifest_mod.build(ctx.shot_list, produced, existing=existing, features=feats,
                                presets_dir=ctx.presets_dir)
    problems = manifest_mod.validate(merged)
    for p in problems:
        runner.say(f"manifest warning: {p}")
    runner.write_text(manifest_path, manifest_mod.dump(merged))
    runner.say(f"manifest: {manifest_path} ({len(merged.get('games', []))} games, "
               f"{sum(len(v) for v in merged.get('clips', {}).values())} clips, "
               f"{len(merged.get('features', {}))} features)")
    return merged
