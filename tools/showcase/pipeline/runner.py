"""Running and logging commands, verifying outputs, and a small job pool.

Every command goes through Runner.run so --dry-run can print it instead and
the log file records what actually ran."""
from __future__ import annotations

import concurrent.futures
import datetime as _dt
import json
import os
import shlex
import shutil
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from fractions import Fraction
from pathlib import Path
from typing import Callable, Iterable, Sequence


class PipelineError(RuntimeError):
    pass


def _resampling_problem(cmd: Sequence[str]) -> str | None:
    from .recipes import resampling_problem
    return resampling_problem(cmd)


class Runner:
    def __init__(self, *, dry_run: bool = False, force: bool = False,
                 log_path: Path | None = None, quiet: bool = False):
        self.dry_run = dry_run
        self.force = force
        self.quiet = quiet
        self._lock = threading.Lock()
        self._log = None
        if log_path and not dry_run:
            log_path.parent.mkdir(parents=True, exist_ok=True)
            self._log = log_path.open("a")
        self.ran: list[list[str]] = []

    # -- logging ------------------------------------------------------------
    def say(self, message: str) -> None:
        stamp = _dt.datetime.now().strftime("%H:%M:%S")
        line = f"[{stamp}] {message}"
        with self._lock:
            if not self.quiet:
                print(line, flush=True)
            if self._log:
                self._log.write(line + "\n")
                self._log.flush()

    def close(self) -> None:
        if self._log:
            self._log.close()
            self._log = None

    # -- commands -----------------------------------------------------------
    def run(self, cmd: Sequence[str], *, env: dict | None = None, cwd: Path | None = None,
            timeout: float | None = 3600, log_file: Path | None = None,
            what: str = "") -> None:
        cmd = [str(c) for c in cmd]
        if cmd[0] == "ffmpeg":
            problem = _resampling_problem(cmd)
            if problem:
                raise PipelineError(f"refusing to run {what or 'ffmpeg'}: {problem}; renders are "
                                    f"never resampled (tools/showcase/pipeline/recipes.py)")
        cmd[0] = tool(cmd[0])
        prefix = "dry-run $ " if self.dry_run else "$ "
        self.say(prefix + shlex.join(cmd) + (f"   # {what}" if what else ""))
        self.ran.append(cmd)
        if self.dry_run:
            return
        stdout = subprocess.PIPE
        handle = None
        if log_file:
            log_file.parent.mkdir(parents=True, exist_ok=True)
            handle = log_file.open("w")
            handle.write(shlex.join(cmd) + "\n\n")
            handle.flush()
            stdout = handle
        try:
            result = subprocess.run(cmd, env=env, cwd=cwd, timeout=timeout,
                                    stdout=stdout, stderr=subprocess.STDOUT)
        except FileNotFoundError as e:
            raise PipelineError(f"{cmd[0]}: not found ({e})") from e
        except subprocess.TimeoutExpired as e:
            raise PipelineError(f"{what or cmd[0]} timed out after {timeout} s") from e
        finally:
            if handle:
                handle.close()
        if result.returncode != 0:
            tail = ""
            if log_file:
                tail = _tail(log_file)
            elif result.stdout:
                tail = result.stdout.decode("utf-8", "replace")[-4000:]
            raise PipelineError(f"{what or cmd[0]} failed (exit {result.returncode})"
                                + (f":\n{tail}" if tail else ""))

    def capture(self, cmd: Sequence[str], timeout: float = 600) -> str:
        """Run a query command (ffprobe, --help) and return stdout; never dry-run."""
        cmd = [str(c) for c in cmd]
        cmd[0] = tool(cmd[0])
        result = subprocess.run(cmd, capture_output=True, timeout=timeout)
        if result.returncode != 0:
            raise PipelineError(f"{cmd[0]} failed (exit {result.returncode}): "
                                f"{result.stderr.decode('utf-8', 'replace')[-2000:]}")
        return result.stdout.decode("utf-8", "replace")

    # -- freshness ----------------------------------------------------------
    def up_to_date(self, outputs: Iterable[Path], inputs: Iterable[Path]) -> bool:
        if self.force:
            return False
        outputs, inputs = list(outputs), list(inputs)
        if not outputs or not all(o.exists() for o in outputs):
            return False
        newest_input = max((i.stat().st_mtime for i in inputs if i.exists()), default=0)
        return all(o.stat().st_mtime >= newest_input for o in outputs)

    def write_text(self, path: Path, text: str) -> None:
        if self.dry_run:
            self.say(f"dry-run: would write {path}")
            return
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def write_json(self, path: Path, data) -> None:
        self.write_text(path, json.dumps(data, indent=2, ensure_ascii=False) + "\n")

    def step(self, description: str, fn: Callable[[], object]):
        """A Python step (crop, average, PNG write): logged, skipped in a dry run."""
        self.say(("dry-run: would " if self.dry_run else "") + description)
        return None if self.dry_run else fn()

    def copy(self, src: Path, dst: Path) -> None:
        self.say(f"{'dry-run ' if self.dry_run else ''}copy {src} -> {dst}")
        if self.dry_run:
            return
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)


def _tail(path: Path, limit: int = 4000) -> str:
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return ""
    return text[-limit:]


# ---------------------------------------------------------------------------
# Probing and verification

@dataclass
class VideoInfo:
    path: Path
    codec: str
    width: int
    height: int
    rate: Fraction
    frames: int
    audio_codec: str | None
    audio_rate: int | None
    colour: dict = field(default_factory=dict)
    duration: float = 0.0
    pix_fmt: str = ""
    profile: str = ""
    codec_tag: str = ""
    stream: dict = field(default_factory=dict, repr=False)

    @property
    def codecs(self) -> str:
        """The browser codecs string of the video stream (codecs.py)."""
        from .codecs import CodecStringError, stream_codec_string
        try:
            return stream_codec_string(self.stream)
        except CodecStringError as e:
            raise PipelineError(f"{self.path}: {e}") from e


def ffprobe_json(path: Path, ffprobe: str | None = None) -> dict:
    """Streams and format, with -show_data so each stream carries its
    decoder configuration record (extradata) for the codecs string."""
    cmd = [ffprobe or tool("ffprobe"), "-v", "error", "-show_streams", "-show_format", "-show_data",
           "-of", "json", str(path)]
    result = subprocess.run(cmd, capture_output=True, timeout=600)
    if result.returncode != 0:
        raise PipelineError(f"ffprobe failed on {path}: {result.stderr.decode('utf-8', 'replace')[-2000:]}")
    return json.loads(result.stdout.decode("utf-8", "replace"))


def probe_video(path: Path, *, count_frames: bool = False, ffprobe: str | None = None) -> VideoInfo:
    """Stream facts, with an exact frame count.

    ``nb_frames`` comes from the container (mp4/mov keep it). When it is
    missing, or ``count_frames`` is set, the frames are decoded and counted."""
    path = Path(path)
    if not path.exists():
        raise PipelineError(f"{path} does not exist")
    data = ffprobe_json(path, ffprobe)
    video = next((s for s in data.get("streams", []) if s.get("codec_type") == "video"), None)
    if not video:
        raise PipelineError(f"{path}: no video stream")
    audio = next((s for s in data.get("streams", []) if s.get("codec_type") == "audio"), None)
    frames = int(video.get("nb_frames") or 0)
    if count_frames or not frames:
        frames = count_video_frames(path, ffprobe)
    rate = Fraction(video.get("r_frame_rate") or video.get("avg_frame_rate") or "0/1")
    colour = {k: video.get(k) for k in ("color_space", "color_primaries", "color_transfer", "color_range")
              if video.get(k) and video.get(k) != "unknown"}
    return VideoInfo(
        path=path, codec=video.get("codec_name", ""), width=int(video.get("width", 0)),
        height=int(video.get("height", 0)), rate=rate, frames=frames,
        audio_codec=audio.get("codec_name") if audio else None,
        audio_rate=int(audio["sample_rate"]) if audio and audio.get("sample_rate") else None,
        colour=colour, duration=float(data.get("format", {}).get("duration") or 0.0),
        pix_fmt=video.get("pix_fmt", ""), profile=video.get("profile", ""),
        codec_tag=video.get("codec_tag_string", ""), stream=video)


def count_video_frames(path: Path, ffprobe: str | None = None) -> int:
    cmd = [ffprobe or tool("ffprobe"), "-v", "error", "-count_frames", "-select_streams", "v:0",
           "-show_entries", "stream=nb_read_frames", "-of", "default=nw=1:nk=1", str(path)]
    result = subprocess.run(cmd, capture_output=True, timeout=3600)
    if result.returncode != 0:
        raise PipelineError(f"ffprobe -count_frames failed on {path}")
    try:
        return int(result.stdout.decode().strip().splitlines()[0])
    except (IndexError, ValueError) as e:
        raise PipelineError(f"ffprobe gave no frame count for {path}") from e


def image_info(path: Path) -> tuple[int, tuple[int, int]]:
    """(frames, (w, h)) of a PNG, WebP or GIF via Pillow (ffprobe cannot demux
    animated WebP on every build)."""
    try:
        from PIL import Image
    except ImportError as e:
        raise PipelineError("Pillow is required to verify images: pip install Pillow") from e
    with Image.open(path) as im:
        return int(getattr(im, "n_frames", 1)), im.size


def verify_video(path: Path, *, frames: int | None = None, size: Sequence[int] | None = None,
                 rate: Fraction | None = None, audio: bool | None = None,
                 count_frames: bool = False, codec: str | None = None, pix_fmt: str | None = None,
                 colour: dict | None = None) -> VideoInfo:
    """Probe ``path`` and fail loudly on any difference from what was asked for.
    ``colour`` maps ffprobe keys (color_transfer, ...) to the tags expected."""
    info = probe_video(path, count_frames=count_frames)
    problems = []
    if codec is not None and info.codec != codec:
        problems.append(f"codec {info.codec}, expected {codec}")
    if pix_fmt is not None and info.pix_fmt != pix_fmt:
        problems.append(f"pixel format {info.pix_fmt}, expected {pix_fmt}")
    for key, want in (colour or {}).items():
        if info.colour.get(key) != want:
            problems.append(f"{key} {info.colour.get(key) or 'untagged'}, expected {want}")
    if frames is not None and info.frames != frames:
        problems.append(f"{info.frames} frames, expected {frames}")
    if size is not None and (info.width, info.height) != tuple(size):
        problems.append(f"{info.width}x{info.height}, expected {size[0]}x{size[1]}")
    if rate is not None and info.rate != rate:
        problems.append(f"frame rate {info.rate}, expected {rate}")
    if audio is True and not info.audio_codec:
        problems.append("no audio stream")
    if audio is False and info.audio_codec:
        problems.append("unexpected audio stream")
    if problems:
        raise PipelineError(f"{path}: " + "; ".join(problems))
    return info


def verify_image(path: Path, *, frames: int | None = None, size: Sequence[int] | None = None,
                 limit: int | None = None) -> tuple[int, tuple[int, int], int]:
    if not path.exists():
        raise PipelineError(f"{path} was not written")
    n, dims = image_info(path)
    bytes_ = path.stat().st_size
    problems = []
    if frames is not None and n != frames:
        problems.append(f"{n} frames, expected {frames}")
    if size is not None and dims != tuple(size):
        problems.append(f"{dims[0]}x{dims[1]}, expected {size[0]}x{size[1]}")
    if limit is not None and bytes_ > limit:
        problems.append(f"{bytes_} bytes exceeds the {limit} byte limit")
    if problems:
        raise PipelineError(f"{path}: " + "; ".join(problems))
    return n, dims, bytes_


def animation_durations(path: Path) -> list[int]:
    """Each stored frame's duration in milliseconds (Pillow sets it on load)."""
    from PIL import Image
    durations = []
    with Image.open(path) as im:
        for i in range(getattr(im, "n_frames", 1)):
            im.seek(i)
            im.load()
            durations.append(int(im.info.get("duration") or 0))
    return durations


def timing_problem(durations: Sequence[int], frames: int, fps: float) -> str | None:
    """Why stored frame durations do not play ``frames`` frames at ``fps``.

    Each stored frame must start within 1 ms of a multiple of 1000/fps (a
    merged run of identical frames starts where its first frame would), and
    the loop must last frames * 1000/fps to within 1 ms."""
    period = 1000.0 / fps
    start = 0
    for i, d in enumerate(durations):
        if d <= 0:
            return f"frame {i} has duration {d} ms"
        if abs(start - round(start / period) * period) > 1:
            return f"frame {i} starts at {start} ms, off the {period:g} ms grid ({list(durations)})"
        start += d
    if abs(start - frames * period) > 1:
        return f"the loop lasts {start} ms, expected {frames * period:g} ms ({list(durations)})"
    return None


def verify_animation(path: Path, *, frames: int, size: Sequence[int], limit: int | None = None,
                     fps: float | None = None) -> int:
    """An animated WebP of up to ``frames`` frames, each shown for 1/fps s.
    libwebp's animation encoder stores a run of identical frames as one
    longer frame, so a still stretch of picture gives fewer frames, never
    more, and the same total length."""
    n, _dims, _bytes = verify_image(path, size=size, limit=limit)
    if not 1 <= n <= frames:
        raise PipelineError(f"{path}: {n} frames, expected at most {frames}")
    if fps:
        verify_timing(path, frames=frames, fps=fps)
    return n


def verify_timing(path: Path, *, frames: int, fps: float) -> None:
    problem = timing_problem(animation_durations(path), frames, fps)
    if problem:
        raise PipelineError(f"{path}: {problem}")


# ---------------------------------------------------------------------------
# Tool discovery

FONT_CANDIDATES = (
    "/System/Library/Fonts/Helvetica.ttc",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/Library/Fonts/Arial.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
)


def find_font(explicit: str | None = None) -> Path | None:
    if explicit:
        p = Path(explicit).expanduser()
        return p if p.exists() else None
    for candidate in FONT_CANDIDATES:
        if Path(candidate).exists():
            return Path(candidate)
    return None


FFMPEG_FULL_BIN = Path("/opt/homebrew/opt/ffmpeg-full/bin")
FFMPEG_TOOLS = {"ffmpeg": "MYNES_FFMPEG", "ffprobe": "MYNES_FFPROBE"}


@dataclass(frozen=True)
class ToolChoice:
    name: str
    path: str | None
    source: str

    def describe(self) -> str:
        return f"{self.name}: {self.path} ({self.source})" if self.path else f"{self.name}: MISSING"


def _executable(path: Path | str) -> str | None:
    p = Path(path).expanduser()
    return str(p) if p.is_file() and os.access(p, os.X_OK) else None


def discover_ffmpeg(ffmpeg: str | None = None, ffprobe: str | None = None, *,
                    env: dict | None = None, keg: Path = FFMPEG_FULL_BIN,
                    which: Callable[[str], str | None] = shutil.which) -> dict[str, ToolChoice]:
    """Pick ffmpeg and ffprobe: --ffmpeg/--ffprobe, then MYNES_FFMPEG/MYNES_FFPROBE,
    then Homebrew's keg-only ffmpeg-full (it has libwebp, drawtext and zscale,
    the plain formula does not), then PATH. When only ffmpeg is given, by flag
    or variable, ffprobe is taken from beside it if it is there."""
    env = os.environ if env is None else env
    explicit = {"ffmpeg": ffmpeg, "ffprobe": ffprobe}
    chosen: dict[str, ToolChoice] = {}
    for name, var in FFMPEG_TOOLS.items():
        choice = None
        for value, source in ((explicit[name], f"--{name}"), (env.get(var), var)):
            if value:
                path = _executable(value) if os.sep in value else which(value)
                if not path:
                    raise PipelineError(f"{source} {value}: not an executable")
                choice = ToolChoice(name, path, source)
                break
        if choice is None and name == "ffprobe" and chosen["ffmpeg"].source in ("--ffmpeg", "MYNES_FFMPEG"):
            beside = _executable(Path(chosen["ffmpeg"].path).with_name("ffprobe"))
            if beside:
                choice = ToolChoice(name, beside, f"beside {chosen['ffmpeg'].source}")
        if choice is None and _executable(keg / name):
            choice = ToolChoice(name, str(keg / name), "ffmpeg-full")
        if choice is None:
            path = which(name)
            choice = ToolChoice(name, path, "PATH" if path else "missing")
        chosen[name] = choice
    return chosen


_TOOLS: dict[str, ToolChoice] = {}
_TOOLS_LOCK = threading.Lock()


def configure_tools(ffmpeg: str | None = None, ffprobe: str | None = None,
                    env: dict | None = None) -> dict[str, ToolChoice]:
    """Resolve ffmpeg/ffprobe once for this process; see discover_ffmpeg."""
    chosen = discover_ffmpeg(ffmpeg, ffprobe, env=env)
    with _TOOLS_LOCK:
        _TOOLS.clear()
        _TOOLS.update(chosen)
    return chosen


def tool(name: str) -> str:
    """The executable to run for ``name``: the configured ffmpeg/ffprobe, else
    whatever PATH finds, else the name itself (the run then fails clearly)."""
    if name in FFMPEG_TOOLS:
        with _TOOLS_LOCK:
            configured = _TOOLS.get(name)
        if configured is None:
            configured = configure_tools()[name]
        return configured.path or name
    if os.sep in name:
        return name
    return shutil.which(name) or name


def have_tool(name: str) -> bool:
    return shutil.which(tool(name)) is not None


def tool_version(name: str, flag: str | None = None) -> str | None:
    """First line of ``tool -version`` (``--version`` for avifenc); None when
    the tool is missing."""
    path = shutil.which(tool(name))
    if not path:
        return None
    flag = flag or ("--version" if name == "avifenc" else "-version")
    try:
        out = subprocess.run([path, flag], capture_output=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return out.stdout.decode("utf-8", "replace").splitlines()[0] if out.stdout else path


def ffmpeg_version_tuple(line: str | None) -> tuple[int, ...] | None:
    if not line:
        return None
    import re
    m = re.search(r"version\s+n?(\d+)\.(\d+)", line)
    return (int(m.group(1)), int(m.group(2))) if m else None


def ffmpeg_has(kind: str, names: Iterable[str]) -> dict[str, bool]:
    """kind is 'encoders' or 'filters'; which of ``names`` this ffmpeg lists."""
    try:
        out = subprocess.run([tool("ffmpeg"), "-hide_banner", f"-{kind}"], capture_output=True,
                             timeout=60).stdout.decode("utf-8", "replace")
    except (OSError, subprocess.TimeoutExpired):
        out = ""
    listed = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2 and not line.startswith(" -") and not line.startswith("="):
            listed.add(parts[1])
    return {n: n in listed for n in names}


# ---------------------------------------------------------------------------
# Jobs

@dataclass
class Job:
    id: str
    run: Callable[["Runner"], object]
    deps: set[str] = field(default_factory=set)
    description: str = ""


@dataclass
class JobResult:
    ok: list[str] = field(default_factory=list)
    failed: dict[str, str] = field(default_factory=dict)
    skipped: list[str] = field(default_factory=list)
    notes: dict[str, object] = field(default_factory=dict)

    @property
    def success(self) -> bool:
        return not self.failed and not self.skipped


def run_jobs(jobs: Sequence[Job], runner: Runner, workers: int = 1) -> JobResult:
    """Run jobs respecting deps; a failed job's dependants are skipped.

    Dry runs and workers <= 1 run serially in dependency order, so the printed
    commands read top to bottom."""
    result = JobResult()
    by_id = {j.id: j for j in jobs}
    for j in jobs:
        unknown = j.deps - set(by_id)
        if unknown:
            raise PipelineError(f"job {j.id} depends on unknown jobs {sorted(unknown)}")
    pending = {j.id for j in jobs}
    done: set[str] = set()

    def ready() -> list[Job]:
        return [by_id[i] for i in sorted(pending) if by_id[i].deps <= done]

    def finish(job: Job, error: Exception | None, note=None):
        pending.discard(job.id)
        done.add(job.id)
        if error is None:
            result.ok.append(job.id)
            if note is not None:
                result.notes[job.id] = note
        else:
            result.failed[job.id] = str(error)
            runner.say(f"FAILED {job.id}: {error}")
            _skip_dependants(job.id)

    def _skip_dependants(failed_id: str):
        changed = True
        while changed:
            changed = False
            for jid in sorted(pending):
                j = by_id[jid]
                if any(d in result.failed or d in result.skipped for d in j.deps):
                    pending.discard(jid)
                    done.add(jid)
                    result.skipped.append(jid)
                    runner.say(f"skipped {jid} (dependency failed)")
                    changed = True

    if workers <= 1 or runner.dry_run:
        while pending:
            batch = ready()
            if not batch:
                raise PipelineError(f"dependency cycle among {sorted(pending)}")
            for job in batch:
                try:
                    note = job.run(runner)
                except Exception as e:  # noqa: BLE001 - reported, not hidden
                    finish(job, e)
                else:
                    finish(job, None, note)
        return result

    running: dict[concurrent.futures.Future, Job] = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        while pending or running:
            for job in ready():
                if job.id in {j.id for j in running.values()}:
                    continue
                if len(running) >= workers:
                    break
                running[pool.submit(job.run, runner)] = job
            if not running:
                if pending:
                    raise PipelineError(f"dependency cycle among {sorted(pending)}")
                break
            finished, _ = concurrent.futures.wait(running, return_when=concurrent.futures.FIRST_COMPLETED)
            for fut in finished:
                job = running.pop(fut)
                try:
                    note = fut.result()
                except Exception as e:  # noqa: BLE001
                    finish(job, e)
                else:
                    finish(job, None, note)
    return result


def summarize(result: JobResult, runner: Runner) -> None:
    runner.say(f"{len(result.ok)} job(s) ok, {len(result.failed)} failed, {len(result.skipped)} skipped")
    for jid, err in result.failed.items():
        runner.say(f"  failed {jid}: {err.splitlines()[0] if err else ''}")


# Variables the emulator reads that would change a capture. The codec
# overrides replace the recorder's 4:4:4 masters (the documented fast choice
# is 4:2:0 H.264), and the headroom variable changes the offscreen target.
CAPTURE_ENV_REMOVED = ("MYNES_REVIEW_INPUT_SCRIPT", "MYNES_REVIEW_START_FRAME", "MYNES_REVIEW_OSD",
                       "MYNES_REVIEW_FRAME", "MYNES_REVIEW_PRESET", "MYNES_PLAYBACK_FRAMES",
                       "MYNES_RECORD_CODEC_ARGS", "MYNES_RECORD_HDR_CODEC_ARGS", "MYNES_OFFSCREEN_HEADROOM")


def env_for_capture(base: dict | None = None, config_home: Path | None = None) -> dict:
    """Isolated config so the maintainer's own settings never leak into a
    capture: the recorder writes its default masters, H.264 4:4:4 for SDR
    and ProRes 4444 for HDR."""
    env = dict(base or os.environ)
    for key in CAPTURE_ENV_REMOVED:
        env.pop(key, None)
    env["MYNES_REVIEW_NO_INPUT"] = "1"
    env["MYNES_FFMPEG"] = tool("ffmpeg")
    if config_home:
        env["XDG_CONFIG_HOME"] = str(config_home)
    return env


def now() -> float:
    return time.time()


def platform_note() -> str:
    return f"{sys.platform} python {sys.version.split()[0]}"
