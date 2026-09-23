import json
import os
import tempfile
import unittest
import zlib
from pathlib import Path

from pipeline import recipes, shots
from pipeline.shots import ShotListError


def ines(prg=1, chr_=1, seed=0, trainer=False, pal=False):
    flags6 = 0x04 if trainer else 0
    header = b"NES\x1a" + bytes([prg, chr_, flags6, 0, 0, 1 if pal else 0, 0, 0, 0, 0, 0, 0])
    body = bytes((i * 7 + seed) & 0xFF for i in range(prg * 16384 + chr_ * 8192))
    return header + (b"\xee" * 512 if trainer else b"") + body


class RealShotList(unittest.TestCase):
    def test_loads_and_is_consistent(self):
        sl = shots.load()
        ids = [s.id for s in sl.shots]
        self.assertEqual(len(ids), len(set(ids)))
        for expected in ("super-mario-bros", "legend-of-zelda", "punch-out", "journey-to-silius",
                         "castlevania-3", "blaster-master", "ninja-gaiden", "mega-man-2", "metroid", "batman"):
            self.assertIn(expected, ids)
        self.assertEqual([f.id for f in sl.features], ["five-televisions", "raw-vs-pvm-vs-rf"])
        d = sl.defaults
        self.assertEqual((d.lens_size, d.stage_sizes, d.readme_size),
                         ((3840, 2880), [(1920, 1440), (960, 720)], (1600, 1200)))
        self.assertEqual((d.hdr_headroom, d.hdr_white_nits), (4.0, 203))
        for s in sl.shots:
            self.assertEqual(s.seconds, 6 if s.kind == "hero" else 15)
            self.assertEqual(s.presets, shots.DEFAULT_PRESETS)
            # Every crop is 100 dots by 93.75 lines: 1358x1120 on the full-size render.
            rect = recipes.flicker_geometry(s.flicker_crop, d.lens_size)
            self.assertEqual((rect.w, rect.h), (1358, 1120), s.id)
            self.assertTrue((shots.PRESETS_DIR / f"{s.default_preset}.json").exists())
        # No lens clips while the site is near its size budget: a 6 s lens clip is about 70 MB.
        self.assertEqual([s.id for s in sl.shots if s.lens], [])
        # Punch-Out!! has every other preset file as a crop-only preset.
        punch = sl.shot("punch-out")
        self.assertEqual(sorted(punch.presets + punch.crops), sorted(p.stem for p in shots.PRESETS_DIR.glob("*.json")))
        self.assertEqual([s.id for s in sl.shots if s.crops], ["punch-out"])
        self.assertEqual(sl.shot("metroid").default_preset, "bedroom_rf_1990")
        for s in sl.shots:
            if s.replay:
                rows = shots.parse_replay((shots.REPLAYS_DIR / s.replay).read_text())
                self.assertLessEqual(rows[-1][0], s.frames)
        five = sl.feature("five-televisions")
        self.assertEqual(len(five.presets), 5)
        self.assertEqual(shots.feature_frames(five, sl.shot(five.shot)), 900)
        side = sl.feature("raw-vs-pvm-vs-rf")
        self.assertEqual(shots.feature_frames(side, sl.shot(side.shot)), 901)

    def test_presets_have_metadata(self):
        sl = shots.load()
        for p in sl.all_presets():
            self.assertIn("name", sl.preset_meta.get(p, {}), p)


class Validation(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.presets = self.dir / "presets"
        self.presets.mkdir()
        for p in ("alpha", "beta", "gamma"):
            (self.presets / f"{p}.json").write_text(json.dumps({"name": p.title()}))

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, data):
        path = self.dir / "shots.json"
        path.write_text(json.dumps(data))
        return path

    def base(self, **shot):
        s = {"id": "one", "title": "One", "scene": "start", "rom": "*one*.nes"}
        s.update(shot)
        return {"defaults": {"presets": ["alpha", "beta"]}, "shots": [s]}

    def load(self, data):
        return shots.load(self.write(data), self.presets)

    def test_defaults_apply(self):
        sl = self.load(self.base())
        s = sl.shots[0]
        self.assertEqual(s.seconds, 6)
        self.assertEqual(s.frames, 361)
        self.assertEqual(s.state, "one.s1")
        self.assertIsNone(s.replay)
        self.assertEqual(s.presets, ["alpha", "beta"])
        self.assertEqual(s.default_preset, "alpha")
        self.assertEqual(s.flicker_crop, shots.DEFAULT_FLICKER_CROP)
        self.assertEqual(s.readme_frames, 361)
        self.assertEqual(s.lens, [])
        self.assertEqual(sl.defaults.stage_sizes, [(1920, 1440), (960, 720)])

    def test_lens_and_sizes(self):
        self.assertEqual(self.load(self.base(lens=True)).shots[0].lens, ["alpha", "beta"])
        self.assertEqual(self.load(self.base(lens=["beta"])).shots[0].lens, ["beta"])
        data = self.base()
        data["defaults"].update(sizes={"lens": "512x384", "stage": ["256x192", "128x96"], "readme": "320x240"},
                                hdr={"headroom": 2.5, "white_nits": 100})
        d = self.load(data).defaults
        self.assertEqual((d.lens_size, d.stage_sizes, d.readme_size), ((512, 384), [(256, 192), (128, 96)], (320, 240)))
        self.assertEqual((d.hdr_headroom, d.hdr_white_nits), (2.5, 100))

    def test_detail_crop(self):
        s = self.load(self.base()).shots[0]
        self.assertEqual(s.detail_crop, s.flicker_crop)
        s = self.load(self.base(detail_crop=[64, 4, 128, 120])).shots[0]
        self.assertEqual(s.detail_crop, [64, 4, 128, 120])
        with self.assertRaises(ShotListError):
            self.load(self.base(detail_crop=[200, 200, 100, 100]))

    def test_crops(self):
        sl = self.load(self.base(crops=["gamma"]))
        self.assertEqual(sl.shots[0].crops, ["gamma"])
        self.assertEqual([(s.id, p) for s, p in sl.select()], [("one", "alpha"), ("one", "beta"), ("one", "gamma")])
        self.assertEqual([(s.id, p) for s, p in sl.select(None, ["gamma"])], [("one", "gamma")])
        self.assertEqual(sl.all_presets(), ["alpha", "beta", "gamma"])
        for bad in (["alpha"], "gamma", ["gamma", "gamma"], [1]):
            with self.assertRaises(ShotListError):
                self.load(self.base(crops=bad))

    def test_feature_kind_default_seconds(self):
        self.assertEqual(self.load(self.base(kind="feature")).shots[0].seconds, 15)

    def test_errors(self):
        cases = [
            self.base(id="bad id"),
            self.base(presets=["alpha", "nope"]),
            self.base(flicker_crop=[250, 0, 20, 20]),
            self.base(thumbnail_frame=400),
            self.base(default_preset="gamma"),
            self.base(readme=["gamma"]),
            self.base(kind="loop"),
            self.base(seconds=0),
            self.base(state="../x.s1"),
            self.base(region="secam"),
            self.base(flicker_frame=358),
            self.base(lens=["gamma"]),
            self.base(lens="yes"),
        ]
        for data in cases:
            with self.assertRaises(ShotListError, msg=json.dumps(data)):
                self.load(data)
        dup = self.base()
        dup["shots"].append(dict(dup["shots"][0]))
        with self.assertRaises(ShotListError):
            self.load(dup)
        for defaults in ({"offscreen": "3840x2880"}, {"sizes": {"lens": "3841x2880"}}, {"sizes": {"stage": []}},
                         {"sizes": {"stage": ["960x720", "960x720"]}}, {"sizes": {"thumb": "64x64"}},
                         {"hdr": {"headroom": 0.5}}, {"hdr": {"peak": 1000}}, {"flicker_crop": [0, 0, 300, 10]}):
            data = self.base()
            data["defaults"].update(defaults)
            with self.assertRaises(ShotListError, msg=json.dumps(defaults)):
                self.load(data)

    def test_feature_validation(self):
        data = self.base(kind="feature", presets=["alpha", "beta", "gamma"])
        data["features"] = [{"id": "five", "type": "five-televisions", "shot": "one",
                             "presets": ["alpha", "beta", "gamma"], "seconds_per_preset": 3}]
        sl = self.load(data)
        self.assertEqual(shots.feature_frames(sl.features[0], sl.shots[0]), 540)
        data["features"][0]["seconds_per_preset"] = 6  # 3 x 6 s > 15 s
        with self.assertRaises(ShotListError):
            self.load(data)
        data["features"] = [{"id": "sbs", "type": "side-by-side", "shot": "one", "presets": ["alpha", "beta"]}]
        with self.assertRaises(ShotListError):
            self.load(data)
        data["features"] = [{"id": "push", "type": "push-in", "shot": "one", "preset": "beta"}]  # resampled: gone
        with self.assertRaises(ShotListError):
            self.load(data)
        data["features"] = [{"id": "one", "type": "side-by-side", "shot": "one",  # id clashes with a shot
                             "presets": ["alpha", "beta", "gamma"]}]
        with self.assertRaises(ShotListError):
            self.load(data)
        data["features"] = [{"id": "x", "type": "wipe", "shot": "one"}]
        with self.assertRaises(ShotListError):
            self.load(data)

    def test_select(self):
        data = self.base()
        data["shots"].append({"id": "two", "title": "Two", "scene": "s", "rom": "*two*", "presets": ["gamma"]})
        sl = self.load(data)
        self.assertEqual([(s.id, p) for s, p in sl.select()], [("one", "alpha"), ("one", "beta"), ("two", "gamma")])
        self.assertEqual([(s.id, p) for s, p in sl.select(["two"])], [("two", "gamma")])
        self.assertEqual([(s.id, p) for s, p in sl.select(None, ["beta"])], [("one", "beta")])
        with self.assertRaises(ShotListError):
            sl.select(["three"])
        self.assertEqual(sl.all_presets(), ["alpha", "beta", "gamma"])


class RomDiscovery(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.roms = Path(self.tmp.name)
        (self.roms / "deep" / "er").mkdir(parents=True)
        for name in ("Super Mario Bros. (World).nes", "Super Mario Bros. 3 (USA).nes",
                     "deep/er/PUNCH-OUT!! (USA).NES", "Metroid (USA).nes", "notes.txt"):
            (self.roms / name).write_bytes(ines())

    def tearDown(self):
        self.tmp.cleanup()

    def test_case_insensitive_recursive(self):
        found = shots.find_roms(self.roms, "*punch-out*.nes")
        self.assertEqual([p.name for p in found], ["PUNCH-OUT!! (USA).NES"])

    def test_exclude_and_ambiguity(self):
        self.assertEqual(len(shots.find_roms(self.roms, "*Super Mario Bros*.nes")), 2)
        self.assertEqual(len(shots.find_roms(self.roms, "*Super Mario Bros*.nes", ["*Bros*3*"])), 1)
        shot = shots.Shot(id="smb", title="", scene="", rom="*Super Mario Bros*.nes", state="", seconds=6,
                          kind="hero", presets=["a"], flicker_crop=[0, 0, 8, 8], caption="", thumbnail_frame=0,
                          default_preset="a", readme=[], readme_seconds=6)
        with self.assertRaises(ShotListError) as cm:
            shots.resolve_rom(self.roms, shot)
        self.assertIn("more than one", str(cm.exception))
        shot.rom_exclude = ["*Bros*3*"]
        self.assertEqual(shots.resolve_rom(self.roms, shot).name, "Super Mario Bros. (World).nes")
        shot.rom = "*zelda*"
        with self.assertRaises(ShotListError):
            shots.resolve_rom(self.roms, shot)
        with self.assertRaises(ShotListError):
            shots.find_roms(self.roms / "missing", "*")


class RomInfo(unittest.TestCase):
    def test_crc_over_prg_then_chr(self):
        data = ines(prg=2, chr_=1, seed=3)
        info = shots.rom_info(data)
        self.assertEqual(info.crc, zlib.crc32(data[16:]) & 0xFFFFFFFF)
        self.assertEqual(info.region, "ntsc")
        self.assertEqual((info.prg_size, info.chr_size), (32768, 8192))
        self.assertEqual(len(info.crc_hex), 8)

    def test_trainer_skipped_and_pal_flag(self):
        plain = shots.rom_info(ines(seed=5))
        trained = shots.rom_info(ines(seed=5, trainer=True))
        self.assertEqual(plain.crc, trained.crc)
        self.assertEqual(shots.rom_info(ines(pal=True)).region, "pal")

    def test_junk_header_is_ntsc(self):
        # Bytes 7-15 of a "DiskDude!" dump: byte 9 is 's' (bit 0 set), bytes
        # 12-15 are "ude!". rom.h ignores byte 9 then, as Punch-Out!! (U) needs.
        data = bytearray(ines())
        data[7:16] = b"DiskDude!"
        self.assertEqual(data[9] & 1, 1)
        self.assertEqual(shots.rom_info(bytes(data)).region, "ntsc")
        self.assertEqual(shots.rom_info(bytes(data), "Game (Europe).nes").region, "pal")

    def test_nes2_timing_byte(self):
        def nes2(timing, byte9=0):
            data = bytearray(ines())
            data[7] = 0x08
            data[9] = byte9
            data[12] = timing
            return bytes(data)
        self.assertEqual(shots.rom_info(nes2(0)).region, "ntsc")
        self.assertEqual(shots.rom_info(nes2(1)).region, "pal")
        self.assertEqual(shots.rom_info(nes2(2)).region, "ntsc")  # multi-region runs NTSC
        self.assertEqual(shots.rom_info(nes2(3)).region, "ntsc")  # only NES_TV_PAL selects the PAL rate
        self.assertEqual(shots.rom_info(nes2(0), "Game (E).nes").region, "ntsc")  # NES 2.0 wins over the name

    def test_file_name_tag(self):
        self.assertEqual(shots.rom_info(ines(), "Metroid (E).nes").region, "pal")
        self.assertEqual(shots.rom_info(ines(), "Metroid (U).nes").region, "ntsc")
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "Metroid (Europe).nes"
            path.write_bytes(ines())
            self.assertEqual(shots.read_rom_info(path).region, "pal")

    def test_chr_ram_rom(self):
        data = ines(prg=1, chr_=0)
        self.assertEqual(shots.rom_info(data).crc, zlib.crc32(data[16:]) & 0xFFFFFFFF)

    def test_rejects_garbage(self):
        with self.assertRaises(ShotListError):
            shots.rom_info(b"NOPE")
        with self.assertRaises(ShotListError):
            shots.rom_info(ines()[:-100])


class StatePaths(unittest.TestCase):
    def test_source_path_template(self):
        info = shots.rom_info(ines(seed=1))
        p = shots.state_source_path("{config}/states/{rom_stem}-{crc}.s{slot}", Path("/r/Zelda (U).nes"),
                                    info, 1, Path("/home/me/.config/mynes"))
        self.assertEqual(p, f"/home/me/.config/mynes/states/Zelda (U)-{info.crc_hex}.s1")
        p = shots.state_source_path("{config}/states/{rom_stem}-{crc}.s{slot}", None, None, 2, Path("/c"))
        self.assertIn("<crc32>", p)
        self.assertTrue(p.endswith(".s2"))

    def test_config_dir(self):
        old = os.environ.get("XDG_CONFIG_HOME")
        try:
            os.environ["XDG_CONFIG_HOME"] = "/x"
            self.assertEqual(shots.config_dir(), Path("/x/mynes"))
            self.assertEqual(shots.config_dir("/explicit"), Path("/explicit"))
        finally:
            if old is None:
                os.environ.pop("XDG_CONFIG_HOME", None)
            else:
                os.environ["XDG_CONFIG_HOME"] = old


class Replays(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(shots.parse_replay("1 80\n140 81\n165 80\n"), [(1, 0x80), (140, 0x81), (165, 0x80)])
        self.assertEqual(shots.parse_replay("  1 ff \n\n"), [(1, 255)])

    def test_rejects(self):
        for bad in ("", "0 00", "5 00\n5 01", "5 00\n4 01", "1 100", "1 zz", "1 00 02", "# comment\n1 00"):
            with self.assertRaises(ShotListError, msg=repr(bad)):
                shots.parse_replay(bad)
        with self.assertRaises(ShotListError):
            shots.parse_replay("".join(f"{i} 00\n" for i in range(1, 130)))
        shots.parse_replay("".join(f"{i} 00\n" for i in range(1, 129)))  # 128 rows ok

    def test_format_round_trip(self):
        rows = [(1, 0x80), (30, 0x82)]
        self.assertEqual(shots.parse_replay(shots.format_replay(rows)), rows)


if __name__ == "__main__":
    unittest.main()
