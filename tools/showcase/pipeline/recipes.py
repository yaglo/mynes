"""Pure recipes: frame counts, crop geometry and command lines.

Nothing here touches the file system or runs a process, so every function is
unit-testable and every command the pipeline runs can be printed by --dry-run.

Two rules hold for every command built here:

- No filter changes the picture size. The emulator aligns the aperture grille
  and shadow mask to output pixels, so each delivery size is recorded by the
  emulator at that size, and resampling anywhere would blur the mask. Filters
  only convert pixel format and colour (``format``, ``scale`` without a size,
  ``setparams``), select frames or cut whole-pixel crops. resampling_problem()
  enforces this for every ffmpeg command the runner starts. The one reduction
  is the exact 2x2 box average for the @1x crop variants, done on pixels in
  images.py.
- Frame count and timebase come from the render: ``-fps_mode passthrough`` and
  no ``-r``, so ffmpeg neither drops nor duplicates a frame, and the runner
  verifies the count afterwards.
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

# Exact NES frame rates: NTSC 12 fsc / (8 samples * (341*262-0.5) dots) =
# 39375000/655171, PAL 5320342.5 / (341*312) = 322445/6448. The recorder
# passes its decimal constant to ffmpeg, which stores 60.0988 as 150247/2500;
# outputs keep whatever rate the render has.
NTSC_RATE = Fraction(39375000, 655171)
PAL_RATE = Fraction(322445, 6448)
RATE_BY_REGION = {"ntsc": NTSC_RATE, "pal": PAL_RATE}

NES_SIZE = (256, 240)
LENS_SIZE = (3840, 2880)                  # lens clips, stills, detail and flicker crops
STAGE_SIZES = ((1920, 1440), (960, 720))  # the switcher's stage on 2x and 1x displays
README_SIZE = (1600, 1200)                # README media, embedded at width="800"
FLICKER_FRAMES = 8
FLICKER_FPS = 8
README_FPS = 30

# HDR renders: diffuse white at 203 nits (ITU-R BT.2408), highlights up to
# the headroom times that. HDR10 static metadata declares a P3-D65 mastering
# display with a 1000-nit peak and a 0.0001-nit minimum.
HDR_WHITE_NITS = 203
HDR_HEADROOM = 4.0
X265_MASTER_DISPLAY = "G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1)"
SVT_MASTER_DISPLAY = "G(0.265,0.690)B(0.150,0.060)R(0.680,0.320)WP(0.3127,0.3290)L(1000,0.0001)"

MB = 1024 * 1024
LIMITS = {"readme_webp": 10 * MB, "flicker_webp": 5 * MB}
README_QUALITIES = (90, 85, 80, 75, 70, 65, 60, 50, 40, 30)
FLICKER_QUALITIES = ("lossless", 95, 90, 85, 80)
POSTER_QUALITY = 85
AVIF_QUALITY = 90
AVIF_SPEED = 6
GAINMAP_QUALITY = 0.9

FFMPEG_BASE = ("ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-nostdin")
PASSTHROUGH = ("-fps_mode", "passthrough")
FASTSTART = ("-movflags", "+faststart")
# WebP frame durations are whole milliseconds. Without this, libwebp_anim
# times frames in 1/rate units of the render (1/60.0988 s), and the 125 ms
# steps of an 8 fps loop came out as 133, 116, 133, 117 ms.
WEBP_TIME_BASE = ("-enc_time_base", "1:1000")
STAGE_AUDIO = ("-c:a", "aac", "-b:a", "128k")
FEATURE_AUDIO = ("-c:a", "aac", "-b:a", "192k")

HDR_TAGS = ("-color_primaries", "bt2020", "-color_trc", "smpte2084", "-colorspace", "bt2020nc",
            "-color_range", "tv")
SDR_TAGS = ("-color_primaries", "bt709", "-color_trc", "bt709", "-colorspace", "bt709",
            "-color_range", "tv")
HDR_PARAMS = "setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc:range=tv"
SDR_PARAMS = "setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=tv"

# swscale matrix names for the colour-space tags a render may carry. The
# recorder's SDR path converts rgb24 to yuv444p with swscale's default,
# BT.601, and leaves the file untagged, so an untagged SDR render is BT.601.
SWS_MATRIX = {"bt709": "bt709", "smpte170m": "bt601", "bt470bg": "bt601", "bt2020nc": "bt2020",
              "bt2020c": "bt2020", "fcc": "fcc", "smpte240m": "smpte240m"}
SDR_DEFAULT_MATRIX = "bt601"
HDR_DEFAULT_MATRIX = "bt2020"
# Planar 4:4:4 formats an HDR frame is read in, with their bit depth.
HDR_RAW_FORMATS = {"yuv444p10le": 10, "yuv444p12le": 12, "yuv444p16le": 16}


class RecipeError(ValueError):
    pass


@dataclass(frozen=True)
class VideoOutput:
    """One encoded file of a clip: file stem, codec, HDR or SDR source, CRF
    (x264, x265 or SVT-AV1 scale), hevc_videotoolbox -q:v for --fast, audio."""
    name: str
    codec: str
    hdr: bool
    crf: int
    fast_quality: int
    audio: bool

    @property
    def file(self) -> str:
        return f"{self.name}.mp4"


STAGE_OUTPUTS = (
    VideoOutput("stage-hdr-hevc", "hevc", True, 18, 70, True),
    VideoOutput("stage-hdr-av1", "av1", True, 24, 0, True),
    VideoOutput("stage-sdr", "h264", False, 18, 0, True),
)
LENS_OUTPUTS = (
    VideoOutput("lens-hdr-hevc", "hevc", True, 14, 80, False),
    VideoOutput("lens-hdr-av1", "av1", True, 20, 0, False),
    VideoOutput("lens-sdr-hevc", "hevc", False, 14, 80, False),
)
X265_PRESET, X264_PRESET, SVT_PRESET = "slow", "slow", 6
FAST_X264_PRESET, FAST_SVT_PRESET = "veryfast", 10


def frame_count(seconds: float, region: str = "ntsc") -> int:
    """Frames the recorder writes for ``seconds``: floor(seconds * fps + 0.5)."""
    if seconds <= 0:
        raise RecipeError(f"seconds must be positive, got {seconds}")
    fps = FPS_BY_REGION[region.lower()]
    return int(math.floor(seconds * fps + 0.5))


def seconds_for_frames(frames: int, region: str = "ntsc") -> str:
    """A --record-seconds value that makes the recorder write exactly ``frames``."""
    if frames <= 0:
        raise RecipeError(f"frames must be positive, got {frames}")
    text = f"{frames / FPS_BY_REGION[region.lower()]:.9f}"
    if frame_count(float(text), region) != frames:  # pragma: no cover - nine decimals suffice
        raise RecipeError(f"no --record-seconds value gives {frames} frames")
    return text


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
        w, h = str(text).lower().split("x")
        w, h = int(w), int(h)
    except ValueError as e:
        raise RecipeError(f"size must be WIDTHxHEIGHT, got {text!r}") from e
    if w < 64 or h < 64:
        raise RecipeError(f"size too small: {text}")
    if w % 2 or h % 2:
        raise RecipeError(f"size must be even for 4:2:0 video: {text}")
    return w, h


def size_string(size: Sequence[int]) -> str:
    return f"{size[0]}x{size[1]}"


def _round(v: float) -> int:
    return int(math.floor(v + 0.5))


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
    def box(self) -> tuple[int, int, int, int]:
        """Pillow's (left, top, right, bottom)."""
        return self.x, self.y, self.x + self.w, self.y + self.h

    def crop_filter(self) -> str:
        return f"crop={self.w}:{self.h}:{self.x}:{self.y}"


# Where the 256x240 picture sits on the receiver's 4:3 raster, as the
# frontend places it (frontends/gpu/decode_window.h, decode_window_geometry): the
# receiver scans the standard active line and field (BT.470 NTSC: a 63.556 us
# line with 10.9 us of blanking after a 1.5 us front porch, 21 blanked lines
# of the console's 262) locked to the console's sync, and the console's
# picture starts 65 dots after its sync with the vertical sync 17 lines
# before the frame's end. A NES dot is 8 samples of 12 per subcarrier cycle.
_DOT_US = 1e6 * 8 / (12 * 3579545)
ACTIVE_DOTS = (1e6 / 15734.264 - 10.9) / _DOT_US          # 282.73
PICTURE_LEFT = 65 - (10.9 - 1.5) / _DOT_US                # 14.53
ACTIVE_LINES = 262 - 21.0                                 # 241
PICTURE_TOP = (262 - 245) - 18.0                          # -1: the top line is in blanking


def nes_scale(size: Sequence[int], override: str | None = None) -> tuple[float, float]:
    """Render pixels per NES pixel, horizontally and vertically: the render's
    width over the active line's dots and its height over the active field's
    lines (NES pixels are 8:7, not square). ``override`` is "15" (both axes)
    or "15x12"."""
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
    return size[0] / ACTIVE_DOTS, size[1] / ACTIVE_LINES


def nes_origin(size: Sequence[int], scale: tuple[float, float], override: str | None = None) -> tuple[float, float]:
    """Render pixel of NES pixel (0, 0): the picture's offset on the raster.
    An explicit scale describes a render whose picture fills the frame, so
    its origin is the frame's corner."""
    if override:
        return 0.0, 0.0
    return PICTURE_LEFT * scale[0], PICTURE_TOP * scale[1]


def validate_nes_rect(crop: Sequence[float]) -> tuple[float, float, float, float]:
    """[x, y, w, h] in NES pixels; fractions are allowed (93.75 lines x 12 = 1125 rows)."""
    if len(crop) != 4:
        raise RecipeError(f"flicker_crop must be [x, y, w, h], got {list(crop)}")
    try:
        x, y, w, h = (float(v) for v in crop)
    except (TypeError, ValueError) as e:
        raise RecipeError(f"flicker_crop must be numbers, got {list(crop)}") from e
    if w <= 0 or h <= 0:
        raise RecipeError(f"flicker_crop needs a positive size, got {list(crop)}")
    if x < 0 or y < 0 or x + w > NES_SIZE[0] or y + h > NES_SIZE[1]:
        raise RecipeError(f"flicker_crop {list(crop)} leaves the 256x240 NES frame")
    return x, y, w, h


def flicker_geometry(crop: Sequence[float], size: Sequence[int] = LENS_SIZE,
                     scale: str | None = None, *, align: int = 1) -> Rect:
    """The crop in render pixels, 1:1. ``align`` rounds the size down to a
    multiple: 2 keeps the 2x2 average of the @1x variant covering every
    pixel, and 6 also puts the crop on whole device pixels at pixel ratios
    1.5 and 3, where browsers lay out in steps of 1/64 CSS px."""
    x, y, w, h = validate_nes_rect(crop)
    sx, sy = nes_scale(size, scale)
    ox, oy = nes_origin(size, (sx, sy), scale)
    rw, rh = _round(w * sx), _round(h * sy)
    if align > 1:
        rw, rh = rw - rw % align, rh - rh % align
    return Rect(_round(ox + x * sx), _round(oy + y * sy), rw, rh).clamp(size)


def third_bands(width: int, count: int = 3) -> list[tuple[int, int]]:
    """(x, w) of ``count`` vertical bands covering ``width`` exactly."""
    edges = [round(i * width / count) for i in range(count + 1)]
    return [(edges[i], edges[i + 1] - edges[i]) for i in range(count)]


# ---------------------------------------------------------------------------
# Filters (format and colour only)

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


def select_frame(frame: int) -> str:
    return f"select='eq(n\\,{frame})'"


def sws_matrix(colour: dict | None, hdr: bool) -> str:
    """The swscale matrix a render was written with (see SWS_MATRIX)."""
    tag = (colour or {}).get("color_space")
    if tag in SWS_MATRIX:
        return SWS_MATRIX[tag]
    return HDR_DEFAULT_MATRIX if hdr else SDR_DEFAULT_MATRIX


def sws_range(colour: dict | None) -> str:
    return "pc" if (colour or {}).get("color_range") in ("pc", "jpeg") else "tv"


def to_rgb(matrix: str, range_: str, pix_fmt: str) -> str:
    """YCbCr to RGB with the render's own matrix and range; no size change."""
    return f"scale=in_color_matrix={matrix}:in_range={range_},format={pix_fmt}"


def sdr_video_filter(matrix: str = SDR_DEFAULT_MATRIX, range_: str = "tv", pix_fmt: str = "yuv420p") -> str:
    """SDR render to tagged BT.709 4:2:0. The matrix change goes through
    16-bit RGB, which every swscale version converts correctly."""
    return (f"scale=in_color_matrix={matrix}:in_range={range_},format=gbrp16le,"
            f"scale=out_color_matrix=bt709:out_range=tv,format={pix_fmt},{SDR_PARAMS}")


def hdr_video_filter(pix_fmt: str = "yuv420p10le") -> str:
    """HDR render (BT.2020 PQ, 4:4:4) to 10-bit 4:2:0: chroma subsampling only.
    setparams puts the colour on the frames, which libsvtav1 reads."""
    return f"format={pix_fmt},{HDR_PARAMS}"


# ---------------------------------------------------------------------------
# Recorder

def record_command(binary: Path | str, rom: Path | str, preset_file: Path | str,
                   state: Path | str, output: Path | str, seconds: float | str, *,
                   replay: Path | str | None = None, record_after: int = 2,
                   size: Sequence[int] = LENS_SIZE, hdr: bool = False,
                   headroom: float = HDR_HEADROOM, white_nits: float = HDR_WHITE_NITS,
                   mask_alignment: str = "pixels", extra_args: Iterable[str] = ()) -> list[str]:
    """mynes_gpu --offscreen WxH ... --record OUT.mov ROM (README, "The recorder").

    The SDR pass renders with --sdr. The HDR pass renders to the EDR target
    and asks the recorder for BT.2020 PQ with the given headroom and white.
    ``mask_alignment`` is "pixels" (the mask at whole output pixels) or
    "physical" (the mask at its own pitch, band-limited)."""
    if mask_alignment not in ("pixels", "physical"):
        raise RecipeError(f"mask_alignment must be pixels or physical, got {mask_alignment!r}")
    cmd = [str(binary), "--offscreen", size_string(size)]
    if not hdr:
        cmd.append("--sdr")
    cmd += ["--mask-alignment", mask_alignment, "--preset", str(preset_file), "--load-state", str(state)]
    if replay:
        cmd += ["--input-replay", str(replay)]
    cmd += ["--record", str(output)]
    if hdr:
        cmd += ["--record-hdr", "--record-headroom", _num(headroom), "--record-hdr-white", _num(white_nits)]
    cmd += ["--record-seconds", seconds if isinstance(seconds, str) else _num(seconds),
            "--record-after", str(int(record_after))]
    cmd += list(extra_args)
    cmd.append(str(rom))
    return cmd


def _num(value: float) -> str:
    return str(int(value)) if float(value).is_integer() else repr(float(value))


# ---------------------------------------------------------------------------
# Stage and lens clips

def _cll(cll: Sequence | None) -> tuple:
    if not cll:
        raise RecipeError("an HDR encode needs max_cll and max_fall from the render's sidecar")
    return cll[0], cll[1]


def video_args(src: Path | str, out: Path | str, spec: VideoOutput, *, cll: Sequence | None = None,
               matrix: str = SDR_DEFAULT_MATRIX, range_: str = "tv", fast: bool = False) -> list[str]:
    """One stage or lens file from the render of the same size.

    HDR: HEVC Main10 (libx265 with HDR10 SEI) or AV1 10-bit (SVT-AV1), both
    tagged BT.2020 PQ with the mastering display and the render's
    MaxCLL/MaxFALL. SDR: H.264 High or HEVC Main tagged BT.709. --fast swaps
    libx265 for hevc_videotoolbox (which writes no HDR10 SEI) and uses faster
    x264 and SVT-AV1 presets."""
    cmd = [*FFMPEG_BASE, "-i", str(src), "-map", "0:v:0"]
    cmd += ["-map", "0:a:0"] if spec.audio else ["-an"]
    if spec.codec == "hevc":
        cmd += _hevc(spec, cll, matrix, range_, fast)
    elif spec.codec == "av1":
        if not spec.hdr:
            raise RecipeError("AV1 is encoded from the HDR render only")
        c, f = _cll(cll)
        cmd += ["-vf", hdr_video_filter(), "-c:v", "libsvtav1",
                "-preset", str(FAST_SVT_PRESET if fast else SVT_PRESET), "-crf", str(spec.crf),
                "-pix_fmt", "yuv420p10le",
                "-svtav1-params", f"enable-hdr=1:mastering-display={SVT_MASTER_DISPLAY}:content-light={c},{f}",
                *HDR_TAGS]
    elif spec.codec == "h264":
        if spec.hdr:
            raise RecipeError("H.264 is encoded from the SDR render only")
        cmd += ["-vf", sdr_video_filter(matrix, range_), "-c:v", "libx264", "-profile:v", "high",
                "-preset", FAST_X264_PRESET if fast else X264_PRESET, "-crf", str(spec.crf),
                "-pix_fmt", "yuv420p", *SDR_TAGS]
    else:
        raise RecipeError(f"unknown codec {spec.codec}")
    cmd += [*PASSTHROUGH]
    if spec.audio:
        cmd += STAGE_AUDIO
    cmd += [*FASTSTART, str(out)]
    return cmd


def _hevc(spec: VideoOutput, cll, matrix: str, range_: str, fast: bool) -> list[str]:
    if fast:
        vf = hdr_video_filter("p010le") if spec.hdr else sdr_video_filter(matrix, range_, "nv12")
        return ["-vf", vf, "-c:v", "hevc_videotoolbox", "-profile:v", "main10" if spec.hdr else "main",
                "-q:v", str(spec.fast_quality), "-tag:v", "hvc1", *(HDR_TAGS if spec.hdr else SDR_TAGS)]
    if spec.hdr:
        c, f = _cll(cll)
        params = ("hdr10=1:repeat-headers=1:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc"
                  f":range=limited:max-cll={c},{f}:master-display={X265_MASTER_DISPLAY}:log-level=error")
        return ["-vf", hdr_video_filter(), "-c:v", "libx265", "-preset", X265_PRESET, "-crf", str(spec.crf),
                "-profile:v", "main10", "-pix_fmt", "yuv420p10le", "-tag:v", "hvc1",
                "-x265-params", params, *HDR_TAGS]
    params = "repeat-headers=1:colorprim=bt709:transfer=bt709:colormatrix=bt709:range=limited:log-level=error"
    return ["-vf", sdr_video_filter(matrix, range_), "-c:v", "libx265", "-preset", X265_PRESET,
            "-crf", str(spec.crf), "-profile:v", "main", "-pix_fmt", "yuv420p", "-tag:v", "hvc1",
            "-x265-params", params, *SDR_TAGS]


# ---------------------------------------------------------------------------
# Stills, posters and README media

def poster_args(src: Path | str, out: Path | str, frame: int = 0, *, matrix: str = SDR_DEFAULT_MATRIX,
                range_: str = "tv", quality: int = POSTER_QUALITY) -> list[str]:
    """Lossy WebP of one frame of the SDR render, at the render's own size."""
    return [*FFMPEG_BASE, "-i", str(src), "-an", "-vf", f"{select_frame(frame)},{to_rgb(matrix, range_, 'bgra')}",
            "-frames:v", "1", "-c:v", "libwebp", "-lossless", "0", "-quality", str(quality), str(out)]


def sdr_png_args(src: Path | str, out: Path | str, frame: int = 0, *, matrix: str = SDR_DEFAULT_MATRIX,
                 range_: str = "tv", rect: Rect | None = None) -> list[str]:
    """Lossless 8-bit PNG of one frame of the SDR render, optionally a 1:1 crop."""
    chain = select_frame(frame) + (f",{rect.crop_filter()}" if rect else "")
    return [*FFMPEG_BASE, "-i", str(src), "-an", "-vf", f"{chain},{to_rgb(matrix, range_, 'rgb24')}",
            "-frames:v", "1", "-c:v", "png", str(out)]


def hdr_raw_args(src: Path | str, out: Path | str, frame: int = 0, *,
                 pix_fmt: str = "yuv444p12le") -> list[str]:
    """One frame of the HDR render as raw planar Y'CbCr in the decoder's own
    format (ffmpeg decodes ProRes 4444 as yuv444p12le), so ffmpeg changes no
    sample. images.yuv_to_rgb48 turns it into 16-bit PQ R'G'B'; then
    images.py cuts the crops, averages the @1x variants and writes the PNGs."""
    if pix_fmt not in HDR_RAW_FORMATS:
        raise RecipeError(f"an HDR still needs a 4:4:4 render of 10, 12 or 16 bits, not {pix_fmt}")
    return [*FFMPEG_BASE, "-i", str(src), "-an", "-vf", select_frame(frame),
            "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", pix_fmt, str(out)]


def avifenc_args(png: Path | str, out: Path | str, *, quality: int = AVIF_QUALITY,
                 speed: int = AVIF_SPEED, clli: Sequence[int] | None = None) -> list[str]:
    """HDR AVIF from a 16-bit PQ PNG: BT.2020 primaries, PQ, BT.2020 matrix,
    10-bit 4:4:4, full range. The PNG carries no colour profile, so avifenc
    is given the code points and told to ignore any metadata."""
    cmd = ["avifenc", "--cicp", "9/16/9", "--depth", "10", "--yuv", "444", "--range", "full",
           "--ignore-icc", "--ignore-exif", "--ignore-xmp", "-q", str(quality), "--speed", str(speed),
           "--jobs", "all"]
    if clli:
        cmd += ["--clli", f"{int(clli[0])},{int(clli[1])}"]
    return cmd + [str(png), str(out)]


def gainmap_args(script: Path | str, sdr_png: Path | str, hdr_png: Path | str, out: Path | str,
                 quality: float = GAINMAP_QUALITY) -> list[str]:
    """gainmap.swift: the SDR render as the JPEG base plus a gain map toward the HDR render."""
    return ["swift", str(script), str(sdr_png), str(hdr_png), str(out), f"{quality:.2f}"]


def readme_frames(source_frames: int) -> int:
    """Every second frame of the source: frames 0, 2, 4, ..."""
    return (source_frames + 1) // 2


def readme_webp_args(src: Path | str, out: Path | str, source_frames: int, quality: int, *,
                     matrix: str = SDR_DEFAULT_MATRIX, range_: str = "tv", fps: int = README_FPS) -> list[str]:
    """Animated WebP of the README render at its own size, every second frame at 30 fps."""
    graph = (f"trim=end_frame={source_frames},select='not(mod(n\\,2))',setpts=N/({fps}*TB),"
             f"{to_rgb(matrix, range_, 'bgra')}")
    return [*FFMPEG_BASE, "-i", str(src), "-an", "-vf", graph,
            "-frames:v", str(readme_frames(source_frames)), *PASSTHROUGH, *WEBP_TIME_BASE,
            "-c:v", "libwebp_anim", "-lossless", "0", "-quality", str(quality),
            "-compression_level", "6", "-loop", "0", str(out)]


def flicker_webp_args(src: Path | str, out: Path | str, rect: Rect, first_frame: int, *,
                      quality: int | str = "lossless", frames: int = FLICKER_FRAMES, fps: int = FLICKER_FPS,
                      matrix: str = SDR_DEFAULT_MATRIX, range_: str = "tv") -> list[str]:
    """Eight consecutive frames of the crop at 1:1 render pixels, 8 fps loop."""
    graph = (f"trim=start_frame={first_frame}:end_frame={first_frame + frames},"
             f"setpts=N/({fps}*TB),{rect.crop_filter()},{to_rgb(matrix, range_, 'bgra')}")
    cmd = [*FFMPEG_BASE, "-i", str(src), "-an", "-vf", graph,
           "-frames:v", str(frames), *PASSTHROUGH, *WEBP_TIME_BASE, "-c:v", "libwebp_anim"]
    if quality == "lossless":
        cmd += ["-lossless", "1"]
    else:
        cmd += ["-lossless", "0", "-quality", str(quality)]
    cmd += ["-compression_level", "6", "-loop", "0", str(out)]
    return cmd


# ---------------------------------------------------------------------------
# Feature clips (built from the full-size SDR renders, never scaled)

def _feature_video(crf: int, preset: str) -> list[str]:
    return ["-c:v", "libx264", "-profile:v", "high", "-crf", str(crf), "-preset", preset,
            "-pix_fmt", "yuv420p", *SDR_TAGS, *PASSTHROUGH]


def five_televisions_args(masters: Sequence[Path | str], captions: Sequence[Path | str],
                          output: Path | str, frames_per_preset: int, rate: Fraction | str,
                          *, crf: int = 14, preset: str = "slow", font: Path | str | None = None,
                          audio: bool = True, master_height: int = LENS_SIZE[1],
                          matrix: str = SDR_DEFAULT_MATRIX, range_: str = "tv") -> list[str]:
    """One game, one television after another.

    Segment i shows frames [i*F, (i+1)*F) of render i, so the game keeps
    running while the set changes; the audio is the first render's, continuous.
    Every render must hold len(masters) * F frames."""
    n = len(masters)
    if n != len(captions):
        raise RecipeError("one caption per master")
    parts = []
    for i in range(n):
        parts.append(f"[{i}:v]trim=start_frame={i * frames_per_preset}"
                     f":end_frame={(i + 1) * frames_per_preset},setpts=PTS-STARTPTS,"
                     f"{drawtext(captions[i], master_height, font=font)}[v{i}]")
    parts.append("".join(f"[v{i}]" for i in range(n)) + f"concat=n={n}:v=1:a=0,setpts=N/FRAME_RATE/TB,"
                 + sdr_video_filter(matrix, range_) + "[v]")
    total = n * frames_per_preset
    cmd = [*FFMPEG_BASE]
    for m in masters:
        cmd += ["-i", str(m)]
    if audio:
        parts.append(f"[0:a]atrim=end={seconds_of(total, Fraction(rate)):.6f},asetpts=PTS-STARTPTS[a]")
    cmd += ["-filter_complex", ";".join(parts), "-map", "[v]"]
    cmd += ["-map", "[a]"] if audio else ["-an"]
    cmd += ["-frames:v", str(total), *_feature_video(crf, preset)]
    if audio:
        cmd += FEATURE_AUDIO
    cmd += [*FASTSTART, str(output)]
    return cmd


def side_by_side_args(masters: Sequence[Path | str], captions: Sequence[Path | str],
                      output: Path | str, frames: int, rate: Fraction | str, *,
                      master_size: Sequence[int] = LENS_SIZE, crf: int = 14, preset: str = "slow",
                      font: Path | str | None = None, audio: bool = True,
                      matrix: str = SDR_DEFAULT_MATRIX, range_: str = "tv") -> list[str]:
    """Three televisions side by side, each showing its own third of the picture."""
    n = len(masters)
    if n != len(captions):
        raise RecipeError("one caption per master")
    bands = third_bands(master_size[0], n)
    parts = []
    for i, (x, w) in enumerate(bands):
        parts.append(f"[{i}:v]trim=end_frame={frames},setpts=PTS-STARTPTS,"
                     f"crop={w}:{master_size[1]}:{x}:0,"
                     f"{drawtext(captions[i], master_size[1], font=font, y=str(int(master_size[1] * 0.05)))}[v{i}]")
    parts.append("".join(f"[v{i}]" for i in range(n)) + f"hstack=inputs={n},setpts=N/FRAME_RATE/TB,"
                 + sdr_video_filter(matrix, range_) + "[v]")
    cmd = [*FFMPEG_BASE]
    for m in masters:
        cmd += ["-i", str(m)]
    if audio:
        parts.append(f"[0:a]atrim=end={seconds_of(frames, Fraction(rate)):.6f},asetpts=PTS-STARTPTS[a]")
    cmd += ["-filter_complex", ";".join(parts), "-map", "[v]"]
    cmd += ["-map", "[a]"] if audio else ["-an"]
    cmd += ["-frames:v", str(frames), *_feature_video(crf, preset)]
    if audio:
        cmd += FEATURE_AUDIO
    cmd += [*FASTSTART, str(output)]
    return cmd


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


def fit_parallel(limit: int, settings: Sequence, build) -> tuple[object, int, list]:
    """Build every setting at once, each to its own file: ``build(setting)``
    returns (path, size). Returns the first setting whose file fits, its size,
    and the other files to remove; the last setting's file is kept when none
    fits. libwebp encodes on one core, so the candidates of a README clip take
    the time of one instead of one after another."""
    from concurrent.futures import ThreadPoolExecutor
    if not settings:
        raise RecipeError("fit needs at least one setting")
    with ThreadPoolExecutor(max_workers=len(settings)) as pool:
        results = list(pool.map(build, list(settings)))
    setting, (path, size) = next(((s, r) for s, r in zip(settings, results) if r[1] <= limit),
                                 (settings[-1], results[-1]))
    return setting, size, [r[0] for r in results if r[0] != path]


# ---------------------------------------------------------------------------
# Guard

# Filters the pipeline uses. None of them changes the picture size except
# crop, which cuts whole pixels; scale appears only as a colour/format
# converter with the options below.
ALLOWED_FILTERS = {"select", "setpts", "trim", "crop", "format", "scale", "setparams", "drawtext",
                   "concat", "hstack", "atrim", "asetpts"}
SCALE_OPTIONS = {"in_color_matrix", "out_color_matrix", "in_range", "out_range"}


# Options that carry a filter graph, by name before any ":" stream specifier.
# -filter with an audio specifier (-filter:a) is not checked; -af is audio.
GRAPH_OPTIONS = {"vf", "filter", "lavfi", "filter_complex"}
# Options that set the picture size, with or without a stream specifier.
SIZE_OPTIONS = {"s", "video_size"}
# Options whose graph lives in a file the guard cannot see.
SCRIPT_OPTIONS = {"filter_script", "filter_complex_script"}


def resampling_problem(cmd: Sequence[str]) -> str | None:
    """Why an ffmpeg command could resample the picture, or None.

    Checks every filter graph (-vf, -filter, -lavfi, -filter_complex, with
    any stream specifier) against ALLOWED_FILTERS and every scale filter for
    size options; refuses output size options (-s, -video_size after the
    inputs) and graphs read from files (-filter_script,
    -filter_complex_script, or an option given as -/name FILE)."""
    args = [str(a) for a in cmd]
    last_input = max((i for i, a in enumerate(args) if a == "-i"), default=-1)
    for i, arg in enumerate(args):
        if not arg.startswith("-") or arg == "-" or i == 0:
            continue
        if arg.startswith("-/"):
            return f"{arg} reads its value from a file"
        name, _, spec = arg[1:].partition(":")
        if name in SCRIPT_OPTIONS:
            return f"{arg} reads a filter graph from a file"
        if name in SIZE_OPTIONS and i > last_input:
            return f"output size option {arg}"
        if name not in GRAPH_OPTIONS or i + 1 >= len(args):
            continue
        if name == "filter" and spec.startswith("a"):
            continue
        for chain in args[i + 1].split(";"):
            for item in _split_filters(chain):
                name, _, options = item.partition("=")
                name = name.partition("@")[0]
                if name not in ALLOWED_FILTERS:
                    return f"filter {name!r} is not one of the non-resampling filters"
                if name == "scale":
                    keys = [o.partition("=")[0] for o in options.split(":") if o]
                    bad = [k for k in keys if k not in SCALE_OPTIONS]
                    if bad or not keys:
                        return f"scale={options} sets {', '.join(bad) or 'a size'}"
    return None


def _split_filters(chain: str) -> list[str]:
    """Filters of one chain, split on unescaped commas outside quotes, with
    [link] labels removed."""
    items, current, quote, escape = [], [], False, False
    for ch in chain:
        if escape:
            current.append(ch)
            escape = False
        elif ch == "\\":
            current.append(ch)
            escape = True
        elif ch == "'":
            current.append(ch)
            quote = not quote
        elif ch == "," and not quote:
            items.append("".join(current))
            current = []
        else:
            current.append(ch)
    items.append("".join(current))
    out = []
    for item in items:
        item = item.strip()
        while item.startswith("["):
            item = item[item.index("]") + 1:]
        while item.endswith("]"):
            item = item[:item.rindex("[")]
        if item:
            out.append(item)
    return out
