"""The record stage against fake_recorder.py, a stand-in for the GPU
frontend's recorder: which passes run, with what arguments and environment,
what is checked afterwards, and when a pass is recorded again."""
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from pipeline import jobs as jobs_mod
from pipeline import shots
from pipeline.runner import PipelineError, Runner, have_tool, probe_video, run_jobs

HERE = Path(__file__).resolve().parent
HAVE_FFMPEG = have_tool("ffmpeg") and have_tool("ffprobe")
LENS, STAGE, README = (192, 144), (128, 96), (160, 120)


def write_recorder(path: Path) -> Path:
    path.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{HERE / "fake_recorder.py"}" "$@"\n')
    path.chmod(0o755)
    return path


def write_fixture(root: Path, **defaults) -> dict:
    """A shot list with one shot on one preset, its ROM, state, replay and
    preset file, and the fake recorder, under ``root``."""
    for d in ("presets", "roms", "states", "replays"):
        (root / d).mkdir(parents=True, exist_ok=True)
    (root / "presets" / "p_a.json").write_text(json.dumps({"name": "Preset A"}))
    (root / "roms" / "Synth Beta (U).nes").write_bytes(b"NES\x1a" + bytes(16384 + 8192 + 12))
    (root / "states" / "beta.s1").write_bytes(b"state")
    (root / "replays" / "beta.replay").write_text("1 80\n")
    data = {"defaults": {"presets": ["p_a"], "sizes": {"lens": "192x144", "stage": ["128x96"], "readme": "160x120"},
                         **defaults},
            "shots": [{"id": "beta", "title": "Beta", "scene": "s", "rom": "*beta*", "state": "beta.s1",
                       "replay": "beta.replay", "seconds": 1, "readme": ["p_a"]}]}
    (root / "shots.json").write_text(json.dumps(data))
    write_recorder(root / "mynes_gpu")
    return data


@unittest.skipUnless(HAVE_FFMPEG, "needs ffmpeg and ffprobe")
class Record(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.data = write_fixture(self.root)
        self.log = self.root / "recorder.log"
        patch = mock.patch.dict(os.environ, {"FAKE_RECORDER_LOG": str(self.log)})
        patch.start()
        self.addCleanup(patch.stop)

    def tearDown(self):
        self.tmp.cleanup()

    def ctx(self) -> jobs_mod.Context:
        r = self.root
        shot_list = shots.load(r / "shots.json", r / "presets")
        return jobs_mod.Context(shot_list=shot_list, out=r / "out", binary=r / "mynes_gpu", roms=r / "roms",
                                presets_dir=r / "presets", states_dir=r / "states", replays_dir=r / "replays")

    def record(self, **runner_opts):
        ctx = self.ctx()
        runner = Runner(quiet=True, **runner_opts)
        return run_jobs(jobs_mod.record_jobs(ctx, ctx.shot_list.select()), runner), ctx

    def calls(self) -> list[dict]:
        if not self.log.exists():
            return []
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def render(self, size, hdr) -> Path:
        ctx = self.ctx()
        return ctx.render_path(ctx.shot_list.shots[0], "p_a", size, hdr)

    def set_defaults(self, **values):
        self.data["defaults"].update(values)
        (self.root / "shots.json").write_text(json.dumps(self.data))

    def test_every_pass_records_once(self):
        result, _ = self.record()
        self.assertEqual(result.failed, {})
        self.assertEqual(len(result.ok), 6)  # three sizes, SDR and HDR
        self.assertEqual(len(self.calls()), 6)
        for size in (STAGE, LENS, README):
            for hdr in (False, True):
                provenance = self.render(size, hdr).with_name(("hdr" if hdr else "sdr") + ".record.json")
                self.assertTrue(provenance.exists(), provenance)
        result, _ = self.record()
        self.assertEqual(result.failed, {})
        self.assertEqual(len(self.calls()), 6)  # all up to date

    def test_hdr_passes_stop_after_the_frames_read(self):
        """The stills read frame 0 of the full-size and README-size HDR
        renders; the SDR passes hold the flicker frames and the README loop."""
        self.record()
        frames = {(size, hdr): probe_video(self.render(size, hdr)).frames
                  for size in (STAGE, LENS, README) for hdr in (False, True)}
        self.assertEqual(frames, {(STAGE, False): 60, (STAGE, True): 60, (LENS, False): 8, (LENS, True): 1,
                                  (README, False): 60, (README, True): 1})

    def test_passes_share_state_replay_and_start(self):
        """Every pass of a clip starts from the same state, replay and frame,
        so the stage, lens and still frames are the same frames."""
        self.record()
        calls = self.calls()
        for flag in ("--load-state", "--input-replay", "--record-after", "--preset"):
            values = {c["argv"][c["argv"].index(flag) + 1] for c in calls}
            self.assertEqual(len(values), 1, (flag, values))
        self.assertEqual({c["argv"][-1] for c in calls}, {str(self.root / "roms" / "Synth Beta (U).nes")})

    def test_shell_settings_do_not_reach_the_recorder(self):
        shell = {"MYNES_RECORD_CODEC_ARGS": "-c:v libx264 -preset ultrafast -pix_fmt yuv420p",
                 "MYNES_RECORD_HDR_CODEC_ARGS": "-c:v libx265 -pix_fmt yuv420p10le",
                 "MYNES_OFFSCREEN_HEADROOM": "1.6", "MYNES_REVIEW_OSD": "1"}
        with mock.patch.dict(os.environ, shell):
            result, _ = self.record()
        self.assertEqual(result.failed, {})
        for call in self.calls():
            self.assertFalse(set(shell) & set(call["env"]), call["env"])
            self.assertEqual(call["env"]["MYNES_REVIEW_NO_INPUT"], "1")
        self.assertEqual(probe_video(self.render(STAGE, False)).pix_fmt, "yuv444p")
        self.assertIn(probe_video(self.render(STAGE, True)).pix_fmt, ("yuv444p10le", "yuv444p12le"))

    def test_a_420_render_is_refused(self):
        with mock.patch.dict(os.environ, {"FAKE_RECORDER_CODEC_ARGS": "-c:v libx264 -preset ultrafast -pix_fmt yuv420p"}):
            result, ctx = self.record()
        self.assertEqual(len(result.failed), 6)
        self.assertTrue(all("pixel format yuv420p, expected yuv444p" in e or "expected yuv444p10le" in e
                            for e in result.failed.values()), result.failed)
        self.assertFalse(self.render(STAGE, False).with_name("sdr.record.json").exists())
        with self.assertRaises(PipelineError) as cm:  # encode refuses the render too
            jobs_mod.render_facts(ctx, Runner(quiet=True), ctx.shot_list.shots[0], "p_a", STAGE, False)
        self.assertIn("4:4:4", str(cm.exception))

    def test_a_failed_run_is_recorded_again(self):
        self.record()
        with mock.patch.dict(os.environ, {"FAKE_RECORDER_FRAMES": "20", "FAKE_RECORDER_EXIT": "4"}):
            result, _ = self.record(force=True)
        self.assertEqual(len(result.failed), 6)
        self.assertFalse(self.render(STAGE, False).with_name("sdr.record.json").exists())
        self.assertFalse(self.render(STAGE, True).with_name("hdr.record.json").exists())
        before = len(self.calls())
        result, _ = self.record()  # no --force: the failed passes run again
        self.assertEqual(result.failed, {})
        self.assertEqual(len(self.calls()), before + 6)
        self.assertEqual(probe_video(self.render(STAGE, False)).frames, 60)

    def test_changed_settings_record_again(self):
        self.record()
        self.set_defaults(hdr={"headroom": 2.0, "white_nits": 100})
        self.record()
        calls = self.calls()[6:]
        self.assertEqual(len(calls), 3)  # the HDR passes only
        self.assertTrue(all("--record-hdr" in c["argv"] for c in calls))
        sidecar = json.loads(self.render(STAGE, True).with_suffix(".json").read_text())
        self.assertEqual((sidecar["headroom"], sidecar["white_nits"]), (2.0, 100))
        self.set_defaults(record_after=5)
        self.record()
        self.assertEqual(len(self.calls()), 6 + 3 + 6)  # a later start changes every pass
        self.set_defaults(record_args=["--room-reflections"])
        self.record()
        self.assertEqual(len(self.calls()), 6 + 3 + 6 + 6)


if __name__ == "__main__":
    unittest.main()
