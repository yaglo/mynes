import os
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


class Tools(unittest.TestCase):
    def test_versions(self):
        self.assertEqual(runner_mod.ffmpeg_version_tuple("ffmpeg version 6.1.1-3ubuntu5 Copyright"), (6, 1))
        self.assertEqual(runner_mod.ffmpeg_version_tuple("ffmpeg version n7.0.2"), (7, 0))
        self.assertIsNone(runner_mod.ffmpeg_version_tuple(None))

    def test_env_for_capture(self):
        env = runner_mod.env_for_capture({"MYNES_REVIEW_INPUT_SCRIPT": "x", "PATH": "/bin"}, Path("/cfg"))
        self.assertNotIn("MYNES_REVIEW_INPUT_SCRIPT", env)
        self.assertEqual(env["XDG_CONFIG_HOME"], "/cfg")
        self.assertEqual(env["MYNES_REVIEW_NO_INPUT"], "1")
        self.assertEqual(env["PATH"], "/bin")


if __name__ == "__main__":
    unittest.main()
