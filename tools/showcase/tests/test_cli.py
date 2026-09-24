"""showcase.py end to end in --dry-run, with fake ROMs and states."""
import json
import os
import shlex
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
        "Batman - The Video Game (USA).nes", "Contra (U).nes"]


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
        crops = sum(len(s.crops) for s in sl.shots)
        readme = sum(len(s.readme) for s in sl.shots)
        lens = sum(len(s.lens) for s in sl.shots)
        stage = len(sl.defaults.stage_sizes)
        # Every clip at each stage size and the full size, README clips also at
        # 1600x1200, crop presets at the full size only, each size recorded
        # twice (SDR and HDR).
        passes = clips * (stage + 1) + readme + crops
        self.assertEqual(out.count(recorder), passes * 2)
        self.assertEqual(out.count("--offscreen 1600x1200"), readme * 2)
        self.assertEqual(out.count("--record-hdr --record-headroom 4 --record-hdr-white 203"), passes)
        self.assertIn("--record-seconds 6 --record-after 2", out)
        # The full size keeps the mask at whole pixels; the stage and README sizes use its physical pitch.
        self.assertIn("--offscreen 3840x2880 --mask-alignment pixels", out)
        self.assertIn("--offscreen 1920x1440 --mask-alignment physical", out)
        self.assertIn("--offscreen 960x720 --sdr --mask-alignment physical", out)
        self.assertIn("--offscreen 1600x1200 --mask-alignment physical", out)
        self.assertNotIn("--offscreen 1920x1440 --mask-alignment pixels", out)
        self.assertNotIn("--offscreen 1920x1440 --sdr --mask-alignment pixels", out)
        self.assertIn("--record-seconds 15", out)
        self.assertIn("--input-replay", out)

        def full_size(shot, preset, hdr=False):
            name = "hdr.mov" if hdr else "sdr.mov"
            return [l for l in lines if recorder in l and f"{shot}/{preset}/3840x2880/{name}" in l]

        # The full-size render stops after the still unless a lens clip, a
        # feature or the README flicker crop needs more frames; the HDR pass
        # runs the whole shot only for a lens clip.
        one = f"--record-seconds {recipes.seconds_for_frames(1)} "
        self.assertIn(one, full_size("legend-of-zelda", "jvc_d_series_2000")[0])
        self.assertIn(one, full_size("legend-of-zelda", "jvc_d_series_2000", hdr=True)[0])
        self.assertIn(f"--record-seconds {recipes.seconds_for_frames(8)} ", full_size("punch-out", "sony_pvm_14l2")[0])
        self.assertIn(one, full_size("punch-out", "sony_pvm_14l2", hdr=True)[0])
        self.assertIn("--record-seconds 15 ", full_size("mega-man-2", "jvc_d_series_2000")[0])  # a feature
        self.assertIn(one, full_size("mega-man-2", "jvc_d_series_2000", hdr=True)[0])
        readme_hdr = [l for l in lines if recorder in l and "/1600x1200/hdr.mov" in l]
        self.assertEqual(len(readme_hdr), readme)
        self.assertTrue(all(one in l for l in readme_hdr))
        for s in sl.shots:
            for p in s.lens:
                for hdr in (False, True):
                    self.assertIn(f"--record-seconds {s.seconds:g} ", full_size(s.id, p, hdr)[0])
        # Every pass of a clip (each size, SDR and HDR) starts from the same
        # state, replay and frame, so lens clips and stills show the stage's frames.
        passes: dict[tuple[str, str], list[list[str]]] = {}
        for line in lines:
            if recorder not in line:
                continue
            argv = shlex.split(line.split("dry-run $ ", 1)[1].split("   # ", 1)[0])
            clip = Path(argv[argv.index("--record") + 1]).parts[-4:-2]
            passes.setdefault(clip, []).append(argv)
        self.assertEqual(len(passes), clips + crops)
        for (shot_id, preset), runs in passes.items():
            shot = sl.shot(shot_id)
            if preset in shot.crops:
                self.assertEqual(len(runs), 2, (shot_id, preset))  # the full-size still, SDR and HDR
                self.assertTrue(all("--offscreen 3840x2880" in " ".join(a) and one in " ".join(a) for a in runs))
                continue
            self.assertEqual(len(runs), 2 * (stage + 1 + (preset in shot.readme)), (shot_id, preset))
            for flag in ("--load-state", "--input-replay", "--record-after", "--preset"):
                values = {a[a.index(flag) + 1] if flag in a else None for a in runs}
                self.assertEqual(len(values), 1, (shot_id, preset, flag, values))
            self.assertEqual(len({a[-1] for a in runs}), 1, (shot_id, preset))  # one ROM
        self.assertEqual(out.count("-c:v libx265 -preset slow -crf 18 -profile:v main10"), clips * stage)
        self.assertEqual(out.count("-c:v libsvtav1 -preset 6 -crf 24"), clips * stage)
        self.assertEqual(out.count("-c:v libx264 -profile:v high -preset slow -crf 18"), clips * stage)
        self.assertEqual(out.count("-crf 14 -profile:v main10"), lens)
        self.assertEqual(out.count("-crf 14 -profile:v main "), lens)
        self.assertEqual(out.count("--cicp 9/16/9 --depth 10 --yuv 444"), (clips + crops) * 2)  # still and crop
        # Every README quality is encoded at once and the best kept; the flicker crop is lossless only.
        self.assertEqual(out.count("-c:v libwebp_anim"), readme * (len(recipes.README_QUALITIES) + 1))
        self.assertIn("hstack=inputs=3", out)
        self.assertIn("concat=n=5", out)
        self.assertIn(f"would merge {clips + crops} clip(s)", out)
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

    def test_all_builds_only_the_features_of_the_selection(self):
        """five-televisions uses mega-man-2 alone; raw-vs-pvm-vs-rf needs
        journey-to-silius, which this run does not record."""
        rc, out = self.run_cli("--dry-run", "--shots", "mega-man-2", "all", "--site", str(self.site))
        self.assertEqual(rc, 0, out[-3000:])
        self.assertIn("== features", out)
        self.assertIn("features/five-televisions/youtube.mp4", out)
        self.assertNotIn("raw-vs-pvm-vs-rf", out)

    def test_all_checks_the_site_before_recording(self):
        missing = Path(self.tmp.name) / "no-such-site"
        rc, out = self.run_cli("--dry-run", "--shots", "metroid", "all", "--site", str(missing))
        self.assertEqual(rc, 1)
        self.assertIn(f"site directory {missing} does not exist", out)
        self.assertNotIn("== record", out)
        broken = Path(self.tmp.name) / "broken-site"
        (broken / "assets" / "hero").mkdir(parents=True)
        (broken / "assets" / "hero" / "manifest.json").write_text('{"version": 2, "clips": {')
        rc, out = self.run_cli("--dry-run", "--shots", "metroid", "all", "--site", str(broken))
        self.assertEqual(rc, 1)
        self.assertIn("manifest.json: not valid JSON", out)
        self.assertNotIn("== record", out)

    def test_bad_shot_selection(self):
        rc, out = self.run_cli("--dry-run", "--shots", "nope", "encode")
        self.assertEqual(rc, 1)
        self.assertIn("unknown shots: nope", out)


if __name__ == "__main__":
    unittest.main()
