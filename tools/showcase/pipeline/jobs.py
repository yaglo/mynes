"""Turn the shot list into record / encode / feature / install jobs.

Output layout (--out, default tools/showcase/out): one directory per clip
(<shot>/<preset>/) with a subdirectory per render size:

  <WxH>/sdr.mov, sdr.json          the recorder's SDR render and its sidecar
  <WxH>/hdr.mov, hdr.json          the HDR render (BT.2020 PQ) and its sidecar
  <WxH>/{sdr,hdr}.record.log|json  recorder output; command, frames, inputs' SHA-256
  stage sizes   stage-hdr-hevc.mp4, stage-hdr-av1.mp4, stage-sdr.mp4, poster.webp
  full size     still-sdr.png, still-hdr.png (16-bit PQ), still-hdr.avif,
                crop-sdr.png, crop-sdr@1x.png, crop-hdr.png, crop-hdr@1x.png,
                crop-hdr.avif, crop-hdr@1x.avif,
                lens-hdr-hevc.mp4, lens-hdr-av1.mp4, lens-sdr-hevc.mp4   (lens clips)
                flicker.webp, flicker.png, flicker-hdr.png, flicker-hdr.jpg (README presets)
  README size   readme.webp, readme.png, readme-hdr.png, readme-hdr.jpg  (README presets)
  readme.json   what the README media fitting chose
  features/<id>/youtube.mp4
"""
from __future__ import annotations

import contextlib
import dataclasses
import hashlib
import json
import platform
import shutil
import sys
import threading
from dataclasses import dataclass, field
from fractions import Fraction
from pathlib import Path

from . import images
from . import manifest as manifest_mod
from . import recipes
from . import shots as shots_mod
from .codecs import mime_type
from .recipes import VideoOutput
from .runner import (Job, PipelineError, Runner, env_for_capture, find_font, image_info, probe_video, tool,
                     verify_animation, verify_image, verify_timing, verify_video)
from .shots import Feature, Shot, ShotList

SHOWCASE_DIR = shots_mod.SHOWCASE_DIR
ROOT = shots_mod.ROOT
DEFAULT_OUT = SHOWCASE_DIR / "out"
DEFAULT_BUILD = ROOT / "build"
GAINMAP_SCRIPT = SHOWCASE_DIR / "gainmap.swift"
RECORD_TIMEOUT = 60 * 60
ENCODE_TIMEOUT = 3 * 3600
DEFAULT_BUDGET_MB = 900

HDR_COLOUR = {"color_primaries": "bt2020", "color_transfer": "smpte2084", "color_space": "bt2020nc"}
SDR_COLOUR = {"color_primaries": "bt709", "color_transfer": "bt709", "color_space": "bt709"}
STILL_FILES = ("still-sdr.png", "still-hdr.png", "still-hdr.avif")
CROP_FILES = ("crop-sdr.png", "crop-sdr@1x.png", "crop-hdr.png", "crop-hdr@1x.png",
              "crop-hdr.avif", "crop-hdr@1x.avif")
SITE_CROP_FILES = ("crop-sdr.png", "crop-sdr@1x.png", "crop-hdr.avif", "crop-hdr@1x.avif")


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
    fast: bool = False
    with_crops: bool = False
    budget_mb: float = DEFAULT_BUDGET_MB
    rom_cache: dict[str, Path] = field(default_factory=dict)

    def __post_init__(self):
        if self.binary is None:
            self.binary = self.build / "bin" / "mynes_gpu"

    @property
    def defaults(self):
        return self.shot_list.defaults

    # -- layout -------------------------------------------------------------
    def clip_dir(self, shot: Shot, preset: str) -> Path:
        return self.out / shot.id / preset

    def size_dir(self, shot: Shot, preset: str, size) -> Path:
        return self.clip_dir(shot, preset) / recipes.size_string(size)

    def render_path(self, shot: Shot, preset: str, size, hdr: bool) -> Path:
        return self.size_dir(shot, preset, size) / ("hdr.mov" if hdr else "sdr.mov")

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

    def feature_presets(self, shot: Shot) -> set[str]:
        return {p for f in self.shot_list.features if f.shot == shot.id for p in f.presets}


# ---------------------------------------------------------------------------
# What to record

@dataclass(frozen=True)
class Render:
    """One render size of a clip, recorded twice (SDR and HDR) from the same
    state and replay, each pass for as many frames as is read from it.
    ``roles`` says what is built from it."""
    size: tuple[int, int]
    sdr_frames: int
    hdr_frames: int
    roles: tuple[str, ...]

    def frames(self, hdr: bool) -> int:
        return self.hdr_frames if hdr else self.sdr_frames


def render_plan(ctx: Context, shot: Shot, preset: str) -> list[Render]:
    """Every size a clip is recorded at, and for how many frames per pass.

    Stage sizes run the whole shot in both passes. At the full size
    (3840x2880) the SDR pass runs the whole shot when a lens clip or a
    feature needs it, and the HDR pass when a lens clip does; otherwise a
    pass stops after the last frame read from it: the still frame, and for
    README presets the eight flicker frames (SDR) or the first of them (HDR).
    At the README size the SDR pass runs readme_seconds and the HDR pass
    stops after the still frame. Emulation from a state and a replay is
    deterministic, so those are the same frames the stage renders show."""
    d = ctx.defaults
    plan: dict[tuple[int, int], list] = {}

    def add(size, role, sdr, hdr):
        entry = plan.setdefault(tuple(size), [0, 0, []])
        entry[0], entry[1] = max(entry[0], sdr), max(entry[1], hdr)
        entry[2].append(role)

    for size in d.stage_sizes:
        add(size, "stage", shot.frames, shot.frames)
    still = shot.thumbnail_frame + 1
    add(d.lens_size, "still", still, still)
    if preset in shot.lens:
        add(d.lens_size, "lens", shot.frames, shot.frames)
    if preset in shot.readme:
        first = shot.flicker_first_frame
        add(d.lens_size, "flicker", first + recipes.FLICKER_FRAMES, first + 1)
        add(d.readme_size, "readme", max(shot.readme_frames, still), still)
    if preset in ctx.feature_presets(shot):
        add(d.lens_size, "feature", shot.frames, 0)
    return [Render(size, sdr, hdr, tuple(roles)) for size, (sdr, hdr, roles) in plan.items()]


def plan_for(ctx: Context, shot: Shot, preset: str, size) -> Render:
    for r in render_plan(ctx, shot, preset):
        if r.size == tuple(size):
            return r
    raise PipelineError(f"{shot.id}/{preset} is not recorded at {recipes.size_string(size)}")


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


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------------------
# Record

def _pass(hdr: bool) -> str:
    return "hdr" if hdr else "sdr"


def sidecar_path(render: Path) -> Path:
    """The recorder writes OUT.json beside OUT.mov."""
    return render.with_suffix(".json")


def read_sidecar(render: Path) -> dict | None:
    path = sidecar_path(render)
    if not path.exists():
        return None
    try:
        data = json.loads(path.read_text())
    except ValueError as e:
        raise PipelineError(f"{path}: not JSON: {e}") from e
    if not isinstance(data, dict):
        raise PipelineError(f"{path}: not a JSON object")
    return data


# What the recorder's default masters decode as: libx264 yuv444p for SDR,
# ProRes 4444 (10-bit in the file, decoded by ffmpeg as 12-bit) for HDR.
# 4:2:0 would subsample the chroma behind the stills and crops and put a
# crop's edges on the chroma grid.
RENDER_PIX_FMTS = {False: ("yuv444p",), True: tuple(recipes.HDR_RAW_FORMATS)}


def render_format_problem(pix_fmt: str, hdr: bool) -> str | None:
    allowed = RENDER_PIX_FMTS[hdr]
    if pix_fmt in allowed:
        return None
    return (f"pixel format {pix_fmt or 'unknown'}, expected {' or '.join(allowed)}: the stills and crops need "
            f"a 4:4:4 master (unset MYNES_RECORD_CODEC_ARGS/MYNES_RECORD_HDR_CODEC_ARGS and record it again)")


def _has_levels(sidecar: dict | None) -> bool:
    return bool(sidecar) and all(isinstance(sidecar.get(k), (int, float)) for k in ("max_cll", "max_fall"))


def record_jobs(ctx: Context, pairs: list[tuple[Shot, str]]) -> list[Job]:
    jobs = []
    for shot, preset in pairs:
        for r in render_plan(ctx, shot, preset):
            for hdr in (False, True):
                if not r.frames(hdr):
                    continue
                tag = f"{recipes.size_string(r.size)}/{_pass(hdr)}"
                jobs.append(Job(id=f"record:{shot.id}/{preset}/{tag}",
                                description=f"record {shot.title} on {preset}, {tag}",
                                run=lambda runner, s=shot, p=preset, rr=r, h=hdr: record_one(ctx, runner, s, p, rr, h)))
    return jobs


def _recorded(provenance: Path, frames: int, size, cmd: list[str]) -> bool:
    """Whether the last finished recording of this pass ran ``cmd`` (so the
    same size, length, start, headroom, white and extra arguments) and gave
    ``frames`` frames."""
    try:
        data = json.loads(provenance.read_text())
    except (OSError, ValueError):
        return False
    return data.get("frames") == frames and data.get("size") == list(size) and data.get("command") == cmd


def record_one(ctx: Context, runner: Runner, shot: Shot, preset: str, r: Render, hdr: bool) -> dict | None:
    state = ctx.state_path(shot)
    if not state.exists() and not runner.dry_run:
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
    if not runner.dry_run and not ctx.binary.exists():
        raise PipelineError(f"{ctx.binary} is missing: build the GPU frontend with the recorder first")
    out = ctx.render_path(shot, preset, r.size, hdr)
    size_dir = out.parent
    name = _pass(hdr)
    provenance = size_dir / f"{name}.record.json"
    frames = r.frames(hdr)
    d = ctx.defaults
    seconds = (recipes._num(shot.seconds) if frames == shot.frames
               else recipes.seconds_for_frames(frames, shot.region))
    cmd = recipes.record_command(
        ctx.binary, rom, preset_path, state, out, seconds, replay=replay,
        record_after=shot.record_after, size=r.size, hdr=hdr, headroom=d.hdr_headroom,
        white_nits=d.hdr_white_nits, extra_args=d.record_args)
    inputs = [state, preset_path, rom] + ([replay] if replay else [])
    if runner.up_to_date([out, sidecar_path(out), provenance], inputs) and _recorded(provenance, frames, r.size, cmd):
        runner.say(f"up to date: {out}")
        return None
    config_home = ctx.clip_dir(shot, preset) / "config"
    if not runner.dry_run:
        size_dir.mkdir(parents=True, exist_ok=True)
        config_home.mkdir(exist_ok=True)
        # Only a run that finishes and passes the checks below writes these
        # again, so a failed run can never look recorded.
        provenance.unlink(missing_ok=True)
        sidecar_path(out).unlink(missing_ok=True)
    runner.run(cmd, env=env_for_capture(config_home=config_home), cwd=ROOT, timeout=RECORD_TIMEOUT,
               log_file=size_dir / f"{name}.record.log",
               what=f"record {shot.id}/{preset} {recipes.size_string(r.size)} {name.upper()}")
    if runner.dry_run:
        runner.say(f"dry-run: would verify {out}: {frames} frames, {recipes.size_string(r.size)}"
                   + (", BT.2020 PQ tags, max_cll and max_fall in the sidecar" if hdr else ""))
        return None
    info = verify_video(out, frames=frames, size=r.size, colour=HDR_COLOUR if hdr else None)
    problem = render_format_problem(info.pix_fmt, hdr)
    if problem:
        raise PipelineError(f"{out}: {problem}")
    if not info.audio_codec:
        runner.say(f"warning: {out} has no audio stream")
    sidecar = read_sidecar(out)
    problems = []
    if sidecar is None:
        if hdr:
            raise PipelineError(f"{sidecar_path(out)} is missing: the HDR encodes need its max_cll and max_fall")
        runner.say(f"warning: {sidecar_path(out)} is missing (a recorder without sidecars)")
    else:
        if sidecar.get("frames") != frames:
            problems.append(f"frames {sidecar.get('frames')}, expected {frames}")
        if [sidecar.get("width"), sidecar.get("height")] != list(r.size):
            problems.append(f"size {sidecar.get('width')}x{sidecar.get('height')}")
        if bool(sidecar.get("hdr")) != hdr:
            problems.append(f"hdr {sidecar.get('hdr')}, expected {hdr}")
        if hdr and not _has_levels(sidecar):
            problems.append("no numeric max_cll and max_fall")
    if problems:
        raise PipelineError(f"{sidecar_path(out)}: " + "; ".join(problems))
    record = {
        "shot": shot.id, "preset": preset, "pass": name, "size": list(r.size), "frames": info.frames,
        "rate": recipes.rate_string(info.rate), "roles": list(r.roles), "command": cmd,
        "video_codec": info.codec, "audio_codec": info.audio_codec, "sidecar": sidecar,
        "rom": str(rom), "rom_sha256": _sha256(rom), "state_sha256": _sha256(state),
        "replay_sha256": _sha256(replay) if replay else None, "preset_sha256": _sha256(preset_path),
    }
    runner.write_json(provenance, record)
    runner.say(f"recorded {out}: {info.frames} frames at {info.rate}")
    return record


# ---------------------------------------------------------------------------
# Render facts

@dataclass
class RenderFacts:
    path: Path
    frames: int
    rate: Fraction
    size: tuple[int, int]
    audio: bool
    hdr: bool
    matrix: str
    range: str
    sidecar: dict
    pix_fmt: str = ""
    assumed: bool = False

    @property
    def cll(self) -> tuple:
        return self.sidecar.get("max_cll"), self.sidecar.get("max_fall")


def render_facts(ctx: Context, runner: Runner, shot: Shot, preset: str, size, hdr: bool) -> RenderFacts:
    """Probe a render; in a dry run without one, assume the plan's numbers."""
    r = plan_for(ctx, shot, preset, size)
    frames = r.frames(hdr)
    path = ctx.render_path(shot, preset, r.size, hdr)
    if not frames:
        raise PipelineError(f"{shot.id}/{preset} has no {_pass(hdr).upper()} render at {recipes.size_string(r.size)}")
    if path.exists():
        info = probe_video(path)
        if info.frames != frames:
            raise PipelineError(f"{path} holds {info.frames} frames, the plan needs {frames}: record it "
                                f"again (showcase.py --shots {shot.id} --presets {preset} record)")
        if (info.width, info.height) != r.size:
            raise PipelineError(f"{path} is {info.width}x{info.height}, not {recipes.size_string(r.size)}")
        problem = render_format_problem(info.pix_fmt, hdr)
        if problem:
            raise PipelineError(f"{path}: {problem}")
        sidecar = read_sidecar(path) or {}
        if hdr and not _has_levels(sidecar):
            raise PipelineError(f"{sidecar_path(path)} lacks max_cll and max_fall")
        return RenderFacts(path, info.frames, info.rate, r.size, bool(info.audio_codec), hdr,
                           recipes.sws_matrix(info.colour, hdr), recipes.sws_range(info.colour), sidecar,
                           info.pix_fmt)
    if runner.dry_run:
        sidecar = {"white_nits": ctx.defaults.hdr_white_nits, "headroom": ctx.defaults.hdr_headroom,
                   "max_cll": "MAXCLL", "max_fall": "MAXFALL"} if hdr else {}
        return RenderFacts(path, frames, recipes.rate_for(shot.region), r.size, True, hdr,
                           recipes.HDR_DEFAULT_MATRIX if hdr else recipes.SDR_DEFAULT_MATRIX, "tv",
                           sidecar, "yuv444p12le" if hdr else "yuv444p", assumed=True)
    raise PipelineError(f"{path} is missing: run `showcase.py --shots {shot.id} --presets {preset} record` first")


# ---------------------------------------------------------------------------
# Encode

def encode_jobs(ctx: Context, pairs: list[tuple[Shot, str]]) -> list[Job]:
    jobs = []
    d = ctx.defaults
    for shot, preset in pairs:
        key = f"{shot.id}/{preset}"
        for size in d.stage_sizes:
            tag = recipes.size_string(size)
            for spec in recipes.STAGE_OUTPUTS:
                jobs.append(Job(id=f"{spec.name}:{key}/{tag}", description=f"{spec.name} {key} {tag}",
                                run=lambda r, s=shot, p=preset, z=size, v=spec: encode_video(ctx, r, s, p, z, v)))
            jobs.append(Job(id=f"poster:{key}/{tag}", description=f"poster {key} {tag}",
                            run=lambda r, s=shot, p=preset, z=size: encode_poster(ctx, r, s, p, z)))
        if preset in shot.lens:
            for spec in recipes.LENS_OUTPUTS:
                jobs.append(Job(id=f"{spec.name}:{key}", description=f"{spec.name} {key}",
                                run=lambda r, s=shot, p=preset, v=spec: encode_video(ctx, r, s, p, d.lens_size, v)))
        jobs.append(Job(id=f"still:{key}", description=f"stills and crops {key}",
                        run=lambda r, s=shot, p=preset: encode_still(ctx, r, s, p)))
        if preset in shot.readme:
            jobs.append(Job(id=f"readme:{key}", description=f"README render {key}",
                            run=lambda r, s=shot, p=preset: encode_readme(ctx, r, s, p)))
            jobs.append(Job(id=f"flicker:{key}", description=f"flicker crop {key}",
                            run=lambda r, s=shot, p=preset: encode_flicker(ctx, r, s, p)))
    return jobs


def _skip_if_fresh(runner: Runner, outputs: list[Path], inputs: list[Path]) -> bool:
    if runner.up_to_date(outputs, inputs):
        runner.say("up to date: " + ", ".join(str(o) for o in outputs))
        return True
    if not runner.dry_run:
        outputs[0].parent.mkdir(parents=True, exist_ok=True)
    return False


@contextlib.contextmanager
def _discard_on_failure(runner: Runner, outputs: list[Path]):
    """Delete ``outputs`` when the block fails, so a file that failed its
    checks is neither taken as up to date by the next run nor installed."""
    try:
        yield
    except BaseException:
        if not runner.dry_run:
            for path in outputs:
                path.unlink(missing_ok=True)
        raise


def _inputs(*facts: RenderFacts) -> list[Path]:
    paths = []
    for f in facts:
        paths += [f.path, sidecar_path(f.path)]
    return paths


EXPECT = {  # (codec, hdr) -> ffprobe codec_name, pix_fmt
    ("hevc", True): ("hevc", "yuv420p10le"), ("av1", True): ("av1", "yuv420p10le"),
    ("h264", False): ("h264", "yuv420p"), ("hevc", False): ("hevc", "yuv420p"),
}


def encode_record_path(out: Path) -> Path:
    """<name>.encode.json beside a stage or lens file: the command that made it."""
    return out.with_name(out.stem + ".encode.json")


def read_encode_record(out: Path) -> dict:
    try:
        data = json.loads(encode_record_path(out).read_text())
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


def encode_video(ctx: Context, runner: Runner, shot: Shot, preset: str, size, spec: VideoOutput) -> dict | None:
    """A stage or lens file from the render of the same size, verified, with
    its codecs string checked to be derivable. <name>.encode.json keeps the
    command, so a file made with other settings (--fast, say) is encoded
    again even when it is newer than the render."""
    facts = render_facts(ctx, runner, shot, preset, size, spec.hdr)
    out = ctx.size_dir(shot, preset, size) / spec.file
    record = encode_record_path(out)
    if spec.audio and not facts.audio:
        spec = dataclasses.replace(spec, audio=False)
    cmd = [str(c) for c in recipes.video_args(facts.path, out, spec, cll=facts.cll if spec.hdr else None,
                                              matrix=facts.matrix, range_=facts.range, fast=ctx.fast)]
    stored = read_encode_record(out)
    if stored.get("command") == cmd:
        if _skip_if_fresh(runner, [out, record], _inputs(facts)):
            return None
    elif stored and out.exists():
        why = (f"it was made {'with' if stored.get('fast') else 'without'} --fast" if stored.get("fast") != ctx.fast
               else "its settings changed")
        runner.say(f"encoding {out} again: {why}")
    if not runner.dry_run:
        out.parent.mkdir(parents=True, exist_ok=True)
        record.unlink(missing_ok=True)
    with _discard_on_failure(runner, [out, record]):
        runner.run(cmd, timeout=ENCODE_TIMEOUT, what=f"{spec.name} {shot.id}/{preset} {recipes.size_string(size)}")
        if runner.dry_run:
            return None
        codec, pix_fmt = EXPECT[(spec.codec, spec.hdr)]
        info = verify_video(out, frames=facts.frames, size=size, rate=facts.rate, audio=spec.audio,
                            codec=codec, pix_fmt=pix_fmt, colour=HDR_COLOUR if spec.hdr else SDR_COLOUR)
        codecs = info.codecs
        runner.write_json(record, {"command": cmd, "fast": ctx.fast, "encoder": _encoder(cmd), "codecs": codecs})
    return {"codecs": codecs, "bytes": out.stat().st_size}


def _encoder(cmd: list[str]) -> str:
    return cmd[cmd.index("-c:v") + 1] if "-c:v" in cmd else ""


def encode_poster(ctx: Context, runner: Runner, shot: Shot, preset: str, size) -> dict | None:
    """poster.webp: frame thumbnail_frame of the SDR render of this stage size."""
    facts = render_facts(ctx, runner, shot, preset, size, False)
    out = ctx.size_dir(shot, preset, size) / "poster.webp"
    if _skip_if_fresh(runner, [out], [facts.path]):
        return None
    with _discard_on_failure(runner, [out]):
        runner.run(recipes.poster_args(facts.path, out, shot.thumbnail_frame, matrix=facts.matrix,
                                       range_=facts.range),
                   what=f"poster {shot.id}/{preset} {recipes.size_string(size)}")
        if runner.dry_run:
            return None
        verify_image(out, frames=1, size=size)
    return {"bytes": out.stat().st_size}


def _hdr_frame(runner: Runner, facts: RenderFacts, frame: int, raw: Path, what: str, work) -> dict:
    """Extract one HDR frame as raw Y'CbCr in the render's own format, convert
    it to 16-bit PQ R'G'B' (images.yuv_to_rgb48), hand the array to ``work``,
    and return what it returns (light levels per written file)."""
    depth = recipes.HDR_RAW_FORMATS.get(facts.pix_fmt)
    if depth is None:
        raise PipelineError(f"{facts.path} is {facts.pix_fmt or 'of unknown format'}: the HDR stills need "
                            f"4:4:4 at 10, 12 or 16 bits ({', '.join(recipes.HDR_RAW_FORMATS)})")
    if facts.matrix not in images.LUMA_WEIGHTS:
        raise PipelineError(f"{facts.path}: no Y'CbCr weights for its {facts.matrix} matrix")
    runner.run(recipes.hdr_raw_args(facts.path, raw, frame, pix_fmt=facts.pix_fmt),
               what=f"HDR frame {frame} of {facts.path.parent.name}/{facts.path.name}")

    def step():
        yuv = images.read_yuv444(raw, facts.size)
        raw.unlink()
        return work(images.yuv_to_rgb48(yuv, depth, facts.matrix, full_range=facts.range == "pc"))

    return runner.step(what, step) or {}


def _write16(path: Path, rgb) -> dict:
    images.write_png16(path, rgb)
    return {path.name: images.light_levels(rgb)}


def _verify_avif(path: Path, size) -> None:
    verify_video(path, size=size, pix_fmt="yuv444p10le", colour=HDR_COLOUR)


def encode_still(ctx: Context, runner: Runner, shot: Shot, preset: str) -> dict | None:
    """Stills and 1:1 detail crops of frame thumbnail_frame at full size:
    lossless SDR PNGs, HDR AVIFs from 16-bit PQ PNGs, @1x by 2x2 average."""
    size = ctx.defaults.lens_size
    sdr = render_facts(ctx, runner, shot, preset, size, False)
    hdr = render_facts(ctx, runner, shot, preset, size, True)
    d = ctx.size_dir(shot, preset, size)
    outputs = [d / n for n in STILL_FILES + CROP_FILES]
    if _skip_if_fresh(runner, outputs, _inputs(sdr, hdr)):
        return None
    with _discard_on_failure(runner, outputs + [d / "still-hdr.yuv"]):
        frame = shot.thumbnail_frame
        rect = recipes.flicker_geometry(shot.flicker_crop, size, ctx.flicker_scale, even_size=True)
        runner.run(recipes.sdr_png_args(sdr.path, d / "still-sdr.png", frame, matrix=sdr.matrix, range_=sdr.range),
                   what=f"still-sdr {shot.id}/{preset}")
        runner.step(f"cut crop-sdr.png ({rect.crop_filter()}) and crop-sdr@1x.png (Image.reduce(2))",
                    lambda: (images.sdr_crop(d / "still-sdr.png", d / "crop-sdr.png", rect),
                             images.sdr_reduce(d / "crop-sdr.png", d / "crop-sdr@1x.png")))

        def hdr_work(rgb):
            part = images.crop(rgb, rect)
            return {**_write16(d / "still-hdr.png", rgb), **_write16(d / "crop-hdr.png", part),
                    **_write16(d / "crop-hdr@1x.png", images.box_average_2x2(part))}

        levels = _hdr_frame(runner, hdr, frame, d / "still-hdr.yuv",
                            f"write still-hdr.png, crop-hdr.png ({rect.crop_filter()}) and crop-hdr@1x.png "
                            f"(2x2 box average) as 16-bit PQ PNGs", hdr_work)
        for png in ("still-hdr.png", "crop-hdr.png", "crop-hdr@1x.png"):
            avif = png.replace(".png", ".avif")
            runner.run(recipes.avifenc_args(d / png, d / avif, clli=levels.get(png)), what=f"{avif} {shot.id}/{preset}")
        if runner.dry_run:
            return None
        half = (rect.w // 2, rect.h // 2)
        for name, want in (("still-sdr.png", size), ("still-hdr.png", size), ("crop-sdr.png", (rect.w, rect.h)),
                           ("crop-sdr@1x.png", half), ("crop-hdr.png", (rect.w, rect.h)), ("crop-hdr@1x.png", half)):
            verify_image(d / name, frames=1, size=want)
        _verify_avif(d / "still-hdr.avif", size)
        _verify_avif(d / "crop-hdr.avif", (rect.w, rect.h))
        _verify_avif(d / "crop-hdr@1x.avif", half)
    return {"crop": [rect.x, rect.y, rect.w, rect.h], "light_levels": levels}


def gainmap_available() -> tuple[bool, str]:
    """gainmap.swift needs macOS 15 (Core Image's hdrImage option) and swift."""
    if sys.platform != "darwin":
        return False, "gain-map JPEGs need macOS"
    try:
        major = int(platform.mac_ver()[0].split(".")[0])
    except ValueError:
        major = 0
    if major < 15:
        return False, f"gain-map JPEGs need macOS 15 or later (this is {platform.mac_ver()[0] or 'unknown'})"
    if not shutil.which(tool("swift")):
        return False, "swift is not installed (xcode-select --install)"
    return True, ""


GAINMAP_MARKERS = (b"urn:iso:std:iso:ts:21496:-1", b"HDRGainMap", b"hdrgm")


def _gainmap(runner: Runner, sdr_png: Path, hdr_png: Path, out: Path, size) -> str | None:
    ok, reason = gainmap_available()
    if not ok:
        runner.say(f"skip {out.name}: {reason}")
        return None
    runner.run(recipes.gainmap_args(GAINMAP_SCRIPT, sdr_png, hdr_png, out), what=f"gain map {out.name}")
    if runner.dry_run:
        return out.name
    verify_image(out, size=size)  # Pillow counts the gain map as a second MPO frame
    if not any(m in out.read_bytes() for m in GAINMAP_MARKERS):
        raise PipelineError(f"{out}: JPEG written without a gain map")
    return out.name


_REPORT_LOCK = threading.Lock()


def _update_report(runner: Runner, path: Path, note: dict) -> None:
    """Merge ``note`` into readme.json; the readme and flicker jobs may run at once."""
    with _REPORT_LOCK:
        existing = json.loads(path.read_text()) if path.exists() else {}
        existing.update(note)
        runner.write_json(path, existing)


def encode_readme(ctx: Context, runner: Runner, shot: Shot, preset: str) -> dict | None:
    """README media from the 1600x1200 render: an animated WebP (every second
    frame at 30 fps, quality lowered until under the limit), a lossless PNG of
    frame thumbnail_frame and, on macOS 15, that frame as a gain-map JPEG."""
    size = ctx.defaults.readme_size
    sdr = render_facts(ctx, runner, shot, preset, size, False)
    hdr = render_facts(ctx, runner, shot, preset, size, True)
    d = ctx.size_dir(shot, preset, size)
    webp, png, hdr_png, jpg = d / "readme.webp", d / "readme.png", d / "readme-hdr.png", d / "readme-hdr.jpg"
    if _skip_if_fresh(runner, [webp, png, hdr_png], _inputs(sdr, hdr)):
        return None
    with _discard_on_failure(runner, [webp, png, hdr_png, jpg, d / "readme-hdr.yuv"]):
        source_frames = min(sdr.frames, shot.readme_frames)
        frames = recipes.readme_frames(source_frames)
        limit = recipes.LIMITS["readme_webp"]

        def build(quality: int) -> int:
            runner.run(recipes.readme_webp_args(sdr.path, webp, source_frames, quality, matrix=sdr.matrix,
                                                range_=sdr.range),
                       timeout=ENCODE_TIMEOUT, what=f"readme webp q{quality}")
            return 0 if runner.dry_run else webp.stat().st_size

        quality, webp_size = recipes.fit(limit, recipes.README_QUALITIES, build)
        runner.run(recipes.sdr_png_args(sdr.path, png, shot.thumbnail_frame, matrix=sdr.matrix, range_=sdr.range),
                   what="readme png")
        _hdr_frame(runner, hdr, shot.thumbnail_frame, d / "readme-hdr.yuv", "write readme-hdr.png as 16-bit PQ PNG",
                   lambda rgb: _write16(hdr_png, rgb))
        gain = _gainmap(runner, png, hdr_png, jpg, size)
        if runner.dry_run:
            return None
        try:
            stored = verify_animation(webp, frames=frames, size=size, limit=limit)
        except PipelineError as e:
            raise PipelineError(f"{e}; lower readme_seconds for shot {shot.id!r} in shots.json "
                                f"(now {shot.readme_seconds:g} s)") from e
        verify_timing(webp, frames=frames, fps=recipes.README_FPS)
        verify_image(png, frames=1, size=size)
    note = {"readme_webp": {"quality": quality, "bytes": webp_size, "frames": frames, "stored_frames": stored,
                            "fps": recipes.README_FPS,
                            "source_frames": source_frames, "size": list(size), "embed_width": size[0] // 2},
            "readme_png": {"frame": shot.thumbnail_frame, "bytes": png.stat().st_size},
            "readme_gainmap": {"file": gain, "bytes": jpg.stat().st_size} if gain else None}
    _update_report(runner, ctx.clip_dir(shot, preset) / "readme.json", note)
    runner.say(f"README render: WebP quality {quality} ({webp_size} bytes)" + (f", {gain}" if gain else ""))
    return note


def encode_flicker(ctx: Context, runner: Runner, shot: Shot, preset: str) -> dict | None:
    """Eight consecutive frames of the flicker crop at 1:1 full-size pixels,
    lossless when under the limit; the first frame as PNG and gain-map JPEG."""
    size = ctx.defaults.lens_size
    sdr = render_facts(ctx, runner, shot, preset, size, False)
    hdr = render_facts(ctx, runner, shot, preset, size, True)
    d = ctx.size_dir(shot, preset, size)
    webp, png, hdr_png, jpg = d / "flicker.webp", d / "flicker.png", d / "flicker-hdr.png", d / "flicker-hdr.jpg"
    if _skip_if_fresh(runner, [webp, png, hdr_png], _inputs(sdr, hdr)):
        return None
    with _discard_on_failure(runner, [webp, png, hdr_png, jpg, d / "flicker-hdr.yuv"]):
        rect = recipes.flicker_geometry(shot.flicker_crop, size, ctx.flicker_scale)
        first = shot.flicker_first_frame
        if first + recipes.FLICKER_FRAMES > sdr.frames:
            raise PipelineError(f"flicker frames {first}..{first + recipes.FLICKER_FRAMES - 1} exceed the render")
        limit = recipes.LIMITS["flicker_webp"]

        def build(quality) -> int:
            runner.run(recipes.flicker_webp_args(sdr.path, webp, rect, first, quality=quality, matrix=sdr.matrix,
                                                 range_=sdr.range),
                       timeout=ENCODE_TIMEOUT, what=f"flicker webp {quality}")
            return 0 if runner.dry_run else webp.stat().st_size

        quality, webp_size = recipes.fit(limit, recipes.FLICKER_QUALITIES, build)
        runner.run(recipes.sdr_png_args(sdr.path, png, first, matrix=sdr.matrix, range_=sdr.range, rect=rect),
                   what="flicker png")
        _hdr_frame(runner, hdr, first, d / "flicker-hdr.yuv",
                   f"write flicker-hdr.png ({rect.crop_filter()}) as 16-bit PQ PNG",
                   lambda rgb: _write16(hdr_png, images.crop(rgb, rect)))
        gain = _gainmap(runner, png, hdr_png, jpg, (rect.w, rect.h))
        if runner.dry_run:
            runner.say(f"dry-run: flicker crop {rect} (NES {shot.flicker_crop} at "
                       f"{recipes.nes_scale(size, ctx.flicker_scale)} render pixels per NES pixel)")
            return None
        stored = verify_animation(webp, frames=recipes.FLICKER_FRAMES, size=(rect.w, rect.h), limit=limit,
                                  fps=recipes.FLICKER_FPS)
        verify_image(png, frames=1, size=(rect.w, rect.h))
    note = {"flicker_webp": {"quality": quality, "bytes": webp_size, "frames": recipes.FLICKER_FRAMES,
                             "stored_frames": stored,
                             "fps": recipes.FLICKER_FPS, "first_frame": first,
                             "crop_px": [rect.x, rect.y, rect.w, rect.h], "crop_nes_px": list(shot.flicker_crop),
                             "embed_width": rect.w // 2},
            "flicker_gainmap": {"file": gain, "bytes": jpg.stat().st_size} if gain else None}
    _update_report(runner, ctx.clip_dir(shot, preset) / "readme.json", note)
    runner.say(f"flicker crop: {quality} ({webp_size} bytes), {rect}")
    return note


# ---------------------------------------------------------------------------
# Features

def feature_jobs(ctx: Context, features: list[Feature] | None = None) -> list[Job]:
    return [Job(id=f"feature:{f.id}", description=f"{f.type} {f.id}",
                run=lambda r, f=f: build_feature(ctx, r, f))
            for f in (features if features is not None else ctx.shot_list.features)]


def build_feature(ctx: Context, runner: Runner, feature: Feature) -> dict | None:
    """youtube.mp4 at the full render size from the SDR renders, unscaled."""
    shot = ctx.shot_list.shot(feature.shot)
    size = ctx.defaults.lens_size
    facts = [render_facts(ctx, runner, shot, p, size, False) for p in feature.presets]
    m0 = facts[0]
    d = ctx.feature_dir(feature)
    out = d / "youtube.mp4"
    if _skip_if_fresh(runner, [out], [f.path for f in facts]):
        return None
    font = find_font(ctx.font or ctx.shot_list.defaults.font)
    if not font:
        raise PipelineError("no font for drawtext captions: pass --font FILE (see `check`)")
    labels = feature.labels or [ctx.preset_label(p) for p in feature.presets]
    captions = []
    for i, label in enumerate(labels):
        cap = d / f"caption-{i}.txt"
        runner.write_text(cap, label)
        captions.append(cap)
    frames = shots_mod.feature_frames(feature, shot)
    min_frames = min(f.frames for f in facts)
    if feature.type == "five-televisions":
        per = recipes.frame_count(feature.seconds_per_preset, shot.region)
        if frames > min_frames:
            raise PipelineError(f"{feature.id}: needs {frames} frames, the renders hold {min_frames}")
        cmd = recipes.five_televisions_args([f.path for f in facts], captions, out, per, m0.rate, font=font,
                                            audio=m0.audio, master_height=size[1], matrix=m0.matrix,
                                            range_=m0.range)
    elif feature.type == "side-by-side":
        frames = min_frames
        cmd = recipes.side_by_side_args([f.path for f in facts], captions, out, frames, m0.rate, master_size=size,
                                        font=font, audio=m0.audio, matrix=m0.matrix, range_=m0.range)
    else:
        raise PipelineError(f"unknown feature type {feature.type}")
    with _discard_on_failure(runner, [out]):
        runner.run(cmd, timeout=ENCODE_TIMEOUT, what=f"feature {feature.id}")
        if runner.dry_run:
            runner.say(f"dry-run: would verify {out}: {frames} frames, {recipes.size_string(size)}")
            return None
        info = verify_video(out, frames=frames, size=size, rate=m0.rate, audio=m0.audio, codec="h264",
                            pix_fmt="yuv420p", colour=SDR_COLOUR)
    return {"frames": info.frames, "size": [info.width, info.height]}


# ---------------------------------------------------------------------------
# Install

@dataclass
class ClipInstall:
    shot: Shot
    preset: str
    copies: list[tuple[Path, str]]      # (built file, path under the site)
    entry: dict


def _source_entry(path: Path, rel: str, hdr: bool) -> dict:
    info = probe_video(path)
    return {"src": rel, "type": mime_type(info.codecs), "hdr": hdr, "width": info.width,
            "height": info.height, "bytes": path.stat().st_size}


def collect_clip(ctx: Context, runner: Runner, shot: Shot, preset: str) -> ClipInstall | None:
    """What one clip installs, or None (with the reason said) when a file is missing."""
    d = ctx.defaults
    copies: list[tuple[Path, str]] = []
    missing: list[Path] = []

    def want(size, name: str) -> tuple[Path, str]:
        src = ctx.size_dir(shot, preset, size) / name
        rel = manifest_mod.clip_path(shot.id, preset, size, name)
        if not src.exists():
            missing.append(src)
        copies.append((src, rel))
        return src, rel

    stage = [(spec, *want(size, spec.file)) for size in d.stage_sizes for spec in recipes.STAGE_OUTPUTS]
    posters = [{"src": want(size, "poster.webp")[1], "width": size[0], "height": size[1]}
               for size in sorted(d.stage_sizes, reverse=True)]
    lens = [(spec, *want(d.lens_size, spec.file)) for spec in recipes.LENS_OUTPUTS] if preset in shot.lens else []
    still_hdr = want(d.lens_size, "still-hdr.avif")[1]
    still_sdr = want(d.lens_size, "still-sdr.png")[1]
    if ctx.with_crops:
        for name in SITE_CROP_FILES:
            want(d.lens_size, name)
    if missing:
        if runner.dry_run:
            return ClipInstall(shot, preset, copies, {})
        runner.say(f"skip {shot.id}/{preset}: not built: " + ", ".join(str(m) for m in missing[:3])
                   + (f" and {len(missing) - 3} more" if len(missing) > 3 else ""))
        return None
    fast = [src.name for _spec, src, _rel in stage + lens if read_encode_record(src).get("fast")]
    if fast:
        runner.say(f"warning: {shot.id}/{preset}: {', '.join(fast)} made with --fast (hevc_videotoolbox, "
                   f"no HDR10 metadata); run encode without --fast before publishing")
    sidecars = [read_sidecar(ctx.render_path(shot, preset, r.size, True)) or {}
                for r in render_plan(ctx, shot, preset)]
    entry = {
        "poster": posters,
        "stage": [_source_entry(src, rel, spec.hdr) for spec, src, rel in stage],
        "still": {"hdr": still_hdr, "sdr": still_sdr, "width": d.lens_size[0], "height": d.lens_size[1],
                  "frame": shot.thumbnail_frame},
        "hdr": {"white_nits": sidecars[0].get("white_nits", d.hdr_white_nits),
                "headroom": sidecars[0].get("headroom", d.hdr_headroom),
                "max_cll": max(int(s.get("max_cll", 0)) for s in sidecars),
                "max_fall": max(int(s.get("max_fall", 0)) for s in sidecars)},
    }
    if lens:
        entry["lens"] = [_source_entry(src, rel, spec.hdr) for spec, src, rel in lens]
    return ClipInstall(shot, preset, copies, entry)


def assets_budget(site: Path, copies: list[tuple[Path, str]]) -> tuple[int, list[tuple[int, str, str]]]:
    """Bytes under the site's assets/ after the copies, and its files largest
    first as (bytes, path under the site, "new", "replaced" or "")."""
    files: dict[str, tuple[int, str]] = {}
    assets = site / "assets"
    if assets.is_dir():
        for path in assets.rglob("*"):
            if path.is_file():
                files[path.relative_to(site).as_posix()] = (path.stat().st_size, "")
    for src, rel in copies:
        if src.exists():
            files[rel] = (src.stat().st_size, "replaced" if rel in files else "new")
    ranked = sorted(((size, rel, state) for rel, (size, state) in files.items()), reverse=True)
    return sum(size for size, _, _ in ranked), ranked


def manifest_path(site: Path) -> Path:
    return site / manifest_mod.HERO_PREFIX / "manifest.json"


def check_site(ctx: Context) -> dict | None:
    """The site's manifest (None when it has none), after checking that the
    site directory exists and the manifest reads."""
    if not ctx.site:
        raise PipelineError("install needs the site directory (mynes-web checkout)")
    if not ctx.site.is_dir():
        raise PipelineError(f"site directory {ctx.site} does not exist")
    path = manifest_path(ctx.site)
    try:
        return manifest_mod.load(path)
    except (OSError, ValueError) as e:
        raise PipelineError(f"{path}: {e}") from e


def describe_site_file(site: Path, rel: str, kind: str) -> dict:
    """What a version 1 manifest left out about a file on the site: size,
    bytes and codecs string for a video, size for an image."""
    path = site / rel
    if not path.is_file():
        return {}
    try:
        if kind == "video":
            info = probe_video(path)
            return {"type": mime_type(info.codecs), "width": info.width, "height": info.height,
                    "bytes": path.stat().st_size}
        _frames, (w, h) = image_info(path)
        return {"width": w, "height": h}
    except (PipelineError, OSError, ValueError):
        return {}


def _same_file(src: Path, dst: Path) -> bool:
    """copy2 keeps the modification time, so a site file with the build's
    size and time is the file install copied; any other is replaced."""
    if not dst.is_file():
        return False
    a, b = src.stat(), dst.stat()
    return a.st_size == b.st_size and a.st_mtime_ns == b.st_mtime_ns


def install(ctx: Context, runner: Runner, pairs: list[tuple[Shot, str]]) -> dict | None:
    """Copy each complete clip into <site>/assets/hero/<shot>/<preset>/ and merge
    manifest.json, unless the site's assets/ would exceed the budget. The
    manifest is read and merged before anything is copied, and left alone
    (with an error) when no selected clip is complete."""
    existing = check_site(ctx)
    site = ctx.site
    path = manifest_path(site)
    clips = [c for c in (collect_clip(ctx, runner, s, p) for s, p in pairs) if c]
    if not clips:
        raise PipelineError(f"nothing to install: no selected clip is complete; {path} is unchanged")
    copies = [c for clip in clips for c in clip.copies]
    total, ranked = assets_budget(site, copies)
    budget = int(ctx.budget_mb * recipes.MB)
    runner.say(f"site assets after install: {total / recipes.MB:.1f} MB of {ctx.budget_mb:g} MB"
               + (" (files not built yet are not counted)" if runner.dry_run else ""))
    if total > budget:
        lines = [f"  {size / recipes.MB:9.1f} MB  {rel}" + (f"  ({state})" if state else "")
                 for size, rel, state in ranked[:20]]
        raise PipelineError(f"refusing to install: {site}/assets would hold {total / recipes.MB:.1f} MB, more than "
                            f"--budget-mb {ctx.budget_mb:g}. Largest files:\n" + "\n".join(lines)
                            + "\nNarrow --shots/--presets, give fewer shots lens clips or raise --budget-mb.")
    produced: dict[str, dict[str, dict]] = {}
    for clip in clips:
        produced.setdefault(clip.shot.id, {})[clip.preset] = clip.entry
    try:
        merged = manifest_mod.build(ctx.shot_list, produced, existing=existing, presets_dir=ctx.presets_dir,
                                    region=ctx.shot_list.defaults.region,
                                    describe=lambda rel, kind: describe_site_file(site, rel, kind))
    except (TypeError, ValueError, KeyError, AttributeError) as e:
        raise PipelineError(f"{path}: cannot merge into this manifest: {e!r}") from e
    for src, rel in copies:
        dst = site / rel
        if runner.dry_run or runner.force or not _same_file(src, dst):
            runner.copy(src, dst)
    if runner.dry_run:
        runner.say(f"dry-run: would merge {len(clips)} clip(s) into {path}")
        return None
    for problem in manifest_mod.validate(merged):
        runner.say(f"manifest warning: {problem}")
    runner.write_text(path, manifest_mod.dump(merged))
    runner.say(f"manifest: {path} ({len(merged.get('games', []))} games, "
               f"{sum(len(v) for v in merged.get('clips', {}).values())} clips, {len(clips)} installed now)")
    return merged
