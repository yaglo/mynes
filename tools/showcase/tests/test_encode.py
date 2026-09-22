"""Integration: every ffmpeg recipe runs on a synthetic master.

A Pillow-drawn 3840x2880, 60-frame pattern is piped into ffmpeg at the exact
NTSC frame rate with a sine audio track, exactly as the recorder would write
it (H.264 in .mov, AAC 44.1 kHz). The full job set then runs against it and
each output is verified for frame count, size, timebase and byte limits, plus
a pixel check that the push-in starts on the whole frame and ends 1:1 on the
window. Set SHOWCASE_TEST_KEEP=1 to keep the outputs for inspection."""
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from fractions import Fraction
from pathlib import Path

from pipeline import jobs as jobs_mod
from pipeline import recipes, shots
from pipeline.runner import PipelineError, Runner, image_info, probe_video, run_jobs

HAVE_TOOLS = shutil.which("ffmpeg") and shutil.which("ffprobe")
try:
    from PIL import Image, ImageDraw, ImageChops, ImageStat
    HAVE_PIL = True
except ImportError:  # pragma: no cover
    HAVE_PIL = False

MASTER_SIZE = (3840, 2880)
FRAMES = 60           # 1 s NTSC = round(60.0988) = 60 frames
PRESETS = ["p_sony", "p_jvc", "p_toshiba", "p_stass", "p_vhs"]


def draw_frame(i: int, size=MASTER_SIZE, hue: int = 0) -> "Image.Image":
    """A plain pattern with motion: gradient, moving square, a bright bar and
    the frame number. Big flat areas keep the test encodes quick."""
    w, h = size
    im = Image.new("RGB", (w, h), (20 + hue, 24, 40 + (i * 3) % 60))
    d = ImageDraw.Draw(im)
    for k in range(0, w, w // 16):
        d.rectangle((k, 0, k + w // 32, h), fill=(30 + hue, 30 + (k * 255 // w), 60))
    x = (i * w // FRAMES) % (w - w // 8)
    d.rectangle((x, h // 3, x + w // 8, h // 3 + h // 8), fill=(250, 250, 250))
    d.rectangle((w // 4, h // 2 + (i % 8) * 4, 3 * w // 4, h // 2 + (i % 8) * 4 + 12), fill=(255, 200, 40))
    d.text((w // 20, h // 20), f"frame {i}", fill=(255, 255, 255))
    return im


def write_master(path: Path, frames: int = FRAMES, size=MASTER_SIZE, hue: int = 0) -> None:
    w, h = size
    cmd = ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-nostdin",
           "-f", "rawvideo", "-pixel_format", "rgb24", "-video_size", f"{w}x{h}",
           "-framerate", recipes.rate_string(recipes.NTSC_RATE), "-i", "pipe:0",
           "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100",
           "-map", "0:v", "-map", "1:a", "-shortest",
           "-c:v", "libx264", "-preset", "ultrafast", "-crf", "18", "-pix_fmt", "yuv420p",
           "-c:a", "aac", "-b:a", "256k", str(path)]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    try:
        for i in range(frames):
            p.stdin.write(draw_frame(i, size, hue).tobytes())
    finally:
        p.stdin.close()
    if p.wait() != 0:
        raise RuntimeError("synthetic master encode failed")


def extract_frame(video: Path, frame: int, out: Path) -> "Image.Image":
    subprocess.run(["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-i", str(video), "-an",
                    "-vf", recipes.select_frame(frame), "-frames:v", "1", str(out)], check=True)
    return Image.open(out).convert("RGB")


def mean_abs_diff(a: "Image.Image", b: "Image.Image") -> float:
    diff = ImageChops.difference(a, b).convert("L")
    return ImageStat.Stat(diff).mean[0]


@unittest.skipUnless(HAVE_TOOLS and HAVE_PIL, "needs ffmpeg, ffprobe and Pillow")
class EncodePipeline(unittest.TestCase):
    """One synthetic 4K master shared by five presets; the whole job set runs."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="showcase-test-")
        root = Path(cls.tmp)
        cls.presets_dir = root / "presets"
        cls.presets_dir.mkdir()
        for p in PRESETS:
            (cls.presets_dir / f"{p}.json").write_text(json.dumps({"name": f"Preset {p[2:].title()}"}))
        cls.master_dir = root / "masters"
        cls.master_dir.mkdir()
        cls.master = cls.master_dir / "master.mov"
        write_master(cls.master)
        data = {
            "defaults": {"presets": PRESETS, "flicker_crop": [64, 72, 128, 96]},
            "presets": {p: {"name": f"Preset {p[2:].title()}", "blurb": "b"} for p in PRESETS},
            "shots": [{"id": "synth", "title": "Synthetic", "scene": "pattern", "rom": "*synth*",
                       "seconds": 1, "readme": ["p_sony"], "thumbnail_frame": 0}],
            "features": [
                {"id": "five-televisions", "type": "five-televisions", "shot": "synth", "presets": PRESETS,
                 "seconds_per_preset": 0.1},
                {"id": "raw-vs-pvm-vs-rf", "type": "side-by-side", "shot": "synth",
                 "presets": ["p_sony", "p_jvc", "p_stass"], "labels": ["Raw", "PVM", "RF"]},
                {"id": "push-in", "type": "push-in", "shot": "synth", "preset": "p_sony", "seconds": 0.5},
            ],
        }
        (root / "shots.json").write_text(json.dumps(data))
        cls.shot_list = shots.load(root / "shots.json", cls.presets_dir)
        cls.out = root / "out"
        cls.ctx = jobs_mod.Context(shot_list=cls.shot_list, out=cls.out, presets_dir=cls.presets_dir,
                                   states_dir=root / "states")
        cls.shot = cls.shot_list.shots[0]
        for p in PRESETS:
            d = cls.ctx.pair_dir(cls.shot, p)
            d.mkdir(parents=True)
            os.symlink(cls.master, d / "master.mov")
        cls.runner = Runner(quiet=True, log_path=cls.out / "showcase.log")
        cls.result = run_jobs(jobs_mod.encode_jobs(cls.ctx, cls.shot_list.select(presets=["p_sony"])),
                              cls.runner, workers=2)
        cls.feature_result = run_jobs(jobs_mod.feature_jobs(cls.ctx), cls.runner, workers=2)
        cls.runner.close()

    @classmethod
    def tearDownClass(cls):
        if os.environ.get("SHOWCASE_TEST_KEEP"):
            print(f"\nkept {cls.tmp}")
        else:
            shutil.rmtree(cls.tmp, ignore_errors=True)

    def pair(self, preset="p_sony") -> Path:
        return self.ctx.pair_dir(self.shot, preset)

    def test_master_is_what_the_recorder_writes(self):
        info = probe_video(self.master)
        self.assertEqual(info.frames, FRAMES)
        self.assertEqual((info.width, info.height), MASTER_SIZE)
        self.assertEqual(info.codec, "h264")
        self.assertEqual(info.audio_codec, "aac")
        self.assertEqual(info.audio_rate, 44100)
        self.assertAlmostEqual(float(info.rate), recipes.NTSC_FPS, places=4)

    def test_all_encode_jobs_succeeded(self):
        self.assertEqual(self.result.failed, {}, self.result.failed)
        self.assertEqual(self.result.skipped, [])
        self.assertEqual(sorted(self.result.ok), sorted([
            "hero:synth/p_sony", "still:synth/p_sony", "reddit:synth/p_sony", "youtube:synth/p_sony",
            "readme:synth/p_sony", "flicker:synth/p_sony"]))

    def test_hero_clip(self):
        info = probe_video(self.pair() / "hero.mp4")
        self.assertEqual(info.frames, FRAMES)
        self.assertEqual((info.width, info.height), (1440, 1080))
        self.assertEqual(info.rate, probe_video(self.master).rate)
        self.assertEqual(info.audio_codec, "aac")
        self.assertEqual(image_info(self.pair() / "hero.poster.webp"), (1, (1440, 1080)))

    def test_lens_still(self):
        self.assertEqual(image_info(self.pair() / "still.4k.png"), (1, MASTER_SIZE))
        self.assertEqual(image_info(self.pair() / "still.4k.webp"), (1, MASTER_SIZE))
        with Image.open(self.pair() / "still.4k.png") as im:
            self.assertEqual(im.mode, "RGB")

    def test_reddit_and_youtube(self):
        info = probe_video(self.pair() / "reddit.mp4")
        self.assertEqual((info.frames, info.width, info.height), (FRAMES, 1440, 1080))
        info = probe_video(self.pair() / "youtube.mp4")
        self.assertEqual((info.frames, info.width, info.height), (FRAMES, *MASTER_SIZE))
        self.assertEqual(info.audio_codec, "aac")
        self.assertTrue(self.result.notes["youtube:synth/p_sony"]["video_copied"])

    def test_readme_animation_and_gif(self):
        report = json.loads((self.pair() / "readme.json").read_text())
        webp = report["readme_webp"]
        self.assertEqual(webp["frames"], 30)
        self.assertLessEqual(webp["bytes"], recipes.LIMITS["readme_webp"])
        self.assertIn(webp["quality"], recipes.README_QUALITIES)
        self.assertEqual(image_info(self.pair() / "readme.webp"), (30, (960, 720)))
        gif = report["readme_gif"]
        self.assertLessEqual(gif["bytes"], recipes.LIMITS["readme_gif"])
        self.assertEqual(image_info(self.pair() / "readme.gif"), (30, (640, 480)))
        with Image.open(self.pair() / "readme.gif") as im:
            self.assertEqual(im.info.get("loop"), 0)

    def test_phase_flicker(self):
        report = json.loads((self.pair() / "readme.json").read_text())["flicker_webp"]
        self.assertEqual(report["crop_master_px"], [960, 864, 1920, 1152])
        self.assertEqual(report["quality"], "lossless")
        self.assertLessEqual(report["bytes"], recipes.LIMITS["flicker_webp"])
        self.assertEqual(image_info(self.pair() / "flicker.webp"), (8, (1920, 1152)))
        self.assertEqual(image_info(self.pair() / "flicker.png"), (1, (1920, 1152)))
        # The static PNG is the master's frame 0 crop, pixel for pixel (both decode the same H.264 frame).
        full = extract_frame(self.master, 0, Path(self.tmp) / "m0.png")
        crop = full.crop((960, 864, 960 + 1920, 864 + 1152))
        with Image.open(self.pair() / "flicker.png") as png:
            self.assertLess(mean_abs_diff(crop, png.convert("RGB")), 0.5)

    def test_feature_jobs_succeeded(self):
        self.assertEqual(self.feature_result.failed, {}, self.feature_result.failed)
        self.assertEqual(self.feature_result.skipped, [])

    def test_five_televisions(self):
        d = self.ctx.feature_dir(self.shot_list.feature("five-televisions"))
        per = recipes.frame_count(0.1)
        self.assertEqual(per, 6)
        for variant, size in (("youtube", MASTER_SIZE), ("reddit", (1440, 1080)), ("site", (1440, 1080))):
            info = probe_video(d / f"{variant}.mp4")
            self.assertEqual((info.frames, info.width, info.height), (5 * per, *size), variant)
            self.assertEqual(info.audio_codec, "aac")
        self.assertEqual(image_info(d / "poster.webp"), (1, (1440, 1080)))
        self.assertEqual((d / "caption-4.txt").read_text(), "Preset Vhs")

    def test_side_by_side(self):
        d = self.ctx.feature_dir(self.shot_list.feature("raw-vs-pvm-vs-rf"))
        info = probe_video(d / "youtube.mp4")
        self.assertEqual((info.frames, info.width, info.height), (FRAMES, *MASTER_SIZE))
        info = probe_video(d / "site.mp4")
        self.assertEqual((info.frames, info.width, info.height), (FRAMES, 1440, 1080))
        self.assertEqual((d / "caption-2.txt").read_text(), "RF")

    def test_push_in_geometry(self):
        d = self.ctx.feature_dir(self.shot_list.feature("push-in"))
        frames = recipes.frame_count(0.5)
        self.assertEqual(frames, 30)
        info = probe_video(d / "youtube.mp4")
        self.assertEqual((info.frames, info.width, info.height), (frames, 1920, 1440))
        self.assertEqual(info.rate, probe_video(self.master).rate)
        for variant in ("reddit", "site"):
            info = probe_video(d / f"{variant}.mp4")
            self.assertEqual((info.frames, info.width, info.height), (frames, 1440, 1080), variant)
        tmp = Path(self.tmp)
        first = extract_frame(d / "youtube.mp4", 0, tmp / "z0.png")
        last = extract_frame(d / "youtube.mp4", frames - 1, tmp / "z1.png")
        full0 = extract_frame(self.master, 0, tmp / "f0.png").resize((1920, 1440), Image.Resampling.BICUBIC)
        window = recipes.push_in_window([64, 72, 128, 96], MASTER_SIZE)
        crop_last = extract_frame(self.master, frames - 1, tmp / "f1.png").crop(
            (window.x, window.y, window.x + window.w, window.y + window.h))
        self.assertLess(mean_abs_diff(first, full0), 4.0)      # starts on the whole frame
        self.assertLess(mean_abs_diff(last, crop_last), 4.0)   # ends 1:1 on the window
        self.assertGreater(mean_abs_diff(first, last), 2.0)    # and actually moved

    def test_idempotent_rerun_skips(self):
        runner = Runner(quiet=True)
        result = run_jobs(jobs_mod.encode_jobs(self.ctx, self.shot_list.select(presets=["p_sony"])), runner)
        self.assertEqual(result.failed, {})
        self.assertEqual(runner.ran, [])  # nothing re-encoded: outputs are newer than the master

    def test_missing_master_is_a_clear_error(self):
        empty = jobs_mod.Context(shot_list=self.shot_list, out=Path(self.tmp) / "empty-out",
                                 presets_dir=self.presets_dir, states_dir=Path(self.tmp) / "states")
        with self.assertRaises(PipelineError) as cm:
            jobs_mod.encode_hero(empty, Runner(quiet=True), self.shot, "p_jvc")
        self.assertIn("showcase.py record --shots synth --presets p_jvc", str(cm.exception))
        self.assertFalse((empty.pair_dir(self.shot, "p_jvc") / "hero.mp4").exists())
        # A dry run assumes the shot's numbers instead and prints the commands.
        runner = Runner(dry_run=True, quiet=True)
        jobs_mod.encode_hero(empty, runner, self.shot, "p_jvc")
        self.assertEqual(len(runner.ran), 2)
        self.assertIn("-fps_mode", runner.ran[0])

    def test_install_and_manifest(self):
        site = Path(self.tmp) / "site"
        (site / "assets" / "hero").mkdir(parents=True)
        existing = {"fps": 60.0988, "aspect": [4, 3],
                    "presets": [{"id": "legacy", "name": "Legacy", "blurb": "keep me"}],
                    "games": [{"id": "old", "title": "Old", "scene": "s", "default_preset": "legacy"}],
                    "clips": {"old": {"legacy": {"video": "assets/old.mp4"}}}}
        (site / "assets" / "hero" / "manifest.json").write_text(json.dumps(existing))
        ctx = jobs_mod.Context(shot_list=self.shot_list, out=self.out, presets_dir=self.presets_dir,
                               states_dir=Path(self.tmp) / "states", site=site)
        runner = Runner(quiet=True)
        merged = jobs_mod.install(ctx, runner, self.shot_list.select(), self.shot_list.features)
        hero = site / "assets" / "hero"
        for name in ("p_sony.mp4", "p_sony.poster.webp", "p_sony.4k.webp", "p_sony.4k.png"):
            self.assertTrue((hero / "synth" / name).exists(), name)
        self.assertFalse((hero / "synth" / "p_jvc.mp4").exists())  # not encoded: not installed, not listed
        self.assertNotIn("p_jvc", merged["clips"]["synth"])
        clip = merged["clips"]["synth"]["p_sony"]
        self.assertEqual(clip["video"], "assets/hero/synth/p_sony.mp4")
        self.assertEqual(clip["still_size"], list(MASTER_SIZE))
        self.assertEqual(merged["clips"]["old"], existing["clips"]["old"])
        self.assertEqual([p["id"] for p in merged["presets"]], ["legacy", *PRESETS])
        self.assertEqual([g["id"] for g in merged["games"]], ["old", "synth"])
        for fid in ("five-televisions", "raw-vs-pvm-vs-rf", "push-in"):
            self.assertTrue((hero / "features" / f"{fid}.mp4").exists())
            self.assertEqual(merged["features"][fid]["video"], f"assets/hero/features/{fid}.mp4")
        on_disk = json.loads((hero / "manifest.json").read_text())
        self.assertEqual(on_disk, merged)
        # A second install copies nothing: everything is up to date.
        runner2 = Runner(quiet=True)
        jobs_mod.install(ctx, runner2, self.shot_list.select(), self.shot_list.features)
        self.assertEqual(runner2.ran, [])


@unittest.skipUnless(HAVE_TOOLS and HAVE_PIL, "needs ffmpeg, ffprobe and Pillow")
class FeatureComposition(unittest.TestCase):
    """Small, distinct masters prove the five-televisions segment selection and
    the side-by-side bands take pixels from the right television."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="showcase-comp-")
        cls.root = Path(cls.tmp)
        cls.size = (384, 288)
        cls.masters = []
        for i in range(3):
            m = cls.root / f"m{i}.mov"
            write_master(m, frames=30, size=cls.size, hue=i * 60)
            cls.masters.append(m)
        cls.font = __import__("pipeline.runner", fromlist=["find_font"]).find_font()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_five_televisions_switches_master_every_segment(self):
        caps = []
        for i in range(3):
            c = self.root / f"c{i}.txt"
            c.write_text(f"TV {i}")
            caps.append(c)
        out = self.root / "five.mp4"
        rate = probe_video(self.masters[0]).rate
        cmd = recipes.five_televisions_args(self.masters, caps, out, 10, rate, font=self.font,
                                            master_height=self.size[1], preset="ultrafast")
        subprocess.run(cmd, check=True)
        info = probe_video(out)
        self.assertEqual(info.frames, 30)
        self.assertEqual(info.rate, rate)
        for seg in range(3):
            got = extract_frame(out, seg * 10 + 3, self.root / "g.png")
            want = extract_frame(self.masters[seg], seg * 10 + 3, self.root / "w.png")
            other = extract_frame(self.masters[(seg + 1) % 3], seg * 10 + 3, self.root / "o.png")
            box = (0, 0, self.size[0], self.size[1] // 2)  # above the caption
            self.assertLess(mean_abs_diff(got.crop(box), want.crop(box)), 3.0, seg)
            self.assertGreater(mean_abs_diff(got.crop(box), other.crop(box)), 8.0, seg)

    def test_side_by_side_bands(self):
        caps = []
        for i in range(3):
            c = self.root / f"s{i}.txt"
            c.write_text(f"Band {i}")
            caps.append(c)
        out = self.root / "sbs.mp4"
        rate = probe_video(self.masters[0]).rate
        cmd = recipes.side_by_side_args(self.masters, caps, out, 30, rate, master_size=self.size,
                                        font=self.font, preset="ultrafast")
        subprocess.run(cmd, check=True)
        info = probe_video(out)
        self.assertEqual((info.frames, info.width, info.height), (30, *self.size))
        got = extract_frame(out, 20, self.root / "g.png")
        bands = recipes.third_bands(self.size[0])
        for i, (x, w) in enumerate(bands):
            box = (x, self.size[1] // 2, x + w, self.size[1])  # below the label
            want = extract_frame(self.masters[i], 20, self.root / "w.png")
            other = extract_frame(self.masters[(i + 1) % 3], 20, self.root / "o.png")
            self.assertLess(mean_abs_diff(got.crop(box), want.crop(box)), 3.0, i)
            self.assertGreater(mean_abs_diff(got.crop(box), other.crop(box)), 8.0, i)


if __name__ == "__main__":
    unittest.main()
