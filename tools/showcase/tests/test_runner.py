import os
import shutil
import tempfile
import time
import unittest
from pathlib import Path

from pipeline import runner as runner_mod
from pipeline.runner import Job, PipelineError, Runner, run_jobs


class Freshness(unittest.TestCase):
    def test_up_to_date(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            src, out = d / "in", d / "out"
            src.write_text("x")
            r = Runner(dry_run=True)
            self.assertFalse(r.up_to_date([out], [src]))
            out.write_text("y")
            old = time.time() - 100
            os.utime(src, (old, old))
            self.assertTrue(r.up_to_date([out], [src]))
            os.utime(out, (old - 10, old - 10))
            self.assertFalse(r.up_to_date([out], [src]))
            os.utime(out, (old + 10, old + 10))
            self.assertTrue(r.up_to_date([out], [src]))
            self.assertFalse(Runner(dry_run=True, force=True).up_to_date([out], [src]))
            self.assertFalse(r.up_to_date([], [src]))


class DryRun(unittest.TestCase):
    def test_commands_are_printed_not_run(self):
        r = Runner(dry_run=True, quiet=True)
        r.run(["definitely-not-a-program", "--flag"], what="x")
        self.assertEqual(r.ran, [["definitely-not-a-program", "--flag"]])

    def test_failure_is_reported(self):
        r = Runner(quiet=True)
        with self.assertRaises(PipelineError):
            r.run(["definitely-not-a-program"])
        with self.assertRaises(PipelineError) as cm:
            r.run(["python3", "-c", "import sys; print('boom'); sys.exit(3)"], what="py")
        self.assertIn("exit 3", str(cm.exception))
        self.assertIn("boom", str(cm.exception))


class Jobs(unittest.TestCase):
    def make(self, order, fail=()):
        def job(name):
            def run(r):
                if name in fail:
                    raise RuntimeError(f"{name} broke")
                order.append(name)
                return name
            return run
        return job

    def test_serial_order_and_skip(self):
        for workers in (1, 3):
            order = []
            job = self.make(order, fail={"b"})
            jobs = [Job("c", job("c"), deps={"b"}), Job("a", job("a")), Job("b", job("b"), deps={"a"}),
                    Job("d", job("d"), deps={"a"})]
            result = run_jobs(jobs, Runner(quiet=True), workers=workers)
            self.assertEqual(order[0], "a")
            self.assertEqual(sorted(result.ok), ["a", "d"])
            self.assertEqual(list(result.failed), ["b"])
            self.assertEqual(result.skipped, ["c"])
            self.assertFalse(result.success)
            self.assertEqual(result.notes["a"], "a")

    def test_cycle_and_unknown_dep(self):
        with self.assertRaises(PipelineError):
            run_jobs([Job("a", lambda r: None, deps={"b"}), Job("b", lambda r: None, deps={"a"})],
                     Runner(quiet=True))
        with self.assertRaises(PipelineError):
            run_jobs([Job("a", lambda r: None, deps={"zzz"})], Runner(quiet=True))


class Discovery(unittest.TestCase):
    """ffmpeg/ffprobe: flag, then variable, then the ffmpeg-full keg, then PATH."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.dirs = {}
        for name in ("flag", "env", "keg", "path", "lonely"):
            d = root / name
            d.mkdir()
            self.dirs[name] = d
            for exe in ("ffmpeg", "ffprobe"):
                if name == "lonely" and exe == "ffprobe":
                    continue
                f = d / exe
                f.write_text("#!/bin/sh\n")
                f.chmod(0o755)
        self.which = lambda n: shutil.which(n, path=str(self.dirs["path"]))

    def tearDown(self):
        self.tmp.cleanup()

    def pick(self, ffmpeg=None, ffprobe=None, env=None, keg=True):
        keg_dir = self.dirs["keg"] if keg else Path(self.tmp.name) / "no-keg"
        chosen = runner_mod.discover_ffmpeg(ffmpeg, ffprobe, env=env or {}, keg=keg_dir, which=self.which)
        return {k: (Path(v.path).parent.name if v.path else None, v.source) for k, v in chosen.items()}

    def test_order(self):
        flag = str(self.dirs["flag"] / "ffmpeg")
        env = {"MYNES_FFMPEG": str(self.dirs["env"] / "ffmpeg"), "MYNES_FFPROBE": str(self.dirs["env"] / "ffprobe")}
        self.assertEqual(self.pick(flag, str(self.dirs["flag"] / "ffprobe"), env),
                         {"ffmpeg": ("flag", "--ffmpeg"), "ffprobe": ("flag", "--ffprobe")})
        self.assertEqual(self.pick(env=env), {"ffmpeg": ("env", "MYNES_FFMPEG"), "ffprobe": ("env", "MYNES_FFPROBE")})
        self.assertEqual(self.pick(), {"ffmpeg": ("keg", "ffmpeg-full"), "ffprobe": ("keg", "ffmpeg-full")})
        self.assertEqual(self.pick(keg=False), {"ffmpeg": ("path", "PATH"), "ffprobe": ("path", "PATH")})

    def test_ffprobe_follows_an_explicit_ffmpeg(self):
        self.assertEqual(self.pick(str(self.dirs["flag"] / "ffmpeg")),
                         {"ffmpeg": ("flag", "--ffmpeg"), "ffprobe": ("flag", "beside --ffmpeg")})
        env = {"MYNES_FFMPEG": str(self.dirs["lonely"] / "ffmpeg")}
        self.assertEqual(self.pick(env=env), {"ffmpeg": ("lonely", "MYNES_FFMPEG"), "ffprobe": ("keg", "ffmpeg-full")})

    def test_bad_explicit_path_and_missing(self):
        with self.assertRaises(PipelineError):
            self.pick(str(self.dirs["flag"] / "nope"))
        chosen = runner_mod.discover_ffmpeg(env={}, keg=Path(self.tmp.name) / "no-keg", which=lambda n: None)
        self.assertIsNone(chosen["ffmpeg"].path)
        self.assertEqual(chosen["ffmpeg"].describe(), "ffmpeg: MISSING")

    def test_runner_runs_the_configured_tool(self):
        try:
            runner_mod.configure_tools(str(self.dirs["flag"] / "ffmpeg"), env={})
            r = Runner(dry_run=True, quiet=True)
            r.run(["ffmpeg", "-version"])
            self.assertEqual(r.ran[0][0], str(self.dirs["flag"] / "ffmpeg"))
            self.assertEqual(runner_mod.env_for_capture({})["MYNES_FFMPEG"], str(self.dirs["flag"] / "ffmpeg"))
        finally:
            runner_mod.configure_tools()


class AnimationTiming(unittest.TestCase):
    def test_even_steps(self):
        self.assertIsNone(runner_mod.timing_problem([125] * 8, 8, 8))
        self.assertIsNone(runner_mod.timing_problem([250, 125, 375, 250], 8, 8))  # merged identical frames
        self.assertIsNone(runner_mod.timing_problem([33, 34, 33] * 10, 30, 30))    # 1000/30 in whole ms

    def test_uneven_steps(self):
        # libwebp_anim timed in 1/60.0988 s units: 7.51 ticks per 125 ms.
        problem = runner_mod.timing_problem([133, 116, 133, 117, 133, 116, 133, 125], 8, 8)
        self.assertIn("frame 1 starts at 133 ms", problem)
        self.assertIn("lasts 1125 ms", runner_mod.timing_problem([125] * 9, 8, 8))
        self.assertIn("duration 0", runner_mod.timing_problem([125, 0], 2, 8))


class Tools(unittest.TestCase):
    def test_versions(self):
        self.assertEqual(runner_mod.ffmpeg_version_tuple("ffmpeg version 6.1.1-3ubuntu5 Copyright"), (6, 1))
        self.assertEqual(runner_mod.ffmpeg_version_tuple("ffmpeg version n7.0.2"), (7, 0))
        self.assertIsNone(runner_mod.ffmpeg_version_tuple(None))

    def test_env_for_capture(self):
        shell = {"MYNES_REVIEW_INPUT_SCRIPT": "x", "PATH": "/bin",
                 "MYNES_RECORD_CODEC_ARGS": "-c:v h264_videotoolbox -b:v 90M -pix_fmt yuv420p",
                 "MYNES_RECORD_HDR_CODEC_ARGS": "-c:v libx265", "MYNES_OFFSCREEN_HEADROOM": "1.6"}
        env = runner_mod.env_for_capture(shell, Path("/cfg"))
        for key in ("MYNES_REVIEW_INPUT_SCRIPT", "MYNES_RECORD_CODEC_ARGS", "MYNES_RECORD_HDR_CODEC_ARGS",
                    "MYNES_OFFSCREEN_HEADROOM"):
            self.assertNotIn(key, env)
        self.assertEqual(env["XDG_CONFIG_HOME"], "/cfg")
        self.assertEqual(env["MYNES_REVIEW_NO_INPUT"], "1")
        self.assertEqual(env["PATH"], "/bin")


if __name__ == "__main__":
    unittest.main()
