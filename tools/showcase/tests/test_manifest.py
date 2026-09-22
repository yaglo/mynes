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
            {"id": "mario", "title": "Super Mario Bros.", "scene": "1-1", "rom": "*mario*", "lens": True},
            {"id": "zelda", "title": "The Legend of Zelda", "scene": "start", "rom": "*zelda*",
             "default_preset": "stass_favourite"},
        ],
    }
    path = tmp / "shots.json"
    path.write_text(json.dumps(data))
    return shots.load(path, presets), presets


def source(rel, codecs, hdr, size, nbytes=1000):
    return {"src": rel, "type": f'video/mp4; codecs="{codecs}"', "hdr": hdr, "width": size[0],
            "height": size[1], "bytes": nbytes}


def clip(shot="mario", preset="sony_pvm_14l2", lens=True):
    path = lambda size, name: manifest.clip_path(shot, preset, size, name)  # noqa: E731
    entry = {
        "poster": [{"src": path((1920, 1440), "poster.webp"), "width": 1920, "height": 1440},
                   {"src": path((960, 720), "poster.webp"), "width": 960, "height": 720}],
        "stage": [source(path(size, name), codecs, hdr, size)
                  for size in ((1920, 1440), (960, 720))
                  for name, codecs, hdr in (("stage-hdr-hevc.mp4", "hvc1.2.4.L153.B0", True),
                                            ("stage-hdr-av1.mp4", "av01.0.12M.10.0.110.09.16.09.0", True),
                                            ("stage-sdr.mp4", "avc1.640033", False))],
        "still": {"hdr": path((3840, 2880), "still-hdr.avif"), "sdr": path((3840, 2880), "still-sdr.png"),
                  "width": 3840, "height": 2880, "frame": 0},
        "hdr": {"white_nits": 203, "headroom": 4.0, "max_cll": 812, "max_fall": 50},
    }
    if lens:
        entry["lens"] = [source(path((3840, 2880), "lens-hdr-hevc.mp4"), "hvc1.2.4.L183.B0", True, (3840, 2880))]
    return entry


V1 = {
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
                  "sony_pvm_14l2": {"video": "assets/images/showcase/old.mp4", "full": "assets/images/old.png",
                                    "still": "assets/hero/mario/sony.4k.webp", "still_size": [3840, 2880],
                                    "note": "kept"}},
    },
    "features": {"push-in": {"video": "assets/hero/features/push-in.mp4"}},
}


class Build(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.shot_list, self.presets_dir = make_shot_list(Path(self.tmp.name))

    def tearDown(self):
        self.tmp.cleanup()

    def test_paths(self):
        self.assertEqual(manifest.clip_dir("mario", "sony"), "assets/hero/mario/sony")
        self.assertEqual(manifest.clip_path("mario", "sony", (1920, 1440), "stage-sdr.mp4"),
                         "assets/hero/mario/sony/1920x1440/stage-sdr.mp4")

    def test_from_scratch(self):
        m = manifest.build(self.shot_list, {"mario": {"sony_pvm_14l2": clip()}}, presets_dir=self.presets_dir)
        self.assertEqual(list(m)[:6], ["version", "fps", "aspect", "presets", "games", "clips"])
        self.assertEqual((m["version"], m["fps"], m["aspect"]), (2, 60.0988, [4, 3]))
        self.assertEqual([p["id"] for p in m["presets"]], ["sony_pvm_14l2", "stass_favourite", "vhs_sp_consumer"])
        by_id = {p["id"]: p for p in m["presets"]}
        self.assertEqual(by_id["sony_pvm_14l2"], {"id": "sony_pvm_14l2", "name": "Sony PVM-14L2", "blurb": "Focused"})
        self.assertEqual(by_id["stass_favourite"]["name"], "Name of stass_favourite")  # from the preset file
        self.assertEqual(m["games"], [{"id": "mario", "title": "Super Mario Bros.", "scene": "1-1",
                                       "default_preset": "sony_pvm_14l2"}])  # only produced games
        self.assertEqual(m["clips"]["mario"]["sony_pvm_14l2"], clip())
        self.assertEqual(manifest.validate(m), [])
        self.assertEqual(json.loads(manifest.dump(m)), m)

    def test_merge_keeps_everything_not_produced(self):
        existing = copy.deepcopy(V1)
        m = manifest.build(self.shot_list, {"mario": {"sony_pvm_14l2": clip()}}, existing=existing,
                           presets_dir=self.presets_dir)
        self.assertEqual(existing, V1)  # input untouched
        self.assertEqual(m["version"], 2)
        self.assertEqual([p["id"] for p in m["presets"]],
                         ["jvc_d_series_2000", "sony_pvm_14l2", "stass_favourite", "vhs_sp_consumer"])
        by_id = {p["id"]: p for p in m["presets"]}
        self.assertEqual(by_id["jvc_d_series_2000"]["blurb"], "Cooler whites")
        self.assertEqual(by_id["sony_pvm_14l2"]["name"], "Sony PVM-14L2")  # shots.json overrides
        self.assertEqual(by_id["stass_favourite"]["name"], "Stas's Favourite")  # kept: no override
        self.assertEqual([g["id"] for g in m["games"]], ["kirby-title", "mario"])
        self.assertEqual(next(g for g in m["games"] if g["id"] == "mario")["title"], "Super Mario Bros.")
        self.assertEqual(m["clips"]["kirby-title"], V1["clips"]["kirby-title"])
        self.assertEqual(m["clips"]["mario"]["jvc_d_series_2000"], V1["clips"]["mario"]["jvc_d_series_2000"])
        sony = m["clips"]["mario"]["sony_pvm_14l2"]
        for key in ("video", "full", "still_size"):  # v1 keys of a produced clip give way to v2
            self.assertNotIn(key, sony)
        self.assertEqual(sony["still"], clip()["still"])
        self.assertEqual(sony["note"], "kept")
        self.assertEqual(m["features"], V1["features"])  # unknown top-level keys survive
        self.assertEqual(manifest.validate(m), [])

    def test_sources_merge_by_src(self):
        first = manifest.build(self.shot_list, {"mario": {"sony_pvm_14l2": clip()}}, presets_dir=self.presets_dir)
        first["clips"]["mario"]["sony_pvm_14l2"]["stage"].append(
            source("assets/hero/mario/sony_pvm_14l2/3200x2400/stage-sdr.mp4", "avc1.640034", False, (3200, 2400)))
        again = clip(lens=False)
        again["stage"][0]["bytes"] = 5
        m = manifest.build(self.shot_list, {"mario": {"sony_pvm_14l2": again}}, existing=first,
                           presets_dir=self.presets_dir)
        stage = m["clips"]["mario"]["sony_pvm_14l2"]["stage"]
        self.assertEqual(len(stage), 7)
        self.assertEqual(stage[0]["bytes"], 5)                       # replaced in place
        self.assertEqual(stage[-1]["width"], 3200)                    # not produced this time: kept
        self.assertEqual(m["clips"]["mario"]["sony_pvm_14l2"]["lens"], clip()["lens"])  # lens not produced: kept

    def test_validate_flags_inconsistency(self):
        m = manifest.build(self.shot_list, {"mario": {"sony_pvm_14l2": clip()}}, presets_dir=self.presets_dir)
        m["games"][0]["default_preset"] = "nope"
        m["clips"]["ghost"] = {"sony_pvm_14l2": {}}
        c = m["clips"]["mario"]["sony_pvm_14l2"]
        del c["stage"][0]["bytes"]
        c["stage"][1]["type"] = "video/mp4"
        del c["still"]["frame"]
        del c["hdr"]["max_fall"]
        c["poster"][1] = {"src": "x.webp"}
        problems = manifest.validate(m)
        self.assertEqual(len(problems), 7, problems)

    def test_load(self):
        p = Path(self.tmp.name) / "manifest.json"
        self.assertIsNone(manifest.load(p))
        p.write_text(json.dumps(V1))
        self.assertEqual(manifest.load(p), V1)
        p.write_text("[]")
        with self.assertRaises(ValueError):
            manifest.load(p)


if __name__ == "__main__":
    unittest.main()
