import copy
import json
import tempfile
import unittest
from pathlib import Path

from pipeline import manifest, shots


def make_shot_list(tmp: Path):
    presets = tmp / "presets"
    presets.mkdir(exist_ok=True)
    for p in ("sony_pvm_14l2", "stass_favourite", "vhs_sp_consumer"):
        (presets / f"{p}.json").write_text(json.dumps({"name": f"Name of {p}"}))
    data = {
        "defaults": {"presets": ["sony_pvm_14l2", "stass_favourite", "vhs_sp_consumer"]},
        "presets": {"sony_pvm_14l2": {"name": "Sony PVM-14L2", "blurb": "Focused"},
                    "stass_favourite": {"blurb": "Worn RF"}},
        "shots": [
            {"id": "mario", "title": "Super Mario Bros.", "scene": "1-1", "rom": "*mario*"},
            {"id": "zelda", "title": "The Legend of Zelda", "scene": "start", "rom": "*zelda*",
             "default_preset": "stass_favourite"},
        ],
        "features": [{"id": "push-in", "type": "push-in", "shot": "mario", "seconds": 5}],
    }
    path = tmp / "shots.json"
    path.write_text(json.dumps(data))
    return shots.load(path, presets), presets


EXISTING = {
    "fps": 60.0988, "aspect": [4, 3],
    "presets": [
        {"id": "jvc_d_series_2000", "name": "JVC D-Series", "blurb": "Cooler whites"},
        {"id": "sony_pvm_14l2", "name": "Old name", "blurb": "Old blurb"},
        {"id": "stass_favourite", "name": "Stas's Favourite", "blurb": "Old worn"},
    ],
    "games": [
        {"id": "kirby-title", "title": "Kirby's Adventure", "scene": "Title", "default_preset": "jvc_d_series_2000"},
        {"id": "mario", "title": "Mario old title", "scene": "old scene", "default_preset": "jvc_d_series_2000"},
    ],
    "clips": {
        "kirby-title": {"jvc_d_series_2000": {"video": "assets/images/showcase/kirby.mp4",
                                              "poster": "assets/hero/kirby-title/jvc.poster.webp"}},
        "mario": {"jvc_d_series_2000": {"poster": "assets/hero/mario/jvc.poster.webp"},
                  "sony_pvm_14l2": {"video": "assets/images/showcase/old.mp4", "full": "assets/images/old.png"}},
    },
}


class Build(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.shot_list, self.presets_dir = make_shot_list(Path(self.tmp.name))

    def tearDown(self):
        self.tmp.cleanup()

    def produced(self):
        return {"mario": {"sony_pvm_14l2": dict(manifest.install_paths("mario", "sony_pvm_14l2"),
                                                still_size=[3840, 2880])}}

    def test_from_scratch(self):
        m = manifest.build(self.shot_list, self.produced(), presets_dir=self.presets_dir)
        self.assertEqual(m["fps"], 60.0988)
        self.assertEqual(m["aspect"], [4, 3])
        self.assertEqual([p["id"] for p in m["presets"]], ["sony_pvm_14l2", "stass_favourite", "vhs_sp_consumer"])
        by_id = {p["id"]: p for p in m["presets"]}
        self.assertEqual(by_id["sony_pvm_14l2"], {"id": "sony_pvm_14l2", "name": "Sony PVM-14L2", "blurb": "Focused"})
        self.assertEqual(by_id["stass_favourite"]["name"], "Name of stass_favourite")  # from the preset file
        self.assertEqual(by_id["stass_favourite"]["blurb"], "Worn RF")
        self.assertEqual(by_id["vhs_sp_consumer"]["blurb"], "")
        self.assertEqual(m["games"], [{"id": "mario", "title": "Super Mario Bros.", "scene": "1-1",
                                       "default_preset": "sony_pvm_14l2"}])  # only produced games
        clip = m["clips"]["mario"]["sony_pvm_14l2"]
        self.assertEqual(clip["video"], "assets/hero/mario/sony_pvm_14l2.mp4")
        self.assertEqual(clip["poster"], "assets/hero/mario/sony_pvm_14l2.poster.webp")
        self.assertEqual(clip["still"], "assets/hero/mario/sony_pvm_14l2.4k.webp")
        self.assertEqual(clip["full"], "assets/hero/mario/sony_pvm_14l2.4k.png")
        self.assertEqual(clip["still_size"], [3840, 2880])
        self.assertEqual(manifest.validate(m), [])
        json.loads(manifest.dump(m))

    def test_merge_keeps_everything_not_produced(self):
        existing = copy.deepcopy(EXISTING)
        m = manifest.build(self.shot_list, self.produced(), existing=existing, presets_dir=self.presets_dir,
                           features={"push-in": {"video": "assets/hero/features/push-in.mp4", "caption": "c"}})
        self.assertEqual(existing, EXISTING)  # input untouched
        ids = [p["id"] for p in m["presets"]]
        self.assertEqual(ids, ["jvc_d_series_2000", "sony_pvm_14l2", "stass_favourite", "vhs_sp_consumer"])
        by_id = {p["id"]: p for p in m["presets"]}
        self.assertEqual(by_id["jvc_d_series_2000"]["blurb"], "Cooler whites")
        self.assertEqual(by_id["sony_pvm_14l2"]["name"], "Sony PVM-14L2")  # shots.json overrides
        self.assertEqual(by_id["stass_favourite"]["name"], "Stas's Favourite")  # kept: no override
        self.assertEqual(by_id["stass_favourite"]["blurb"], "Worn RF")  # overridden blurb
        self.assertEqual([g["id"] for g in m["games"]], ["kirby-title", "mario"])
        mario = next(g for g in m["games"] if g["id"] == "mario")
        self.assertEqual(mario["title"], "Super Mario Bros.")
        self.assertEqual(mario["default_preset"], "sony_pvm_14l2")
        self.assertEqual(m["clips"]["kirby-title"], EXISTING["clips"]["kirby-title"])
        self.assertEqual(m["clips"]["mario"]["jvc_d_series_2000"], {"poster": "assets/hero/mario/jvc.poster.webp"})
        sony = m["clips"]["mario"]["sony_pvm_14l2"]
        self.assertEqual(sony["video"], "assets/hero/mario/sony_pvm_14l2.mp4")
        self.assertEqual(sony["full"], "assets/hero/mario/sony_pvm_14l2.4k.png")
        self.assertEqual(m["features"]["push-in"]["caption"], "c")
        self.assertEqual(manifest.validate(m), [])

    def test_partial_produce_keeps_other_keys(self):
        existing = copy.deepcopy(EXISTING)
        m = manifest.build(self.shot_list, {"mario": {"sony_pvm_14l2": {"poster": "assets/hero/mario/new.webp"}}},
                           existing=existing, presets_dir=self.presets_dir)
        sony = m["clips"]["mario"]["sony_pvm_14l2"]
        self.assertEqual(sony["poster"], "assets/hero/mario/new.webp")
        self.assertEqual(sony["video"], "assets/images/showcase/old.mp4")

    def test_validate_flags_inconsistency(self):
        m = manifest.build(self.shot_list, self.produced(), presets_dir=self.presets_dir)
        m["games"][0]["default_preset"] = "nope"
        m["clips"]["ghost"] = {"sony_pvm_14l2": {}}
        problems = manifest.validate(m)
        self.assertEqual(len(problems), 2)

    def test_load_and_paths(self):
        p = Path(self.tmp.name) / "manifest.json"
        self.assertIsNone(manifest.load(p))
        p.write_text(json.dumps(EXISTING))
        self.assertEqual(manifest.load(p), EXISTING)
        self.assertEqual(manifest.feature_paths("x"), {"video": "assets/hero/features/x.mp4",
                                                       "poster": "assets/hero/features/x.poster.webp"})


if __name__ == "__main__":
    unittest.main()
