"""showcase.py end to end in --dry-run, with fake ROMs and states."""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

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
        rc, out = self.run_cli("--dry-run", "--shots", "batman", "record")
        self.assertEqual(rc, 1)
        self.assertIn("refusing to record", out)

    def test_all_dry_run(self):
        states = Path(self.tmp.name) / "states-complete"
        states.mkdir()
        for shot in ("super-mario-bros", "legend-of-zelda", "punch-out", "journey-to-silius", "castlevania-3",
                     "blaster-master", "ninja-gaiden", "mega-man-2", "metroid", "batman"):
            (states / f"{shot}.s1").write_bytes(b"state")
        rc, out = self.run_cli("--dry-run", "all", "--site", str(self.site), states=states)
        self.assertEqual(rc, 0, out[-3000:])
        self.assertIn("dry-run $ ", out)
        self.assertIn("--record-seconds 6 --record-after 2", out)
        self.assertIn("--record-seconds 15", out)
        self.assertIn("--input-replay", out)
        self.assertIn("-c:v libx264 -crf 20 -preset slow", out)
        self.assertIn("zoompan=", out)
        self.assertIn("hstack=inputs=3", out)
        self.assertIn("concat=n=5", out)
        self.assertIn("manifest:", out)
        self.assertEqual(out.count("dry-run $ " + str(ROOT / "build" / "bin" / "mynes_gpu")), 60)  # 10 shots x 6 presets
        self.assertFalse(self.out.exists())  # a dry run writes nothing
        manifest = json.loads((self.site / "assets" / "hero" / "manifest.json").read_text())
        self.assertEqual(manifest["games"], [])  # and installs nothing

    def test_bad_shot_selection(self):
        rc, out = self.run_cli("--dry-run", "--shots", "nope", "encode")
        self.assertEqual(rc, 1)
        self.assertIn("unknown shots: nope", out)


if __name__ == "__main__":
    unittest.main()
