"""When the poster, stills and crops, README animation and flicker clip are
built again: their files are newer than the render either way, so each keeps
a <name>.encode.json record of the shots.json and command-line parameters it
was made with, and a change to one of those builds it again. Runs as a dry
run against files that stand in for the outputs, so it needs no ffmpeg."""
import dataclasses
import json
import os
import tempfile
import time
import unittest
from pathlib import Path

from pipeline import jobs as jobs_mod
from pipeline import shots
from pipeline.runner import Runner

LENS, STAGE, README = (512, 384), (256, 192), (320, 240)


class ParameterRecords(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        presets = root / "presets"
        presets.mkdir()
        (presets / "p_a.json").write_text(json.dumps({"name": "Preset A"}))
        data = {"defaults": {"presets": ["p_a"], "flicker_crop": [78, 73, 100, 93.75],
                             "sizes": {"lens": "512x384", "stage": ["256x192"], "readme": "320x240"}},
                "shots": [{"id": "synth", "title": "Synthetic", "scene": "s", "rom": "*synth*", "seconds": 2,
                           "readme": ["p_a"], "readme_seconds": 1, "thumbnail_frame": 3,
                           "detail_crop": [10, 20, 60, 40]}]}
        (root / "shots.json").write_text(json.dumps(data))
        self.shot = shots.load(root / "shots.json", presets).shots[0]
        self.ctx = jobs_mod.Context(shot_list=shots.load(root / "shots.json", presets), out=root / "out",
                                    presets_dir=presets, states_dir=root / "states")

    def tearDown(self):
        self.tmp.cleanup()

    # A dry run without renders assumes the plan's numbers, so the records
    # computed here are the ones a real build of the same files writes.
    def facts(self, size, hdr, ctx=None, shot=None):
        return jobs_mod.render_facts(ctx or self.ctx, Runner(dry_run=True, quiet=True), shot or self.shot,
                                     "p_a", size, hdr)

    def make(self, d: Path, names, record: str, params: dict):
        """Stand-in outputs and the record of a finished build of them."""
        d.mkdir(parents=True, exist_ok=True)
        for name in names:
            (d / name).write_bytes(b"x")
        (d / record).write_text(json.dumps({"params": params}))
        old = time.time() - 100
        for path in d.iterdir():
            os.utime(path, (old, old))

    def rebuilt(self, job, ctx=None, shot=None) -> list[list[str]]:
        runner = Runner(dry_run=True, quiet=True)
        job(ctx or self.ctx, runner, shot or self.shot, "p_a")
        return runner.ran

    def replaced(self, **changes):
        return dataclasses.replace(self.shot, **changes)

    # -- poster ---------------------------------------------------------------
    def poster(self, ctx, runner, shot, preset):
        return jobs_mod.encode_poster(ctx, runner, shot, preset, STAGE)

    def test_poster(self):
        d = self.ctx.size_dir(self.shot, "p_a", STAGE)
        self.make(d, ["poster.webp"], "poster.encode.json",
                  jobs_mod.poster_params(self.shot, self.facts(STAGE, False)))
        self.assertEqual(self.rebuilt(self.poster), [])
        ran = self.rebuilt(self.poster, shot=self.replaced(thumbnail_frame=7))
        self.assertEqual(len(ran), 1)
        self.assertIn("eq(n\\,7)", " ".join(ran[0]))

    def test_output_without_a_record_is_built_again(self):
        d = self.ctx.size_dir(self.shot, "p_a", STAGE)
        self.make(d, ["poster.webp"], "poster.encode.json", {})
        (d / "poster.encode.json").unlink()
        self.assertNotEqual(self.rebuilt(self.poster), [])

    # -- stills and detail crops -------------------------------------------------
    def still_record(self, ctx=None, shot=None):
        ctx, shot = ctx or self.ctx, shot or self.shot
        return jobs_mod.still_params(ctx, shot, "p_a", self.facts(LENS, False, ctx, shot),
                                     self.facts(LENS, True, ctx, shot))

    def test_stills_and_crops(self):
        d = self.ctx.size_dir(self.shot, "p_a", LENS)
        self.make(d, jobs_mod.STILL_FILES + jobs_mod.CROP_FILES, "still.encode.json", self.still_record())
        self.assertEqual(self.rebuilt(jobs_mod.encode_still), [])
        for shot in (self.replaced(thumbnail_frame=9), self.replaced(detail_crop=[12, 20, 60, 40])):
            self.assertNotEqual(self.rebuilt(jobs_mod.encode_still, shot=shot), [])
        scaled = dataclasses.replace(self.ctx, flicker_scale="3")
        self.assertNotEqual(self.rebuilt(jobs_mod.encode_still, ctx=scaled), [])

    def test_the_manifest_crop_is_the_recorded_crop(self):
        record = self.still_record()
        entry = jobs_mod.crop_entry(self.ctx, self.shot, "p_a", {n: n for n in jobs_mod.SITE_CROP_FILES})
        self.assertEqual(record["crop"], [entry["x"], entry["y"], entry["width"], entry["height"]])

    # -- README animation -------------------------------------------------------
    README_FILES = ["readme.webp"]

    def readme_record(self, shot=None):
        shot = shot or self.shot
        return jobs_mod.readme_params(shot, self.facts(README, False, shot=shot))

    def test_readme(self):
        d = self.ctx.size_dir(self.shot, "p_a", README)
        self.make(d, self.README_FILES, "readme.encode.json", self.readme_record())
        self.assertEqual(self.rebuilt(jobs_mod.encode_readme), [])
        # The animation starts at frame 0; only its length comes from shots.json.
        self.assertEqual(self.rebuilt(jobs_mod.encode_readme, shot=self.replaced(thumbnail_frame=9)), [])
        self.assertNotEqual(self.rebuilt(jobs_mod.encode_readme, shot=self.replaced(readme_seconds=1.5)), [])

    # -- flicker clip -----------------------------------------------------------
    FLICKER_FILES = ["flicker.webp"]

    def flicker_record(self, ctx=None, shot=None):
        ctx, shot = ctx or self.ctx, shot or self.shot
        return jobs_mod.flicker_params(ctx, shot, "p_a", self.facts(LENS, False, ctx, shot))

    def test_flicker(self):
        d = self.ctx.size_dir(self.shot, "p_a", LENS)
        self.make(d, self.FLICKER_FILES, "flicker.encode.json", self.flicker_record())
        self.assertEqual(self.rebuilt(jobs_mod.encode_flicker), [])
        for shot in (self.replaced(flicker_frame=11), self.replaced(flicker_crop=[80, 73, 100, 93.75])):
            self.assertNotEqual(self.rebuilt(jobs_mod.encode_flicker, shot=shot), [])
        scaled = dataclasses.replace(self.ctx, flicker_scale="3")
        self.assertNotEqual(self.rebuilt(jobs_mod.encode_flicker, ctx=scaled), [])

    def test_the_detail_crop_does_not_move_the_flicker_clip(self):
        d = self.ctx.size_dir(self.shot, "p_a", LENS)
        self.make(d, self.FLICKER_FILES, "flicker.encode.json", self.flicker_record())
        self.assertEqual(self.rebuilt(jobs_mod.encode_flicker, shot=self.replaced(detail_crop=[0, 0, 60, 40])), [])


if __name__ == "__main__":
    unittest.main()
