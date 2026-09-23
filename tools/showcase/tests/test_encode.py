"""Integration: every ffmpeg, avifenc and Swift step runs on synthetic renders.

Small renders stand in for the recorder's output at each size: an SDR pass
(rgb24 piped into libx264 yuv444p with swscale's default BT.601 matrix,
untagged, AAC audio) and an HDR pass (PQ R'G'B' turned into limited-range
BT.2020 Y'CbCr in numpy and piped as yuv444p16le with input tags into ProRes
4444 10-bit), each with the recorder's OUT.json sidecar, as the recorder
writes them. The picture has one-pixel columns of alternating luma (a
stand-in for the aperture grille, which any resampling would blur), flat
colour patches, a moving block, and in the HDR pass a highlight at 800 nits
and a patch at the 10000-nit PQ peak. The whole encode, feature and install
job set runs against them and every output is checked for size, frame
count, timebase, codec, colour tags, HDR metadata and pixels. Set
SHOWCASE_TEST_KEEP=1 to keep the files."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from fractions import Fraction
from pathlib import Path

from pipeline import jobs as jobs_mod
from pipeline import recipes, shots
from pipeline import runner as runner_mod
from pipeline.runner import (PipelineError, Runner, animation_durations, ffmpeg_has, have_tool, image_info,
                             probe_video, run_jobs, tool)

try:
    import numpy as np
    from PIL import Image
    from pipeline import images
    HAVE_LIBS = True
except ImportError:  # pragma: no cover
    HAVE_LIBS = False

NEEDED_ENCODERS = ["libx264", "libx265", "libsvtav1", "libwebp", "libwebp_anim", "prores_ks", "aac", "png"]
HAVE_FFMPEG = have_tool("ffmpeg") and have_tool("ffprobe") and all(ffmpeg_has("encoders", NEEDED_ENCODERS).values())
HAVE_TOOLS = HAVE_FFMPEG and have_tool("avifenc") and HAVE_LIBS
SKIP = "needs ffmpeg with " + ", ".join(NEEDED_ENCODERS) + ", avifenc, numpy and Pillow"

LENS, STAGE, README = (512, 384), [(256, 192), (128, 96)], (320, 240)
FRAMES = 60           # 1 s NTSC = round(60.0988) = 60 frames
PRESETS = ["p_sony", "p_jvc", "p_toshiba", "p_stass", "p_vhs"]
PATCHES = [(200, 40, 40), (40, 180, 60), (50, 60, 200), (230, 200, 60)]
HIGHLIGHT_NITS = 800
PEAK_NITS = 10000
WHITE_NITS = 203


def draw(i: int, size, hdr: bool, hue: int = 0) -> "np.ndarray":
    """Frame i: rgb24 for SDR, rgb48 PQ codes for HDR."""
    w, h = size
    img = np.zeros((h, w, 3), np.float64)
    img[:, :] = (0.08, 0.09, 0.16)
    top = h // 4
    img[:top, i % 2::2] = 0.85                 # one-pixel luma columns, alternating
    img[:top, 1 - i % 2::2] = 0.15             # phase every frame like dot crawl
    pw = w // len(PATCHES)
    for k, colour in enumerate(PATCHES):       # flat colour patches, dimmer on odd frames
        img[top:top * 2, k * pw:(k + 1) * pw] = np.array(colour) / 255.0 * (1.0 if i % 2 == 0 else 0.9)
    x = (i * w // FRAMES) % (w - w // 8)       # a block moving right, one step per frame
    img[top * 2:top * 3, x:x + w // 8] = 0.95
    img = np.roll(img, hue // 60, axis=2)      # distinct televisions for the feature tests
    if not hdr:
        return (img * 255 + 0.5).astype(np.uint8)
    nits = (img ** 2.2) * WHITE_NITS           # the SDR picture at 203 nits white
    nits[highlight_patch(size)] = HIGHLIGHT_NITS
    nits[peak_patch(size)] = PEAK_NITS         # PQ 1.0: Y' 940 of 10 bits
    return (images.nits_to_pq(nits) * 65535 + 0.5).astype("<u2")


def highlight_patch(size, inset: int = 0):
    w, h = size
    return slice(h * 3 // 4 + 4 + inset, h - 4 - inset), slice(w // 3 + inset, 2 * w // 3 - inset)


def peak_patch(size, inset: int = 0):
    w, h = size
    return slice(h * 3 // 4 + 4 + inset, h - 4 - inset), slice(w // 16 + inset, w // 16 + w // 8 - inset)


HDR_COLOUR = jobs_mod.HDR_COLOUR
HDR_TAGS = ["-color_primaries", "bt2020", "-color_trc", "smpte2084", "-colorspace", "bt2020nc",
            "-color_range", "tv"]


def write_render(path: Path, size, hdr: bool, frames: int = FRAMES, hue: int = 0) -> dict:
    """What the recorder writes: OUT.mov plus OUT.json. The HDR pass pipes
    limited-range BT.2020 Y'CbCr at 16 bits with input tags, as the recorder
    does (frontends/gpu/frame_pq.h), so ffmpeg only reduces the bit depth."""
    w, h = size
    head = [tool("ffmpeg"), "-y", "-hide_banner", "-loglevel", "error", "-nostdin", "-f", "rawvideo",
            "-pixel_format", "yuv444p16le" if hdr else "rgb24", "-video_size", f"{w}x{h}", "-r", "60.0988",
            *(HDR_TAGS if hdr else []), "-i", "pipe:0",
            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100", "-map", "0:v", "-map", "1:a", "-shortest"]
    if hdr:
        codec = ["-c:v", "prores_ks", "-profile:v", "4", "-pix_fmt", "yuv444p10le", "-vendor", "apl0", *HDR_TAGS]
    else:
        codec = ["-c:v", "libx264", "-preset", "ultrafast", "-crf", "12", "-pix_fmt", "yuv444p"]
    path.parent.mkdir(parents=True, exist_ok=True)
    p = subprocess.Popen(head + codec + ["-c:a", "aac", "-b:a", "256k", str(path)], stdin=subprocess.PIPE)
    levels = []
    try:
        for i in range(frames):
            frame = draw(i, size, hdr, hue)
            if hdr:
                levels.append(images.light_levels(frame))
                frame = images.rgb48_to_yuv(frame).astype("<u2")
            p.stdin.write(frame.tobytes())
    finally:
        p.stdin.close()
    if p.wait() != 0:
        raise RuntimeError("synthetic render failed")
    sidecar = {"frames": frames, "rate": 60.0988, "width": w, "height": h, "hdr": hdr,
               "white_nits": WHITE_NITS if hdr else 100, "headroom": 4.0 if hdr else 1}
    if hdr:
        sidecar.update(max_cll=max(c for c, _ in levels), max_fall=max(f for _, f in levels))
    path.with_suffix(".json").write_text(json.dumps(sidecar))
    return sidecar


def decode(path: Path, size, *, frame: int = 0, matrix: str = "bt709", range_: str = "tv") -> "np.ndarray":
    """One frame of an SDR video or image as rgb24."""
    out = subprocess.run([tool("ffmpeg"), "-v", "error", "-i", str(path), "-an", "-vf",
                          f"{recipes.select_frame(frame)},scale=in_color_matrix={matrix}:in_range={range_},"
                          f"format=rgb24", "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                         capture_output=True, check=True).stdout
    w, h = size
    return np.frombuffer(out, np.uint8).reshape(h, w, 3).astype(np.int64)


def decode_hdr(path: Path, size, *, frame: int = 0) -> "np.ndarray":
    """One frame of an HDR file as 16-bit PQ R'G'B' codes. A PNG is read as
    rgb48. A video is read as yuv444p16le (ffmpeg widens 10 and 12 bits by a
    plain shift) and converted with images.yuv_to_rgb48, not swscale's rgb48
    output, which is 256/257 too dark."""
    png = Path(path).suffix == ".png"
    fmt = "rgb48le" if png else "yuv444p16le"
    out = subprocess.run([tool("ffmpeg"), "-v", "error", "-i", str(path), "-an", "-vf", recipes.select_frame(frame),
                          "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", fmt, "-"],
                         capture_output=True, check=True).stdout
    w, h = size
    data = np.frombuffer(out, "<u2")
    rgb = data.reshape(h, w, 3) if png else images.yuv_to_rgb48(data.reshape(3, h, w), 16, "bt2020")
    return rgb.astype(np.int64)


def side_data(path: Path) -> dict:
    out = subprocess.run([tool("ffprobe"), "-v", "error", "-select_streams", "v:0", "-show_frames",
                          "-read_intervals", "%+#1", "-of", "json", str(path)], capture_output=True, check=True)
    frame = json.loads(out.stdout)["frames"][0]
    return {d["side_data_type"]: d for d in frame.get("side_data_list", [])}


def ffprobe_mime(path: Path) -> str | None:
    out = subprocess.run([tool("ffprobe"), "-v", "error", "-select_streams", "v:0", "-show_entries",
                          "stream=mime_codec_string", "-of", "default=nw=1:nk=1", str(path)],
                         capture_output=True, text=True)
    return out.stdout.strip() or None


@unittest.skipUnless(HAVE_TOOLS, SKIP)
class EncodePipeline(unittest.TestCase):
    """One set of synthetic renders shared by five presets; the whole job set runs."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="showcase-test-")
        root = Path(cls.tmp)
        cls.presets_dir = root / "presets"
        cls.presets_dir.mkdir()
        for p in PRESETS:
            (cls.presets_dir / f"{p}.json").write_text(json.dumps({"name": f"Preset {p[2:].title()}"}))
        data = {
            "defaults": {"presets": PRESETS, "flicker_crop": [78, 73, 100, 93.75],
                         "sizes": {"lens": "512x384", "stage": ["256x192", "128x96"], "readme": "320x240"}},
            "presets": {p: {"name": f"Preset {p[2:].title()}", "blurb": "b"} for p in PRESETS},
            "shots": [{"id": "synth", "title": "Synthetic", "scene": "pattern", "rom": "*synth*",
                       "seconds": 1, "readme": ["p_sony"], "lens": ["p_sony"], "thumbnail_frame": 0}],
            "features": [
                {"id": "five-televisions", "type": "five-televisions", "shot": "synth", "presets": PRESETS,
                 "seconds_per_preset": 0.1},
                {"id": "raw-vs-pvm-vs-rf", "type": "side-by-side", "shot": "synth",
                 "presets": ["p_sony", "p_jvc", "p_stass"], "labels": ["Raw", "PVM", "RF"]},
            ],
        }
        (root / "shots.json").write_text(json.dumps(data))
        cls.shot_list = shots.load(root / "shots.json", cls.presets_dir)
        cls.shot = cls.shot_list.shots[0]
        cls.out = root / "out"
        cls.ctx = jobs_mod.Context(shot_list=cls.shot_list, out=cls.out, presets_dir=cls.presets_dir,
                                   states_dir=root / "states")
        # The recorder's output for p_sony at every planned size; the other
        # presets only need the full-size SDR render the features read.
        cls.sidecars = {}
        for r in jobs_mod.render_plan(cls.ctx, cls.shot, "p_sony"):
            for hdr in (False, True):
                path = cls.ctx.render_path(cls.shot, "p_sony", r.size, hdr)
                cls.sidecars[(r.size, hdr)] = write_render(path, r.size, hdr, r.frames(hdr))
        full = cls.ctx.render_path(cls.shot, "p_sony", LENS, False)
        for p in PRESETS[1:]:
            dst = cls.ctx.render_path(cls.shot, p, LENS, False)
            dst.parent.mkdir(parents=True)
            os.symlink(full, dst)
            os.symlink(full.with_suffix(".json"), dst.with_suffix(".json"))
        cls.runner = Runner(quiet=True, log_path=cls.out / "showcase.log")
        cls.result = run_jobs(jobs_mod.encode_jobs(cls.ctx, cls.shot_list.select(presets=["p_sony"])),
                              cls.runner, workers=3)
        cls.feature_result = run_jobs(jobs_mod.feature_jobs(cls.ctx), cls.runner, workers=2)
        cls.runner.close()

    @classmethod
    def tearDownClass(cls):
        if os.environ.get("SHOWCASE_TEST_KEEP"):
            print(f"\nkept {cls.tmp}")
        else:
            shutil.rmtree(cls.tmp, ignore_errors=True)

    def d(self, size, preset="p_sony") -> Path:
        return self.ctx.size_dir(self.shot, preset, size)

    def source(self, size, hdr) -> "np.ndarray":
        return draw(0, size, hdr).astype(np.int64)

    # -- the plan and the renders --------------------------------------------
    def test_render_plan(self):
        plan = {r.size: (r.sdr_frames, r.hdr_frames, r.roles)
                for r in jobs_mod.render_plan(self.ctx, self.shot, "p_sony")}
        self.assertEqual(plan[(256, 192)], (60, 60, ("stage",)))
        self.assertEqual(plan[LENS], (60, 60, ("still", "lens", "flicker", "feature")))
        self.assertEqual(plan[README], (60, 1, ("readme",)))  # readme-hdr.png reads frame 0 only
        # Without a lens clip, the full size stops after the frames read from each pass.
        ctx = jobs_mod.Context(shot_list=self.shot_list, out=self.out)
        ctx.shot_list = shots.load(self.shot_list.path, self.presets_dir)
        plan = {r.size: (r.sdr_frames, r.hdr_frames) for r in jobs_mod.render_plan(ctx, ctx.shot_list.shots[0], "p_jvc")}
        self.assertEqual(plan, {(256, 192): (60, 60), (128, 96): (60, 60), LENS: (60, 1)})  # a feature reads SDR
        ctx.shot_list.features = []
        plan = {r.size: (r.sdr_frames, r.hdr_frames) for r in jobs_mod.render_plan(ctx, ctx.shot_list.shots[0], "p_jvc")}
        self.assertEqual(plan, {(256, 192): (60, 60), (128, 96): (60, 60), LENS: (1, 1)})
        ctx.shot_list.shots[0].lens = []
        plan = {r.size: (r.sdr_frames, r.hdr_frames) for r in jobs_mod.render_plan(ctx, ctx.shot_list.shots[0], "p_sony")}
        self.assertEqual(plan[LENS], (8, 1))  # the eight flicker frames; the HDR still and flicker frame 0

    def test_renders_are_what_the_recorder_writes(self):
        info = probe_video(self.ctx.render_path(self.shot, "p_sony", LENS, True))
        self.assertEqual((info.codec, info.frames), ("prores", FRAMES))
        self.assertIn(info.pix_fmt, ("yuv444p10le", "yuv444p12le"))  # ffmpeg decodes ProRes 4444 as 12-bit
        self.assertEqual(info.colour["color_transfer"], "smpte2084")
        self.assertEqual(info.rate, Fraction(150247, 2500))
        info = probe_video(self.ctx.render_path(self.shot, "p_sony", LENS, False))
        self.assertEqual((info.codec, info.pix_fmt), ("h264", "yuv444p"))
        self.assertNotIn("color_space", info.colour)  # untagged: swscale's BT.601

    # -- stage and lens clips ------------------------------------------------
    def test_all_encode_jobs_succeeded(self):
        self.assertEqual(self.result.failed, {}, self.result.failed)
        self.assertEqual(self.result.skipped, [])
        expected = {f"{spec.name}:synth/p_sony/{recipes.size_string(z)}" for z in STAGE for spec in recipes.STAGE_OUTPUTS}
        expected |= {f"poster:synth/p_sony/{recipes.size_string(z)}" for z in STAGE}
        expected |= {f"{spec.name}:synth/p_sony" for spec in recipes.LENS_OUTPUTS}
        expected |= {"still:synth/p_sony", "readme:synth/p_sony", "flicker:synth/p_sony"}
        self.assertEqual(set(self.result.ok), expected)

    def test_stage_clips(self):
        rate = probe_video(self.ctx.render_path(self.shot, "p_sony", STAGE[0], False)).rate
        patterns = {"hevc": r"^hvc1\.2\.4\.L\d+(\.[0-9A-F]+)+$", "av1": r"^av01\.0\.\d\dM\.10\.0\.110\.09\.16\.09\.0$",
                    "h264": r"^avc1\.6400[0-9a-f]{2}$"}
        for size in STAGE:
            for spec in recipes.STAGE_OUTPUTS:
                path = self.d(size) / spec.file
                info = probe_video(path)
                self.assertEqual((info.frames, info.width, info.height), (FRAMES, *size), path)
                self.assertEqual(info.rate, rate)
                self.assertEqual(info.audio_codec, "aac")
                self.assertRegex(info.codecs, patterns[spec.codec])
                if spec.codec == "hevc":
                    self.assertEqual((info.codec_tag, info.profile), ("hvc1", "Main 10"))
                if spec.codec == "h264":
                    self.assertEqual(info.profile, "High")
                    self.assertEqual(info.colour["color_space"], "bt709")
                else:
                    self.assertEqual((info.colour["color_primaries"], info.colour["color_transfer"]),
                                     ("bt2020", "smpte2084"))
                mime = ffprobe_mime(path)
                if mime and spec.codec == "av1":  # ffprobe 7+ derives the AV1 string itself
                    self.assertEqual(info.codecs, mime)

    def test_hdr10_metadata(self):
        sidecar = self.sidecars[(STAGE[0], True)]
        for name in ("stage-hdr-hevc.mp4", "stage-hdr-av1.mp4"):
            data = side_data(self.d(STAGE[0]) / name)
            mastering = data["Mastering display metadata"]
            self.assertEqual(Fraction(mastering["max_luminance"]), 1000, name)
            self.assertAlmostEqual(float(Fraction(mastering["red_x"])), 0.680, places=3)
            light = data["Content light level metadata"]
            self.assertEqual((light["max_content"], light["max_average"]),
                             (sidecar["max_cll"], sidecar["max_fall"]), name)

    def test_hdr_survives_the_encode(self):
        size = STAGE[0]
        got = decode_hdr(self.d(size) / "stage-hdr-hevc.mp4", size)
        want = self.source(size, True)
        patch = highlight_patch(size, inset=4)
        highlight = images.pq_to_nits(got[patch].astype(np.uint16)).mean()
        self.assertAlmostEqual(highlight, HIGHLIGHT_NITS, delta=HIGHLIGHT_NITS * 0.01)
        self.assertGreater(highlight, 3 * WHITE_NITS)  # above SDR white, as HDR should be
        self.assertLess(np.abs(got[patch] - want[patch]).mean(), 150)  # x265 moves chroma by about one code

    def test_sdr_colour_matrix(self):
        """The untagged BT.601 render becomes tagged BT.709 with the same colours."""
        size = STAGE[0]
        got = decode(self.d(size) / "stage-sdr.mp4", size, matrix="bt709")
        w, h = size
        pw = w // len(PATCHES)
        for k, colour in enumerate(PATCHES):
            centre = got[h // 4 + 8:h // 2 - 8, k * pw + 6:(k + 1) * pw - 6].reshape(-1, 3).mean(axis=0)
            self.assertLess(np.abs(centre - colour).max(), 4, (k, centre, colour))

    def test_mask_columns_are_not_resampled(self):
        size = STAGE[0]
        got = decode(self.d(size) / "stage-sdr.mp4", size)
        rows = got[4:size[1] // 4 - 4, 8:-8].mean(axis=2)
        even, odd = rows[:, 0::2].mean(), rows[:, 1::2].mean()
        self.assertGreater(even - odd, 150)  # 0.85 vs 0.15 survives; any scaling would average it away

    def test_lens_clips(self):
        stage = probe_video(self.d(STAGE[0]) / "stage-sdr.mp4")
        for spec in recipes.LENS_OUTPUTS:
            info = probe_video(self.d(LENS) / spec.file)
            self.assertEqual((info.frames, info.width, info.height), (stage.frames, *LENS), spec.name)
            self.assertEqual(info.rate, stage.rate)
            self.assertIsNone(info.audio_codec)
        info = probe_video(self.d(LENS) / "lens-sdr-hevc.mp4")
        self.assertEqual((info.profile, info.colour["color_space"]), ("Main", "bt709"))
        self.assertRegex(info.codecs, r"^hvc1\.1\.6\.L\d+")
        # Same start: frame 10 of the lens and of the stage show the block at the same place.
        for path, size in ((self.d(LENS) / "lens-sdr-hevc.mp4", LENS), (self.d(STAGE[0]) / "stage-sdr.mp4", STAGE[0])):
            w, h = size
            x = (10 * w // FRAMES) % (w - w // 8) + w // 16
            got = decode(path, size, frame=10)
            self.assertGreater(got[h * 5 // 8, x].mean(), 200, path.name)

    def backup(self, *paths):
        """Copy files aside and put them back when the test ends."""
        for path in paths:
            saved = path.with_name(path.name + ".saved")
            shutil.copy2(path, saved)
            self.addCleanup(shutil.move, saved, path)

    def test_fast_uses_videotoolbox(self):
        if not ffmpeg_has("encoders", ["hevc_videotoolbox"])["hevc_videotoolbox"]:
            self.skipTest("no hevc_videotoolbox")
        fast = jobs_mod.Context(shot_list=self.shot_list, out=self.out, presets_dir=self.presets_dir, fast=True)
        cases = ((STAGE[1], recipes.STAGE_OUTPUTS[0], "yuv420p10le", HDR_COLOUR, r"^hvc1\.2\.4\."),
                 (LENS, recipes.LENS_OUTPUTS[2], "yuv420p", jobs_mod.SDR_COLOUR, r"^hvc1\.1\.6\."))
        for size, spec, pix_fmt, colour, codecs in cases:
            out = self.d(size) / spec.file
            self.backup(out, jobs_mod.encode_record_path(out))
            # No --force: the full encode is newer than the render, but was made by another command.
            runner = Runner(quiet=True)
            note = jobs_mod.encode_video(fast, runner, self.shot, "p_sony", size, spec)
            self.assertEqual(len(runner.ran), 1, spec.name)
            self.assertIn("hevc_videotoolbox", runner.ran[0])
            self.assertRegex(note["codecs"], codecs)
            info = probe_video(out)
            self.assertEqual((info.frames, info.width, info.height, info.pix_fmt), (FRAMES, *size, pix_fmt))
            self.assertEqual({k: info.colour[k] for k in colour}, colour)
            self.assertEqual(jobs_mod.read_encode_record(out)["encoder"], "hevc_videotoolbox")
        said = []
        runner = Runner(quiet=True)
        runner.say = said.append
        jobs_mod.collect_clip(self.install_ctx(Path(self.tmp)), runner, self.shot, "p_sony")
        self.assertTrue(any("stage-hdr-hevc.mp4, lens-sdr-hevc.mp4 made with --fast" in s for s in said), said)
        # A later run without --fast encodes the file again with libx265.
        size, spec = cases[0][:2]
        runner = Runner(quiet=True)
        jobs_mod.encode_video(self.ctx, runner, self.shot, "p_sony", size, spec)
        self.assertIn("libx265", runner.ran[0])
        self.assertEqual(jobs_mod.read_encode_record(self.d(size) / spec.file)["fast"], False)

    def test_a_failed_encode_leaves_nothing_behind(self):
        """A file that fails its checks is deleted, so the next run encodes it
        again and install cannot copy it."""
        out = self.d(STAGE[1]) / "stage-sdr.mp4"
        record = jobs_mod.encode_record_path(out)
        self.backup(out, record)
        real = tool("ffmpeg")
        cut = Path(self.tmp) / "ffmpeg-cut"
        cut.write_text(f"#!{sys.executable}\nimport os, sys\na = sys.argv[1:]\n"
                       f"os.execv({real!r}, [{real!r}] + a[:-1] + ['-frames:v', '10', a[-1]])\n")
        cut.chmod(0o755)
        runner_mod.configure_tools(str(cut), tool("ffprobe"))
        try:
            with self.assertRaises(PipelineError) as cm:
                jobs_mod.encode_video(self.ctx, Runner(quiet=True, force=True), self.shot, "p_sony", STAGE[1],
                                      recipes.STAGE_OUTPUTS[2])
        finally:
            runner_mod.configure_tools()
        self.assertIn("10 frames, expected 60", str(cm.exception))
        self.assertFalse(out.exists())
        self.assertFalse(record.exists())
        runner = Runner(quiet=True)
        jobs_mod.encode_video(self.ctx, runner, self.shot, "p_sony", STAGE[1], recipes.STAGE_OUTPUTS[2])
        self.assertEqual(len(runner.ran), 1)
        self.assertEqual(probe_video(out).frames, FRAMES)

    # -- posters, stills, crops ----------------------------------------------
    def test_posters_come_from_the_render_of_their_size(self):
        for size in STAGE:
            poster = self.d(size) / "poster.webp"
            self.assertEqual(image_info(poster), (1, size))
            with Image.open(poster) as im:
                got = np.asarray(im.convert("RGB")).astype(np.int64)
            want = self.source(size, False)
            self.assertLess(np.abs(got[size[1] // 4:] - want[size[1] // 4:]).mean(), 4, size)

    def test_stills(self):
        d = self.d(LENS)
        with Image.open(d / "still-sdr.png") as im:
            self.assertEqual((im.mode, im.size), ("RGB", LENS))
            sdr = np.asarray(im).astype(np.int64)
        self.assertLess(np.abs(sdr - self.source(LENS, False)).max(), 12)
        self.assertLess(np.abs(sdr - self.source(LENS, False)).mean(), 1.5)
        hdr = decode_hdr(d / "still-hdr.png", LENS)
        source = self.source(LENS, True)
        self.assertLess(np.abs(hdr - source).mean(), 40)  # 10-bit steps and ProRes
        self.assertLess(abs((hdr - source).mean()), 4)     # no bias: the range expansion is exact
        highlight = images.pq_to_nits(hdr[highlight_patch(LENS, inset=4)].astype(np.uint16)).mean()
        self.assertAlmostEqual(highlight, HIGHLIGHT_NITS, delta=HIGHLIGHT_NITS * 0.01)
        peak = hdr[peak_patch(LENS)]  # Y' 940 is PQ 1.0; ProRes leaves one-code noise around it
        self.assertEqual((np.median(peak), peak.max()), (65535, 65535))
        info = probe_video(d / "still-hdr.avif")
        self.assertEqual((info.width, info.height, info.pix_fmt), (*LENS, "yuv444p10le"))
        self.assertEqual((info.colour["color_primaries"], info.colour["color_transfer"],
                          info.colour["color_space"], info.colour["color_range"]),
                         ("bt2020", "smpte2084", "bt2020nc", "pc"))
        light = next(s for s in info.stream.get("side_data_list", []) if "light" in s["side_data_type"].lower())
        self.assertEqual((light["max_content"], light["max_average"]),
                         images.light_levels(hdr.astype(np.uint16)))
        self.assertEqual(light["max_content"], PEAK_NITS)

    def test_detail_crops_are_1_to_1_with_exact_1x_averages(self):
        d = self.d(LENS)
        rect = recipes.flicker_geometry([78, 73, 100, 93.75], LENS, even_size=True)
        self.assertEqual(rect, recipes.Rect(156, 117, 200, 150))
        with Image.open(d / "still-sdr.png") as still, Image.open(d / "crop-sdr.png") as crop, \
                Image.open(d / "crop-sdr@1x.png") as small:
            self.assertEqual(np.asarray(crop).tolist(), np.asarray(still.crop(rect.box)).tolist())
            self.assertEqual(np.asarray(small).tolist(), np.asarray(crop.reduce(2)).tolist())
        still = decode_hdr(d / "still-hdr.png", LENS)
        crop = decode_hdr(d / "crop-hdr.png", (rect.w, rect.h))
        small = decode_hdr(d / "crop-hdr@1x.png", (rect.w // 2, rect.h // 2))
        self.assertTrue(np.array_equal(crop, still[rect.y:rect.y + rect.h, rect.x:rect.x + rect.w]))
        self.assertTrue(np.array_equal(small, images.box_average_2x2(crop.astype(np.uint16))))
        for name, size in (("crop-hdr.avif", (rect.w, rect.h)), ("crop-hdr@1x.avif", (rect.w // 2, rect.h // 2))):
            info = probe_video(d / name)
            self.assertEqual((info.width, info.height, info.colour["color_transfer"]), (*size, "smpte2084"))

    # -- README media ----------------------------------------------------------
    def test_readme_media(self):
        report = json.loads((self.ctx.clip_dir(self.shot, "p_sony") / "readme.json").read_text())
        self.assertEqual(report["readme_webp"]["frames"], 30)
        self.assertEqual(report["readme_webp"]["embed_width"], README[0] // 2)
        self.assertEqual(image_info(self.d(README) / "readme.webp"), (30, README))
        durations = animation_durations(self.d(README) / "readme.webp")
        self.assertEqual(set(durations), {33, 34})  # 30 fps in whole milliseconds
        self.assertEqual(sum(durations), 1000)
        self.assertEqual(image_info(self.d(README) / "readme.png"), (1, README))
        flicker = report["flicker_webp"]
        rect = recipes.flicker_geometry([78, 73, 100, 93.75], LENS)
        self.assertEqual(flicker["crop_px"], [rect.x, rect.y, rect.w, rect.h])
        self.assertEqual(flicker["quality"], "lossless")
        self.assertEqual(image_info(self.d(LENS) / "flicker.webp"), (8, (rect.w, rect.h)))
        self.assertEqual(animation_durations(self.d(LENS) / "flicker.webp"), [125] * 8)  # an even 8 fps
        with Image.open(self.d(LENS) / "flicker.png") as png, Image.open(self.d(LENS) / "still-sdr.png") as still:
            self.assertEqual(np.asarray(png).tolist(), np.asarray(still.crop(rect.box)).tolist())
        with Image.open(self.d(LENS) / "flicker.webp") as anim:  # lossless: frame 0 is the PNG exactly
            anim.seek(0)
            self.assertEqual(np.asarray(anim.convert("RGB")).tolist(),
                             np.asarray(Image.open(self.d(LENS) / "flicker.png").convert("RGB")).tolist())

    def test_lossy_flicker_fallback(self):
        """The flicker WebP falls back to lossy quality when lossless is over 5 MB."""
        rect = recipes.flicker_geometry([78, 73, 100, 93.75], LENS)
        out = Path(self.tmp) / "flicker-q90.webp"
        Runner(quiet=True).run(recipes.flicker_webp_args(self.ctx.render_path(self.shot, "p_sony", LENS, False), out,
                                                         rect, 0, quality=90))
        self.assertEqual(runner_mod.verify_animation(out, frames=8, size=(rect.w, rect.h), fps=recipes.FLICKER_FPS), 8)
        with Image.open(out) as anim, Image.open(self.d(LENS) / "flicker.png") as png:
            got = np.asarray(anim.convert("RGB")).astype(np.int64)
            self.assertLess(np.abs(got - np.asarray(png).astype(np.int64)).mean(), 4)

    def test_gainmap_jpegs(self):
        ok, reason = jobs_mod.gainmap_available()
        report = json.loads((self.ctx.clip_dir(self.shot, "p_sony") / "readme.json").read_text())
        if not ok:
            self.assertIsNone(report["readme_gainmap"])
            self.skipTest(reason)
        for jpg, png, size in ((self.d(README) / "readme-hdr.jpg", self.d(README) / "readme.png", README),
                               (self.d(LENS) / "flicker-hdr.jpg", self.d(LENS) / "flicker.png",
                                tuple(report["flicker_webp"]["crop_px"][2:]))):
            frames, got = image_info(jpg)
            self.assertEqual((frames, got), (2, size))  # the base image and the gain map (MPO)
            self.assertIn(b"urn:iso:std:iso:ts:21496:-1", jpg.read_bytes())
            # The base image is the SDR render, so a viewer without HDR shows the SDR picture.
            with Image.open(jpg) as base, Image.open(png) as sdr:
                base.seek(0)
                error = np.abs(np.asarray(base.convert("RGB")).astype(np.int64) - np.asarray(sdr.convert("RGB")))
            self.assertLess(error.mean(), 3, jpg.name)

    # -- features ------------------------------------------------------------
    def test_features(self):
        self.assertEqual(self.feature_result.failed, {}, self.feature_result.failed)
        per = recipes.frame_count(0.1)
        info = probe_video(self.out / "features" / "five-televisions" / "youtube.mp4")
        self.assertEqual((info.frames, info.width, info.height), (5 * per, *LENS))
        self.assertEqual((info.audio_codec, info.colour["color_space"]), ("aac", "bt709"))
        info = probe_video(self.out / "features" / "raw-vs-pvm-vs-rf" / "youtube.mp4")
        self.assertEqual((info.frames, info.width, info.height), (FRAMES, *LENS))
        self.assertEqual((self.out / "features" / "raw-vs-pvm-vs-rf" / "caption-2.txt").read_text(), "RF")

    # -- runner behaviour ----------------------------------------------------
    def test_idempotent_rerun_skips(self):
        runner = Runner(quiet=True)
        result = run_jobs(jobs_mod.encode_jobs(self.ctx, self.shot_list.select(presets=["p_sony"])), runner)
        self.assertEqual(result.failed, {})
        self.assertEqual(runner.ran, [])  # nothing re-encoded: outputs are newer than the renders

    def test_missing_render_is_a_clear_error(self):
        empty = jobs_mod.Context(shot_list=self.shot_list, out=Path(self.tmp) / "empty-out",
                                 presets_dir=self.presets_dir)
        spec = recipes.STAGE_OUTPUTS[2]
        with self.assertRaises(PipelineError) as cm:
            jobs_mod.encode_video(empty, Runner(quiet=True), self.shot, "p_jvc", STAGE[0], spec)
        self.assertIn("showcase.py --shots synth --presets p_jvc record", str(cm.exception))
        runner = Runner(dry_run=True, quiet=True)  # a dry run assumes the plan's numbers instead
        jobs_mod.encode_video(empty, runner, self.shot, "p_jvc", STAGE[0], recipes.STAGE_OUTPUTS[0])
        self.assertIn("max-cll=MAXCLL,MAXFALL", " ".join(runner.ran[0]))

    def test_runner_refuses_to_resample(self):
        with self.assertRaises(PipelineError) as cm:
            Runner(dry_run=True, quiet=True).run(["ffmpeg", "-i", "a.mov", "-vf", "scale=960:720", "b.mp4"])
        self.assertIn("never resampled", str(cm.exception))

    # -- install -------------------------------------------------------------
    def make_site(self, name: str) -> Path:
        """A site with a version 1 manifest: one clip with a video, a poster
        and a still, as the live site has them."""
        site = Path(self.tmp) / name
        (site / "assets" / "hero").mkdir(parents=True)
        (site / "assets" / "old.bin").write_bytes(b"\0" * 300_000)
        shutil.copy(self.d(STAGE[1]) / "stage-sdr.mp4", site / "assets" / "old.mp4")
        shutil.copy(self.d(STAGE[1]) / "poster.webp", site / "assets" / "old.webp")
        existing = {"fps": 60.0988, "aspect": [4, 3],
                    "presets": [{"id": "legacy", "name": "Legacy", "blurb": "keep me"}],
                    "games": [{"id": "old", "title": "Old", "scene": "s", "default_preset": "legacy"}],
                    "clips": {"old": {"legacy": {"video": "assets/old.mp4", "poster": "assets/old.webp",
                                                 "still": "assets/old.4k.webp", "still_size": [512, 384]}}},
                    "features": {"push-in": {"video": "assets/hero/features/push-in.mp4"}}}
        (site / "assets" / "hero" / "manifest.json").write_text(json.dumps(existing))
        return site

    def install_ctx(self, site: Path, **kw) -> jobs_mod.Context:
        return jobs_mod.Context(shot_list=self.shot_list, out=self.out, presets_dir=self.presets_dir, site=site, **kw)

    def listening_runner(self) -> tuple[Runner, list[str]]:
        said = []
        runner = Runner(quiet=True)
        runner.say = said.append
        return runner, said

    def test_install_and_manifest(self):
        site = self.make_site("site")
        runner, said = self.listening_runner()
        merged = jobs_mod.install(self.install_ctx(site), runner, self.shot_list.select())
        self.assertFalse([s for s in said if s.startswith("manifest warning")], said)
        hero = site / "assets" / "hero" / "synth" / "p_sony"
        for size in STAGE:
            for name in ("stage-hdr-hevc.mp4", "stage-hdr-av1.mp4", "stage-sdr.mp4", "poster.webp"):
                self.assertTrue((hero / recipes.size_string(size) / name).exists(), name)
        for name in ("lens-hdr-hevc.mp4", "lens-hdr-av1.mp4", "lens-sdr-hevc.mp4", "still-hdr.avif", "still-sdr.png"):
            self.assertTrue((hero / "512x384" / name).exists(), name)
        self.assertFalse((hero / "512x384" / "crop-sdr.png").exists())      # crops only with --with-crops
        self.assertFalse((site / "assets" / "hero" / "synth" / "p_jvc").exists())  # not encoded: not installed
        self.assertEqual(merged["version"], 2)
        clip = merged["clips"]["synth"]["p_sony"]
        self.assertEqual(sorted(clip), ["hdr", "lens", "poster", "stage", "still"])
        self.assertEqual(clip["poster"], [
            {"src": "assets/hero/synth/p_sony/256x192/poster.webp", "width": 256, "height": 192},
            {"src": "assets/hero/synth/p_sony/128x96/poster.webp", "width": 128, "height": 96}])
        self.assertEqual(len(clip["stage"]), 6)
        first = clip["stage"][0]
        self.assertEqual(first["src"], "assets/hero/synth/p_sony/256x192/stage-hdr-hevc.mp4")
        self.assertRegex(first["type"], r'^video/mp4; codecs="hvc1\.2\.4\.L\d+')
        self.assertEqual((first["hdr"], first["width"], first["height"]), (True, 256, 192))
        self.assertEqual(first["bytes"], (hero / "256x192" / "stage-hdr-hevc.mp4").stat().st_size)
        self.assertEqual([s["hdr"] for s in clip["stage"]], [True, True, False] * 2)
        self.assertEqual([s["width"] for s in clip["lens"]], [512] * 3)
        self.assertEqual(clip["still"], {"hdr": "assets/hero/synth/p_sony/512x384/still-hdr.avif",
                                         "sdr": "assets/hero/synth/p_sony/512x384/still-sdr.png",
                                         "width": 512, "height": 384, "frame": 0})
        self.assertEqual(clip["hdr"], {"white_nits": WHITE_NITS, "headroom": 4.0,
                                       "max_cll": max(s["max_cll"] for (z, h), s in self.sidecars.items() if h),
                                       "max_fall": max(s["max_fall"] for (z, h), s in self.sidecars.items() if h)})
        # The version 1 clip is rewritten in version 2 form, with what its files say about themselves.
        old = self.d(STAGE[1]) / "stage-sdr.mp4"
        self.assertEqual(merged["clips"]["old"], {"legacy": {
            "stage": [{"src": "assets/old.mp4", "type": f'video/mp4; codecs="{probe_video(old).codecs}"',
                       "hdr": False, "width": 128, "height": 96, "bytes": old.stat().st_size}],
            "still": {"hdr": None, "sdr": "assets/old.4k.webp", "width": 512, "height": 384, "frame": 0},
            "poster": [{"src": "assets/old.webp", "width": 128, "height": 96}]}})
        self.assertEqual(merged["features"], {"push-in": {"video": "assets/hero/features/push-in.mp4"}})
        self.assertEqual([p["id"] for p in merged["presets"]], ["legacy", *PRESETS])
        self.assertEqual([g["id"] for g in merged["games"]], ["old", "synth"])
        self.assertEqual(json.loads((site / "assets" / "hero" / "manifest.json").read_text()), merged)
        runner2, said = self.listening_runner()
        jobs_mod.install(self.install_ctx(site, with_crops=True), runner2, self.shot_list.select())
        self.assertTrue((hero / "512x384" / "crop-hdr@1x.avif").exists())
        self.assertEqual(runner2.ran, [])  # no commands
        copied = sorted(Path(s.rsplit(" -> ", 1)[1]).name for s in said if s.startswith("copy "))
        self.assertEqual(copied, sorted(jobs_mod.SITE_CROP_FILES))  # only the crops were new
        # A site file that differs from the build is replaced, even when it is newer.
        stale = hero / "256x192" / "stage-sdr.mp4"
        shutil.copy(self.d(STAGE[1]) / "stage-sdr.mp4", stale)
        os.utime(stale, None)
        runner3, said = self.listening_runner()
        merged = jobs_mod.install(self.install_ctx(site), runner3, self.shot_list.select())
        self.assertEqual([s for s in said if s.startswith("copy ")],
                         [f"copy {self.d(STAGE[0]) / 'stage-sdr.mp4'} -> {stale}"])
        self.assertEqual(stale.read_bytes(), (self.d(STAGE[0]) / "stage-sdr.mp4").read_bytes())
        entry = next(e for e in merged["clips"]["synth"]["p_sony"]["stage"] if e["src"].endswith("256x192/stage-sdr.mp4"))
        self.assertEqual((entry["width"], entry["bytes"]), (256, stale.stat().st_size))

    def test_install_needs_the_hdr_sidecars(self):
        """The manifest's light levels come from the HDR sidecars; without
        them install stops before copying anything."""
        sidecar = self.ctx.render_path(self.shot, "p_sony", STAGE[1], True).with_suffix(".json")
        levels = json.loads(sidecar.read_text())
        self.backup(sidecar)
        for i, (text, message) in enumerate(((None, f"{sidecar} is missing"),
                                             (json.dumps({**levels, "max_cll": None}),
                                              f"{sidecar} lacks numeric max_cll and max_fall"))):
            if text is None:
                sidecar.unlink()
            else:
                sidecar.write_text(text)
            site = self.make_site(f"site-sidecar-{i}")
            manifest = (site / "assets" / "hero" / "manifest.json").read_text()
            with self.assertRaises(PipelineError) as cm:
                jobs_mod.install(self.install_ctx(site), Runner(quiet=True), self.shot_list.select())
            self.assertIn(message, str(cm.exception))
            self.assertFalse((site / "assets" / "hero" / "synth").exists())
            self.assertEqual((site / "assets" / "hero" / "manifest.json").read_text(), manifest)

    def test_install_reads_the_manifest_before_copying(self):
        for i, text in enumerate(('{"version": 2, "clips": {', "[1, 2]", '{"clips": {"synth": {"p_sony": "oops"}}}')):
            site = self.make_site(f"site-bad-{i}")
            path = site / "assets" / "hero" / "manifest.json"
            path.write_text(text)
            with self.assertRaises(PipelineError) as cm:
                jobs_mod.install(self.install_ctx(site), Runner(quiet=True), self.shot_list.select())
            self.assertTrue(str(cm.exception).startswith(f"{path}: "), cm.exception)
            self.assertFalse((site / "assets" / "hero" / "synth").exists())
            self.assertEqual(path.read_text(), text)

    def test_install_with_nothing_built_leaves_the_manifest(self):
        site = self.make_site("site-empty")
        path = site / "assets" / "hero" / "manifest.json"
        before = path.read_text()
        empty = jobs_mod.Context(shot_list=self.shot_list, out=Path(self.tmp) / "nothing-built",
                                 presets_dir=self.presets_dir, site=site)
        with self.assertRaises(PipelineError) as cm:
            jobs_mod.install(empty, Runner(quiet=True), self.shot_list.select())
        self.assertIn("nothing to install", str(cm.exception))
        self.assertEqual(path.read_text(), before)  # still version 1: no clip to write

    def test_install_refuses_over_budget(self):
        site = self.make_site("site-small")
        before = sorted(p.relative_to(site).as_posix() for p in site.rglob("*"))
        runner = Runner(quiet=True)
        with self.assertRaises(PipelineError) as cm:
            jobs_mod.install(self.install_ctx(site, budget_mb=0.3), runner, self.shot_list.select())
        message = str(cm.exception)
        self.assertIn("refusing to install", message)
        self.assertIn("Largest files:", message)
        self.assertIn("assets/old.bin", message)
        self.assertRegex(message, r"assets/hero/synth/p_sony/512x384/\S+  \(new\)")
        self.assertEqual(sorted(p.relative_to(site).as_posix() for p in site.rglob("*")), before)  # nothing copied


@unittest.skipUnless(HAVE_FFMPEG and HAVE_LIBS, SKIP)
class FeatureComposition(unittest.TestCase):
    """Small, distinct renders prove the five-televisions segment selection and
    the side-by-side bands take pixels from the right television."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="showcase-comp-")
        cls.root = Path(cls.tmp)
        cls.size = (384, 288)
        cls.masters = []
        for i in range(3):
            m = cls.root / f"m{i}.mov"
            write_render(m, cls.size, False, frames=30, hue=i * 60)
            cls.masters.append(m)
        from pipeline.runner import find_font
        cls.font = find_font()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def captions(self, prefix):
        caps = []
        for i in range(3):
            c = self.root / f"{prefix}{i}.txt"
            c.write_text(f"TV {i}")
            caps.append(c)
        return caps

    def test_five_televisions_switches_render_every_segment(self):
        out = self.root / "five.mp4"
        rate = probe_video(self.masters[0]).rate
        cmd = recipes.five_televisions_args(self.masters, self.captions("c"), out, 10, rate, font=self.font,
                                            master_height=self.size[1], preset="ultrafast")
        Runner(quiet=True).run(cmd)
        info = probe_video(out)
        self.assertEqual((info.frames, info.rate), (30, rate))
        box = (slice(0, self.size[1] // 2), slice(0, self.size[0]))  # above the caption
        for seg in range(3):
            got = decode(out, self.size, frame=seg * 10 + 3)[box]
            want = decode(self.masters[seg], self.size, frame=seg * 10 + 3, matrix="bt601")[box]
            other = decode(self.masters[(seg + 1) % 3], self.size, frame=seg * 10 + 3, matrix="bt601")[box]
            self.assertLess(np.abs(got - want).mean(), 3.0, seg)
            self.assertGreater(np.abs(got - other).mean(), 8.0, seg)

    def test_side_by_side_bands(self):
        out = self.root / "sbs.mp4"
        rate = probe_video(self.masters[0]).rate
        cmd = recipes.side_by_side_args(self.masters, self.captions("s"), out, 30, rate, master_size=self.size,
                                        font=self.font, preset="ultrafast")
        Runner(quiet=True).run(cmd)
        info = probe_video(out)
        self.assertEqual((info.frames, info.width, info.height), (30, *self.size))
        got = decode(out, self.size, frame=20)
        for i, (x, w) in enumerate(recipes.third_bands(self.size[0])):
            box = (slice(self.size[1] // 2, self.size[1]), slice(x, x + w))  # below the label
            want = decode(self.masters[i], self.size, frame=20, matrix="bt601")[box]
            other = decode(self.masters[(i + 1) % 3], self.size, frame=20, matrix="bt601")[box]
            self.assertLess(np.abs(got[box] - want).mean(), 3.0, i)
            self.assertGreater(np.abs(got[box] - other).mean(), 8.0, i)


if __name__ == "__main__":
    unittest.main()
