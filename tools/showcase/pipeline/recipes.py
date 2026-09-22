"""Pure recipes: frame counts, crop geometry and ffmpeg argument lists.

Nothing here touches the file system or runs a process, so every function is
unit-testable and every command the pipeline runs can be printed by --dry-run.
All ffmpeg invocations keep the recorder's frame count and timebase:
outputs derived from a master use ``-fps_mode passthrough`` so ffmpeg neither
drops nor duplicates a frame, and the runner verifies the count afterwards.
"""
from __future__ import annotations

import math
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path
from typing import Iterable, Sequence

# The recorder's frame-count constants (frames = round(seconds * fps)).
NTSC_FPS = 60.0988
PAL_FPS = 50.007
FPS_BY_REGION = {"ntsc": NTSC_FPS, "pal": PAL_FPS}

# Exact NES frame rates, used as the output timebase when a master carries
# none: NTSC 12 fsc / (8 samples * (341*262-0.5) dots) = 39375000/655171,
# PAL 5320342.5 / (341*312) = 322445/6448.
NTSC_RATE = Fraction(39375000, 655171)
PAL_RATE = Fraction(322445, 6448)
RATE_BY_REGION = {"ntsc": NTSC_RATE, "pal": PAL_RATE}

NES_SIZE = (256, 240)
MASTER_SIZE = (3840, 2880)
HERO_SIZE = (1440, 1080)
README_SIZE = (960, 720)
GIF_SIZE = (640, 480)
FLICKER_FRAMES = 8
FLICKER_FPS = 8
README_FPS = 30
AUDIO_RATE = 44100

MB = 1024 * 1024
LIMITS = {"readme_webp": 5 * MB, "flicker_webp": 3 * MB, "readme_gif": 8 * MB}
README_QUALITIES = (90, 85, 80, 75, 70, 65, 60, 50, 40, 30)
FLICKER_QUALITIES = ("lossless", 95, 90, 85, 80)
GIF_COLOURS = (256, 192, 128, 96, 64)

FFMPEG_BASE = ("ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-nostdin")
PASSTHROUGH = ("-fps_mode", "passthrough")
COPYABLE_VIDEO = ("h264", "hevc")


class RecipeError(ValueError):
    pass


def frame_count(seconds: float, region: str = "ntsc") -> int:
    """Frames the recorder writes for ``seconds``: floor(seconds * fps + 0.5)."""
    if seconds <= 0:
        raise RecipeError(f"seconds must be positive, got {seconds}")
    fps = FPS_BY_REGION[region.lower()]
    return int(math.floor(seconds * fps + 0.5))


def rate_for(region: str = "ntsc") -> Fraction:
    return RATE_BY_REGION[region.lower()]


def rate_string(rate: Fraction | str) -> str:
    if isinstance(rate, str):
        return rate
    return f"{rate.numerator}/{rate.denominator}"


def seconds_of(frames: int, rate: Fraction) -> float:
    return frames / float(rate)


def parse_size(text: str) -> tuple[int, int]:
    try:
        w, h = text.lower().split("x")
        w, h = int(w), int(h)
    except ValueError as e:
        raise RecipeError(f"size must be WIDTHxHEIGHT, got {text!r}") from e
    if w < 64 or h < 64:
        raise RecipeError(f"size too small: {text}")
    return w, h


def size_string(size: Sequence[int]) -> str:
    return f"{size[0]}x{size[1]}"


def even(n: int) -> int:
    return n - (n % 2)


# ---------------------------------------------------------------------------
# Geometry

@dataclass(frozen=True)
class Rect:
    x: int
    y: int
    w: int
    h: int

    def clamp(self, bounds: Sequence[int]) -> "Rect":
        bw, bh = bounds
        w, h = min(self.w, bw), min(self.h, bh)
        x = min(max(self.x, 0), bw - w)
        y = min(max(self.y, 0), bh - h)
        return Rect(x, y, w, h)

    @property
    def centre(self) -> tuple[float, float]:
        return self.x + self.w / 2, self.y + self.h / 2

    def crop_filter(self) -> str:
        return f"crop={self.w}:{self.h}:{self.x}:{self.y}"


def nes_scale(master_size: Sequence[int], override: str | None = None) -> tuple[float, float]:
    """Pixels per NES pixel on the master, horizontally and vertically.

    A 3840x2880 master maps 256x240 NES pixels at 15 x 12 (NES pixels are 8:7,
    not square). ``override`` is "15" (both axes) or "15x12"."""
    if override:
        parts = override.lower().split("x")
        try:
            values = [float(p) for p in parts]
        except ValueError as e:
            raise RecipeError(f"--flicker-scale must be S or SXxSY, got {override!r}") from e
        if len(values) == 1:
            return values[0], values[0]
        if len(values) == 2:
            return values[0], values[1]
        raise RecipeError(f"--flicker-scale must be S or SXxSY, got {override!r}")
    return master_size[0] / NES_SIZE[0], master_size[1] / NES_SIZE[1]


def validate_nes_rect(crop: Sequence[int]) -> Rect:
    if len(crop) != 4:
        raise RecipeError(f"flicker_crop must be [x, y, w, h], got {list(crop)}")
    x, y, w, h = (int(v) for v in crop)
    if w <= 0 or h <= 0:
        raise RecipeError(f"flicker_crop needs a positive size, got {list(crop)}")
    if x < 0 or y < 0 or x + w > NES_SIZE[0] or y + h > NES_SIZE[1]:
        raise RecipeError(f"flicker_crop {list(crop)} leaves the 256x240 NES frame")
    return Rect(x, y, w, h)


def flicker_geometry(crop: Sequence[int], master_size: Sequence[int] = MASTER_SIZE,
                     scale: str | None = None) -> Rect:
    """The flicker crop in master pixels, 1:1 (no resampling), even-sized."""
    r = validate_nes_rect(crop)
    sx, sy = nes_scale(master_size, scale)
    rect = Rect(int(round(r.x * sx)), int(round(r.y * sy)),
                even(int(round(r.w * sx))), even(int(round(r.h * sy))))
    return rect.clamp(master_size)


def push_in_window(crop: Sequence[int], master_size: Sequence[int] = MASTER_SIZE,
                   scale: str | None = None, aspect: Sequence[int] = (4, 3)) -> Rect:
    """A 4:3 window around the flicker crop, at 1:1 master pixels.

    The push-in ends on this window, so the last frame shows master pixels
    unscaled; the window is the smallest 4:3 rectangle that covers the crop."""
    inner = flicker_geometry(crop, master_size, scale)
    aw, ah = aspect
    w = max(inner.w, int(math.ceil(inner.h * aw / ah)))
    h = int(round(w * ah / aw))
    w, h = even(w), even(h)
    cx, cy = inner.centre
    rect = Rect(int(round(cx - w / 2)), int(round(cy - h / 2)), w, h)
    return rect.clamp(master_size)


def third_bands(width: int, count: int = 3) -> list[tuple[int, int]]:
    """(x, w) of ``count`` vertical bands covering ``width`` exactly."""
    edges = [round(i * width / count) for i in range(count + 1)]
    return [(edges[i], edges[i + 1] - edges[i]) for i in range(count)]


# ---------------------------------------------------------------------------
# Filter-graph helpers

def filter_escape(text: str) -> str:
    """Escape a value inside a filter option (paths, expressions)."""
    out = []
    for ch in text:
        if ch in "\\':[],;":
            out.append("\\" + ch)
        else:
            out.append(ch)
    return "".join(out)


def drawtext(textfile: Path | str, height: int, *, x: str | None = None,
             y: str | None = None, font: Path | str | None = None) -> str:
    """Caption from a text file (no escaping worries), sized for ``height``."""
    size = max(12, int(round(height * 0.045)))
    margin = int(round(height * 0.05))
    parts = [f"textfile={filter_escape(str(textfile))}",
             f"fontsize={size}", "fontcolor=white",
             "box=1", "boxcolor=black@0.55", f"boxborderw={max(4, size // 4)}",
             f"x={x or margin}", f"y={y or f'h-th-{margin}'}"]
    if font:
        parts.insert(0, f"fontfile={filter_escape(str(font))}")
    return "drawtext=" + ":".join(parts)


def scale_filter(size: Sequence[int]) -> str:
    return f"scale={size[0]}:{size[1]}:flags=lanczos"


def select_frame(frame: int) -> str:
    return f"select='eq(n\\,{frame})'"


# ---------------------------------------------------------------------------
# Recorder

def record_command(binary: Path | str, rom: Path | str, preset_file: Path | str,
                   state: Path | str, output: Path | str, seconds: float, *,
                   replay: Path | str | None = None, record_after: int = 2,
                   offscreen: Sequence[int] = MASTER_SIZE,
                   extra_args: Iterable[str] = ()) -> list[str]:
    """mynes_gpu --offscreen ... --record OUT.mov ROM (see README, recorder)."""
    cmd = [str(binary), "--offscreen", size_string(offscreen), "--sdr",
           "--mask-alignment", "pixels", "--preset", str(preset_file),
           "--load-state", str(state)]
    if replay:
        cmd += ["--input-replay", str(replay)]
    cmd += ["--record", str(output), "--record-seconds", _num(seconds),
            "--record-after", str(int(record_after))]
    cmd += list(extra_args)
    cmd.append(str(rom))
    return cmd


def _num(value: float) -> str:
    return str(int(value)) if float(value).is_integer() else repr(float(value))


# ---------------------------------------------------------------------------
# Per-(shot, preset) derived outputs

def _colour_tags(tags: dict | None) -> list[str]:
    """Copy the master's colour tags so a re-encode never relabels them."""
    if not tags:
        return []
    out = []
    for key, opt in (("color_space", "-colorspace"), ("color_primaries", "-color_primaries"),
                     ("color_transfer", "-color_trc"), ("color_range", "-color_range")):
        v = tags.get(key)
        if v and v != "unknown":
            out += [opt, v]
    return out


def hero_args(master: Path | str, output: Path | str, *, size: Sequence[int] = HERO_SIZE,
              crf: int = 20, preset: str = "slow", audio: bool = True,
              audio_bitrate: str = "128k", colour: dict | None = None) -> list[str]:
    """1440x1080 H.264 site clip, exact frame count, loop friendly."""
    cmd = [*FFMPEG_BASE, "-i", str(master), "-map", "0:v:0"]
    cmd += ["-map", "0:a:0"] if audio else ["-an"]
    cmd += ["-vf", scale_filter(size) + ",format=yuv420p",
            "-c:v", "libx264", "-crf", str(crf), "-preset", preset, "-pix_fmt", "yuv420p",
            *_colour_tags(colour), *PASSTHROUGH]
    if audio:
        cmd += ["-c:a", "aac", "-b:a", audio_bitrate]
    cmd += ["-movflags", "+faststart", str(output)]
    return cmd


def reddit_args(master, output, *, size=HERO_SIZE, crf: int = 18, preset: str = "slow",
                audio: bool = True, colour: dict | None = None) -> list[str]:
    return hero_args(master, output, size=size, crf=crf, preset=preset, audio=audio,
                     audio_bitrate="192k", colour=colour)


def youtube_args(master: Path | str, output: Path | str, *, video_codec: str | None,
                 audio_codec: str | None, crf: int = 14, preset: str = "slow") -> list[str]:
    """4K master rewrapped to .mp4; video copied when the codec allows."""
    cmd = [*FFMPEG_BASE, "-i", str(master), "-map", "0:v:0"]
    cmd += ["-map", "0:a:0"] if audio_codec else ["-an"]
    if video_codec in COPYABLE_VIDEO:
        cmd += ["-c:v", "copy"]
    else:
        cmd += ["-c:v", "libx264", "-crf", str(crf), "-preset", preset,
                "-pix_fmt", "yuv420p", *PASSTHROUGH]
    if audio_codec == "aac":
        cmd += ["-c:a", "copy"]
    elif audio_codec:
        cmd += ["-c:a", "aac", "-b:a", "256k"]
    cmd += ["-movflags", "+faststart", str(output)]
    return cmd


def poster_args(video: Path | str, output: Path | str, frame: int = 0,
                quality: int = 85) -> list[str]:
    """Lossy WebP poster of one frame at the clip's own size."""
    return [*FFMPEG_BASE, "-i", str(video), "-an", "-vf", select_frame(frame),
            "-frames:v", "1", "-c:v", "libwebp", "-lossless", "0",
            "-quality", str(quality), str(output)]


def still_png_args(master, output, frame: int = 0) -> list[str]:
    return [*FFMPEG_BASE, "-i", str(master), "-an", "-vf", select_frame(frame),
            "-frames:v", "1", "-c:v", "png", str(output)]


def still_webp_args(master, output, frame: int = 0, quality: int = 90) -> list[str]:
    return [*FFMPEG_BASE, "-i", str(master), "-an", "-vf", select_frame(frame),
            "-frames:v", "1", "-c:v", "libwebp", "-lossless", "0",
            "-quality", str(quality), str(output)]


def readme_frames(source_frames: int) -> int:
    """Every second frame of the source: frames 0, 2, 4, ..."""
    return (source_frames + 1) // 2


def readme_filter(source_frames: int, size=README_SIZE, fps: int = README_FPS) -> str:
    return (f"trim=end_frame={source_frames},select='not(mod(n\\,2))',"
            f"setpts=N/({fps}*TB),{scale_filter(size)}")


def readme_webp_args(master, output, source_frames: int, quality: int, *,
                     size=README_SIZE, fps: int = README_FPS) -> list[str]:
    """960x720 animated WebP at 30 fps from every second frame."""
    return [*FFMPEG_BASE, "-i", str(master), "-an",
            "-vf", readme_filter(source_frames, size, fps),
            "-frames:v", str(readme_frames(source_frames)), *PASSTHROUGH,
            "-c:v", "libwebp_anim", "-lossless", "0", "-quality", str(quality),
            "-compression_level", "6", "-loop", "0", str(output)]


def gif_palette_args(master, palette, source_frames: int, colours: int, *,
                     size=GIF_SIZE, fps: int = README_FPS) -> list[str]:
    return [*FFMPEG_BASE, "-i", str(master), "-an",
            "-vf", readme_filter(source_frames, size, fps)
            + f",palettegen=max_colors={colours}:stats_mode=diff",
            "-frames:v", "1", str(palette)]


def gif_args(master, palette, output, source_frames: int, *, size=GIF_SIZE,
             fps: int = README_FPS, dither: str = "bayer:bayer_scale=5") -> list[str]:
    graph = (f"[0:v]{readme_filter(source_frames, size, fps)}[v];"
             f"[v][1:v]paletteuse=dither={dither}:diff_mode=rectangle")
    return [*FFMPEG_BASE, "-i", str(master), "-i", str(palette), "-an",
            "-filter_complex", graph, "-frames:v", str(readme_frames(source_frames)),
            *PASSTHROUGH, "-loop", "0", str(output)]


def flicker_webp_args(master, output, rect: Rect, first_frame: int, *,
                      quality: int | str = "lossless", frames: int = FLICKER_FRAMES,
                      fps: int = FLICKER_FPS) -> list[str]:
    """Eight consecutive frames of the crop at 1:1 master pixels, 8 fps loop."""
    graph = (f"trim=start_frame={first_frame}:end_frame={first_frame + frames},"
             f"setpts=N/({fps}*TB),{rect.crop_filter()}")
    cmd = [*FFMPEG_BASE, "-i", str(master), "-an", "-vf", graph,
           "-frames:v", str(frames), *PASSTHROUGH, "-c:v", "libwebp_anim"]
    if quality == "lossless":
        cmd += ["-lossless", "1", "-pix_fmt", "bgra"]
    else:
        cmd += ["-lossless", "0", "-quality", str(quality)]
    cmd += ["-compression_level", "6", "-loop", "0", str(output)]
    return cmd


def flicker_png_args(master, output, rect: Rect, frame: int) -> list[str]:
    return [*FFMPEG_BASE, "-i", str(master), "-an",
            "-vf", f"{select_frame(frame)},{rect.crop_filter()}",
            "-frames:v", "1", "-c:v", "png", str(output)]


# ---------------------------------------------------------------------------
# Feature clips (built from masters)

def _video_out(size: Sequence[int] | None, crf: int, preset: str) -> list[str]:
    return ["-c:v", "libx264", "-crf", str(crf), "-preset", preset, "-pix_fmt", "yuv420p",
            *PASSTHROUGH]


def _audio_out(bitrate: str) -> list[str]:
    return ["-c:a", "aac", "-b:a", bitrate]


def five_televisions_args(masters: Sequence[Path | str], captions: Sequence[Path | str],
                          output: Path | str, frames_per_preset: int, rate: Fraction | str,
                          *, size: Sequence[int] | None = None, crf: int = 18,
                          preset: str = "slow", font: Path | str | None = None,
                          audio: bool = True, master_height: int = MASTER_SIZE[1]) -> list[str]:
    """One game, one television after another.

    Segment i shows frames [i*F, (i+1)*F) of master i, so the game keeps
    running while the set changes; the audio is the first master's, continuous.
    Every master must hold len(masters) * F frames."""
    n = len(masters)
    if n != len(captions):
        raise RecipeError("one caption per master")
    parts = []
    for i in range(n):
        parts.append(f"[{i}:v]trim=start_frame={i * frames_per_preset}"
                     f":end_frame={(i + 1) * frames_per_preset},setpts=PTS-STARTPTS,"
                     f"{drawtext(captions[i], master_height, font=font)}[v{i}]")
    chain = "".join(f"[v{i}]" for i in range(n)) + f"concat=n={n}:v=1:a=0,setpts=N/FRAME_RATE/TB"
    if size:
        chain += "," + scale_filter(size)
    parts.append(chain + ",format=yuv420p[v]")
    total = n * frames_per_preset
    cmd = [*FFMPEG_BASE]
    for m in masters:
        cmd += ["-i", str(m)]
    if audio:
        parts.append(f"[0:a]atrim=end={seconds_of(total, Fraction(rate)):.6f},asetpts=PTS-STARTPTS[a]")
    cmd += ["-filter_complex", ";".join(parts), "-map", "[v]"]
    cmd += ["-map", "[a]"] if audio else ["-an"]
    cmd += ["-frames:v", str(total), *_video_out(size, crf, preset)]
    if audio:
        cmd += _audio_out("192k")
    cmd += ["-movflags", "+faststart", str(output)]
    return cmd


def side_by_side_args(masters: Sequence[Path | str], captions: Sequence[Path | str],
                      output: Path | str, frames: int, rate: Fraction | str, *,
                      master_size=MASTER_SIZE, size: Sequence[int] | None = None,
                      crf: int = 18, preset: str = "slow", font: Path | str | None = None,
                      audio: bool = True) -> list[str]:
    """Three televisions side by side, each showing its third of the picture."""
    n = len(masters)
    if n != len(captions):
        raise RecipeError("one caption per master")
    bands = third_bands(master_size[0], n)
    parts = []
    for i, (x, w) in enumerate(bands):
        parts.append(f"[{i}:v]trim=end_frame={frames},setpts=PTS-STARTPTS,"
                     f"crop={w}:{master_size[1]}:{x}:0,"
                     f"{drawtext(captions[i], master_size[1], font=font, y=str(int(master_size[1] * 0.05)))}[v{i}]")
    chain = "".join(f"[v{i}]" for i in range(n)) + f"hstack=inputs={n},setpts=N/FRAME_RATE/TB"
    if size:
        chain += "," + scale_filter(size)
    parts.append(chain + ",format=yuv420p[v]")
    cmd = [*FFMPEG_BASE]
    for m in masters:
        cmd += ["-i", str(m)]
    if audio:
        parts.append(f"[0:a]atrim=end={seconds_of(frames, Fraction(rate)):.6f},asetpts=PTS-STARTPTS[a]")
    cmd += ["-filter_complex", ";".join(parts), "-map", "[v]"]
    cmd += ["-map", "[a]"] if audio else ["-an"]
    cmd += ["-frames:v", str(frames), *_video_out(size, crf, preset)]
    if audio:
        cmd += _audio_out("192k")
    cmd += ["-movflags", "+faststart", str(output)]
    return cmd


def push_in_expressions(window: Rect, master_size: Sequence[int], frames: int) -> dict[str, str]:
    """zoompan z/x/y: ease (smoothstep) from the full frame into ``window``.

    zoompan's ``on`` counts output frames from 0; with d=1 each input frame
    makes one output frame, so t = on/(frames-1) sweeps 0..1 exactly."""
    zoom = master_size[0] / window.w
    cx, cy = window.centre
    t = f"min(on/{max(frames - 1, 1)}\\,1)"
    ease = f"(({t})*({t})*(3-2*({t})))"
    return {
        "z": f"1+({zoom:.6f}-1)*{ease}",
        "x": f"(iw/2+({cx:.1f}-iw/2)*{ease})-iw/zoom/2",
        "y": f"(ih/2+({cy:.1f}-ih/2)*{ease})-ih/zoom/2",
    }


def push_in_args(master: Path | str, output: Path | str, window: Rect, frames: int,
                 rate: Fraction | str, *, master_size=MASTER_SIZE, start_frame: int = 0,
                 size: Sequence[int] | None = None, crf: int = 14, preset: str = "slow",
                 audio: bool = True) -> list[str]:
    """12 s zoompan from the full frame into the window at 1:1 master pixels.

    The native output size is the window's, so the last frame is unscaled
    master pixels; ``size`` scales that whole animation for the site/reddit."""
    e = push_in_expressions(window, master_size, frames)
    graph = (f"[0:v]trim=start_frame={start_frame}:end_frame={start_frame + frames},"
             f"setpts=PTS-STARTPTS,"
             f"zoompan=z='{e['z']}':x='{e['x']}':y='{e['y']}':d=1"
             f":s={window.w}x{window.h}:fps={rate_string(rate)}")
    if size:
        graph += "," + scale_filter(size)
    graph += ",format=yuv420p[v]"
    parts = [graph]
    cmd = [*FFMPEG_BASE, "-i", str(master)]
    if audio:
        start = seconds_of(start_frame, Fraction(rate))
        parts.append(f"[0:a]atrim=start={start:.6f}:end={start + seconds_of(frames, Fraction(rate)):.6f},"
                     f"asetpts=PTS-STARTPTS[a]")
    cmd += ["-filter_complex", ";".join(parts), "-map", "[v]"]
    cmd += ["-map", "[a]"] if audio else ["-an"]
    cmd += ["-frames:v", str(frames), *_video_out(size, crf, preset)]
    if audio:
        cmd += _audio_out("192k")
    cmd += ["-movflags", "+faststart", str(output)]
    return cmd


def feature_variant_args(source: Path | str, output: Path | str, *, size=HERO_SIZE,
                         crf: int = 20, preset: str = "slow", audio: bool = True) -> list[str]:
    """A scaled variant of a rendered feature clip (site 1440x1080)."""
    return hero_args(source, output, size=size, crf=crf, preset=preset, audio=audio)


# ---------------------------------------------------------------------------
# Size-fitting

def fit(limit: int, settings: Sequence, build) -> tuple[object, int]:
    """Try ``settings`` in order until ``build(setting)`` returns a size <= limit.

    Returns (setting, size). The last setting's output is kept when none fits,
    and the caller reports it."""
    last = None
    for setting in settings:
        size = build(setting)
        last = (setting, size)
        if size <= limit:
            return last
    if last is None:
        raise RecipeError("fit needs at least one setting")
    return last
