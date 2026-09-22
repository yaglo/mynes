"""showcase.py end to end in --dry-run, with fake ROMs and states."""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from pipeline import recipes, shots

SHOWCASE = Path(__file__).resolve().parents[1]
SCRIPT = SHOWCASE / "showcase.py"
ROOT = SHOWCASE.parents[1]


def ines(seed=0):
    header = b"NES\x1a" + bytes([1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])
    return header + bytes((i * 7 + seed) & 0xFF for i in range(16384 + 8192))


ROMS = ["Super Mario Bros. (World).nes", "Super Mario Bros. 3 (USA).nes", "Legend of Zelda, The (USA).nes",
        "Mike Tyson's Punch-Out!! (USA).nes", "Journey to Silius (USA).nes", "Castlevania III (USA).nes",
        "Blaster Master (USA).nes", "Ninja Gaiden (USA).nes", "Mega Man 2 (USA).nes", "Metroid (USA).nes",
        "Batman - The Video Game (USA).nes"]


class Cli(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        cls.roms = root / "roms"
        cls.roms.mkdir()
        for i, name in enumerate(ROMS):
            (cls.roms / name).write_bytes(ines(i))
        cls.states = root / "states"
        cls.states.mkdir()
        cls.out = root / "out"
        cls.site = root / "site"
        (cls.site / "assets" / "hero").mkdir(parents=True)
        (cls.site / "assets" / "hero" / "manifest.json").write_text(json.dumps(
            {"fps": 60.0988, "aspect": [4, 3], "presets": [], "games": [], "clips": {}}))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_cli(self, *args, states=None, env=None):
        cmd = [sys.executable, str(SCRIPT), "--roms", str(self.roms), "--states", str(states or self.states),
               "--out", str(self.out), *args]
        result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=300,
                                env={**os.environ, **(env or {})})
        return result.returncode, result.stdout + result.stderr

    def test_check_reports_roms_and_states(self):
        rc, out = self.run_cli("check")
        self.assertIn(rc, (0, 1))  # 1 when this build lacks the recorder
        self.assertIn("super-mario-bros: " + str(self.roms / "Super Mario Bros. (World).nes"), out)
        self.assertIn("MISSING", out)
        self.assertIn("save state(s) missing", out)
        self.assertIn("check:", out)

    def test_states_prints_instructions_with_crc(self):
        rc, out = self.run_cli("--shots", "metroid", "states")
        self.assertEqual(rc, 1)
        self.assertIn("[missing]", out)
        self.assertIn("Press F5", out)
        self.assertIn("Metroid (USA)-", out)  # CRC filled in from the ROM
        self.assertIn(str(self.states / "metroid.s1"), out)

    def test_record_refuses_without_states(self):
        rc, out = self.run_cli("--shots", "batman", "record")
        self.assertEqual(rc, 1)
        self.assertIn("refusing to record", out)
        self.assertIn("Press F5", out)
        # A dry run says so and still prints the commands.
        rc, out = self.run_cli("--dry-run", "--shots", "batman", "--presets", "sony_pvm_14l2", "record")
        self.assertEqual(rc, 0, out[-2000:])
        self.assertIn("save states missing for batman", out)
        self.assertIn("--record-hdr", out)

    def test_all_dry_run(self):
        states = Path(self.tmp.name) / "states-complete"
        states.mkdir()
        sl = shots.load()
        for shot in sl.shots:
            (states / f"{shot.id}.s1").write_bytes(b"state")
        rc, out = self.run_cli("--dry-run", "all", "--site", str(self.site), states=states)
        self.assertEqual(rc, 0, out[-3000:])
        lines = out.splitlines()
        recorder = "dry-run $ " + str(ROOT / "build" / "bin" / "mynes_gpu")
        clips = sum(len(s.presets) for s in sl.shots)
        readme = sum(len(s.readme) for s in sl.shots)
        lens = sum(len(s.lens) for s in sl.shots)
        stage = len(sl.defaults.stage_sizes)
        # Every clip at each stage size and the full size, README clips also at
        # 1600x1200, each size recorded twice (SDR and HDR).
        self.assertEqual(out.count(recorder), (clips * (stage + 1) + readme) * 2)
        self.assertEqual(out.count("--offscreen 1600x1200"), readme * 2)
        self.assertEqual(out.count("--record-hdr --record-headroom 4 --record-hdr-white 203"), clips * (stage + 1) + readme)
        self.assertIn("--record-seconds 6 --record-after 2", out)
        self.assertIn("--record-seconds 15", out)
        self.assertIn("--input-replay", out)

        def full_size(shot, preset):
            return [l for l in lines if recorder in l and f"{shot}/{preset}/3840x2880/" in l]

        # The full-size render stops after the still unless a lens clip, a
        # feature or the README flicker crop needs more frames.
        self.assertIn(f"--record-seconds {recipes.seconds_for_frames(1)} ",
                      full_size("legend-of-zelda", "jvc_d_series_2000")[0])
        self.assertIn(f"--record-seconds {recipes.seconds_for_frames(8)} ", full_size("punch-out", "sony_pvm_14l2")[0])
        self.assertIn("--record-seconds 15 ", full_size("mega-man-2", "jvc_d_series_2000")[0])  # a feature
        for s in sl.shots:
            for p in s.lens:
                self.assertIn(f"--record-seconds {s.seconds:g} ", full_size(s.id, p)[0])
        self.assertEqual(out.count("-c:v libx265 -preset slow -crf 18 -profile:v main10"), clips * stage)
        self.assertEqual(out.count("-c:v libsvtav1 -preset 6 -crf 24"), clips * stage)
        self.assertEqual(out.count("-c:v libx264 -profile:v high -preset slow -crf 18"), clips * stage)
        self.assertEqual(out.count("-crf 14 -profile:v main10"), lens)
        self.assertEqual(out.count("-crf 14 -profile:v main "), lens)
        self.assertEqual(out.count("--cicp 9/16/9 --depth 10 --yuv 444"), clips * 3)
        self.assertEqual(out.count("-c:v libwebp_anim"), readme * 2)  # readme.webp and flicker.webp
        self.assertIn("hstack=inputs=3", out)
        self.assertIn("concat=n=5", out)
        self.assertIn(f"would merge {clips} clip(s)", out)
        self.assertNotIn("zoompan", out)
        self.assertNotIn("flags=lanczos", out)
        self.assertFalse(self.out.exists())  # a dry run writes nothing
        manifest = json.loads((self.site / "assets" / "hero" / "manifest.json").read_text())
        self.assertEqual(manifest["games"], [])  # and installs nothing

    def test_all_for_one_shot_and_preset(self):
        rc, out = self.run_cli("--dry-run", "--shots", "legend-of-zelda", "--presets", "sony_pvm_14l2",
                               "all", "--site", str(self.site))
        self.assertEqual(rc, 0, out[-3000:])
        self.assertNotIn("== features", out)  # no feature uses this selection
        self.assertEqual(out.count("dry-run copy"), 10)  # 6 stage files, 2 posters, 2 stills

    def test_bad_shot_selection(self):
        rc, out = self.run_cli("--dry-run", "--shots", "nope", "encode")
        self.assertEqual(rc, 1)
        self.assertIn("unknown shots: nope", out)


if __name__ == "__main__":
    unittest.main()
