import shlex
import unittest
from fractions import Fraction

from pipeline import recipes
from pipeline.recipes import Rect


class FrameCounts(unittest.TestCase):
    def test_ntsc(self):
        self.assertEqual(recipes.frame_count(6), 361)
        self.assertEqual(recipes.frame_count(15), 901)
        self.assertEqual(recipes.frame_count(3), 180)
        self.assertEqual(recipes.frame_count(12), 721)
        self.assertEqual(recipes.frame_count(1), 60)

    def test_pal(self):
        self.assertEqual(recipes.frame_count(6, "pal"), 300)
        self.assertEqual(recipes.frame_count(15, "PAL"), 750)

    def test_rounding_is_half_up(self):
        # 0.5 s NTSC = 30.0494 -> 30; a value landing on .5 rounds up, never to even
        self.assertEqual(recipes.frame_count(0.5), 30)
        self.assertEqual(int(recipes.frame_count(2.5 / recipes.NTSC_FPS * 1)), 3)

    def test_rejects_nonpositive(self):
        with self.assertRaises(recipes.RecipeError):
            recipes.frame_count(0)

    def test_exact_rates(self):
        self.assertAlmostEqual(float(recipes.NTSC_RATE), 60.0988, places=4)
        self.assertAlmostEqual(float(recipes.PAL_RATE), 50.007, places=3)
        self.assertEqual(recipes.rate_string(recipes.NTSC_RATE), "39375000/655171")
        self.assertEqual(recipes.rate_string("719503/11972"), "719503/11972")


class Geometry(unittest.TestCase):
    def test_nes_scale_from_master(self):
        self.assertEqual(recipes.nes_scale((3840, 2880)), (15.0, 12.0))
        self.assertEqual(recipes.nes_scale((3840, 2880), "15"), (15.0, 15.0))
        self.assertEqual(recipes.nes_scale((3840, 2880), "15x12"), (15.0, 12.0))
        with self.assertRaises(recipes.RecipeError):
            recipes.nes_scale((3840, 2880), "big")

    def test_flicker_crop_default_region(self):
        # A 128x96 NES region is 1920x1152 master pixels (15 x 12), not 1920x1440.
        self.assertEqual(recipes.flicker_geometry([64, 72, 128, 96]), Rect(960, 864, 1920, 1152))

    def test_flicker_crop_square_scale_override(self):
        self.assertEqual(recipes.flicker_geometry([64, 72, 128, 96], scale="15"),
                         Rect(960, 1080, 1920, 1440))

    def test_flicker_crop_clamps_to_master(self):
        r = recipes.flicker_geometry([200, 200, 56, 40])
        self.assertEqual((r.x + r.w, r.y + r.h), (3840, 2880))
        self.assertEqual((r.w, r.h), (840, 480))

    def test_flicker_crop_even_dimensions(self):
        r = recipes.flicker_geometry([1, 1, 3, 3], (3841, 2881))
        self.assertEqual(r.w % 2, 0)
        self.assertEqual(r.h % 2, 0)

    def test_flicker_crop_validation(self):
        for bad in ([0, 0, 0, 10], [200, 0, 100, 10], [0, 0, 10], [-1, 0, 10, 10]):
            with self.assertRaises(recipes.RecipeError):
                recipes.flicker_geometry(bad)

    def test_push_in_window_is_4_3_at_1_to_1(self):
        w = recipes.push_in_window([64, 72, 128, 96])
        self.assertEqual(w, Rect(960, 720, 1920, 1440))
        self.assertEqual(w.w * 3, w.h * 4)

    def test_push_in_window_clamped_near_top(self):
        w = recipes.push_in_window([64, 8, 128, 96])  # the Punch-Out crowd
        self.assertEqual(w, Rect(960, 0, 1920, 1440))

    def test_thirds(self):
        self.assertEqual(recipes.third_bands(3840), [(0, 1280), (1280, 1280), (2560, 1280)])
        self.assertEqual(sum(w for _, w in recipes.third_bands(1441)), 1441)


class Filters(unittest.TestCase):
    def test_escape(self):
        self.assertEqual(recipes.filter_escape("a:b,c'd"), "a\\:b\\,c\\'d")

    def test_drawtext_uses_textfile(self):
        f = recipes.drawtext("/tmp/cap.txt", 2880, font="/Fonts/A B.ttf")
        self.assertIn("textfile=/tmp/cap.txt", f)
        self.assertIn("fontfile=/Fonts/A B.ttf", f)
        self.assertIn("fontsize=130", f)

    def test_readme_frames(self):
        self.assertEqual(recipes.readme_frames(361), 181)
        self.assertEqual(recipes.readme_frames(60), 30)
        self.assertIn("select='not(mod(n\\,2))'", recipes.readme_filter(361))
        self.assertIn("setpts=N/(30*TB)", recipes.readme_filter(361))
        self.assertIn("scale=960:720", recipes.readme_filter(361))


class Commands(unittest.TestCase):
    def test_record_command(self):
        cmd = recipes.record_command("build/bin/mynes_gpu", "r.nes", "presets/p.json", "s.s1",
                                     "out/master.mov", 6, replay="r.replay", record_after=2)
        self.assertEqual(cmd, ["build/bin/mynes_gpu", "--offscreen", "3840x2880", "--sdr",
                               "--mask-alignment", "pixels", "--preset", "presets/p.json",
                               "--load-state", "s.s1", "--input-replay", "r.replay",
                               "--record", "out/master.mov", "--record-seconds", "6",
                               "--record-after", "2", "r.nes"])
        cmd = recipes.record_command("g", "r.nes", "p.json", "s", "m.mov", 2.5, extra_args=["--room-reflections"])
        self.assertNotIn("--input-replay", cmd)
        self.assertEqual(cmd[-2:], ["--room-reflections", "r.nes"])  # extra args, then the ROM last
        self.assertEqual(cmd[cmd.index("--record-seconds") + 1], "2.5")

    def test_hero(self):
        cmd = recipes.hero_args("m.mov", "hero.mp4", colour={"color_space": "bt709"})
        s = shlex.join(cmd)
        self.assertIn("-c:v libx264 -crf 20 -preset slow -pix_fmt yuv420p", s)
        self.assertIn("scale=1440:1080:flags=lanczos", s)
        self.assertIn("-fps_mode passthrough", s)
        self.assertIn("-c:a aac -b:a 128k", s)
        self.assertIn("-movflags +faststart", s)
        self.assertIn("-colorspace bt709", s)
        self.assertNotIn("-r ", s)  # passthrough only: never let ffmpeg retime
        self.assertIn("-an", recipes.hero_args("m.mov", "h.mp4", audio=False))

    def test_reddit(self):
        s = shlex.join(recipes.reddit_args("m.mov", "r.mp4"))
        self.assertIn("-crf 18", s)
        self.assertIn("-b:a 192k", s)

    def test_youtube_copy_or_encode(self):
        s = shlex.join(recipes.youtube_args("m.mov", "y.mp4", video_codec="h264", audio_codec="aac"))
        self.assertIn("-c:v copy -c:a copy", s)
        s = shlex.join(recipes.youtube_args("m.mov", "y.mp4", video_codec="prores", audio_codec="pcm_f32le"))
        self.assertIn("-c:v libx264 -crf 14 -preset slow", s)
        self.assertIn("-c:a aac -b:a 256k", s)
        s = shlex.join(recipes.youtube_args("m.mov", "y.mp4", video_codec="hevc", audio_codec=None))
        self.assertIn("-c:v copy", s)
        self.assertIn(" -an ", s)

    def test_stills_and_poster(self):
        self.assertIn("-c:v png", shlex.join(recipes.still_png_args("m.mov", "s.png", 7)))
        self.assertIn("eq(n\\,7)", shlex.join(recipes.still_png_args("m.mov", "s.png", 7)))
        s = shlex.join(recipes.still_webp_args("m.mov", "s.webp"))
        self.assertIn("-c:v libwebp -lossless 0 -quality 90", s)
        s = shlex.join(recipes.poster_args("hero.mp4", "p.webp"))
        self.assertIn("-quality 85", s)

    def test_readme_webp(self):
        cmd = recipes.readme_webp_args("m.mov", "r.webp", 361, 80)
        s = shlex.join(cmd)
        self.assertIn("-frames:v 181", s)
        self.assertIn("-c:v libwebp_anim -lossless 0 -quality 80", s)
        self.assertIn("-loop 0", s)

    def test_gif_two_pass(self):
        s = shlex.join(recipes.gif_palette_args("m.mov", "pal.png", 361, 128))
        self.assertIn("palettegen=max_colors=128:stats_mode=diff", s)
        s = shlex.join(recipes.gif_args("m.mov", "pal.png", "r.gif", 361))
        self.assertIn("paletteuse=dither=bayer:bayer_scale=5", s)
        self.assertIn("-frames:v 181", s)

    def test_flicker(self):
        r = recipes.flicker_geometry([64, 72, 128, 96])
        s = shlex.join(recipes.flicker_webp_args("m.mov", "f.webp", r, 10))
        self.assertIn("trim=start_frame=10:end_frame=18", s)
        self.assertIn("setpts=N/(8*TB)", s)
        self.assertIn("crop=1920:1152:960:864", s)
        self.assertIn("-lossless 1", s)
        self.assertIn("-frames:v 8", s)
        s = shlex.join(recipes.flicker_webp_args("m.mov", "f.webp", r, 10, quality=90))
        self.assertIn("-lossless 0 -quality 90", s)
        s = shlex.join(recipes.flicker_png_args("m.mov", "f.png", r, 10))
        self.assertIn("eq(n\\,10)", s)
        self.assertIn("crop=1920:1152:960:864", s)

    def test_five_televisions(self):
        masters = [f"m{i}.mov" for i in range(5)]
        caps = [f"c{i}.txt" for i in range(5)]
        cmd = recipes.five_televisions_args(masters, caps, "five.mp4", 180, recipes.NTSC_RATE,
                                            size=(1440, 1080), font="/f.ttf")
        s = shlex.join(cmd)
        self.assertEqual(cmd.count("-i"), 5)
        graph = cmd[cmd.index("-filter_complex") + 1]
        self.assertIn("[2:v]trim=start_frame=360:end_frame=540", graph)
        self.assertIn("concat=n=5:v=1:a=0", graph)
        self.assertIn("scale=1440:1080", graph)
        self.assertIn("textfile=c3.txt", graph)
        self.assertIn("[0:a]atrim=end=14.975", graph)
        self.assertIn("-frames:v 900", s)
        self.assertIn("-crf 18", s)
        with self.assertRaises(recipes.RecipeError):
            recipes.five_televisions_args(masters, caps[:2], "x.mp4", 180, recipes.NTSC_RATE)

    def test_side_by_side(self):
        cmd = recipes.side_by_side_args(["a.mov", "b.mov", "c.mov"], ["a", "b", "c"], "sbs.mp4", 901,
                                        recipes.NTSC_RATE, font="/f.ttf")
        graph = cmd[cmd.index("-filter_complex") + 1]
        self.assertIn("[0:v]trim=end_frame=901,setpts=PTS-STARTPTS,crop=1280:2880:0:0", graph)
        self.assertIn("[1:v]trim=end_frame=901,setpts=PTS-STARTPTS,crop=1280:2880:1280:0", graph)
        self.assertIn("[2:v]trim=end_frame=901,setpts=PTS-STARTPTS,crop=1280:2880:2560:0", graph)
        self.assertIn("hstack=inputs=3", graph)
        self.assertNotIn("scale=", graph)
        self.assertIn("-frames:v 901", shlex.join(cmd))

    def test_push_in(self):
        w = recipes.push_in_window([64, 72, 128, 96])
        e = recipes.push_in_expressions(w, (3840, 2880), 721)
        self.assertTrue(e["z"].startswith("1+(2.000000-1)*"))
        self.assertIn("on/720", e["z"])
        self.assertIn("(1920.0-iw/2)", e["x"])
        self.assertIn("(1440.0-ih/2)", e["y"])
        cmd = recipes.push_in_args("m.mov", "z.mp4", w, 721, "719503/11972", size=(1440, 1080))
        graph = cmd[cmd.index("-filter_complex") + 1]
        self.assertIn("trim=start_frame=0:end_frame=721", graph)
        self.assertIn(":d=1:s=1920x1440:fps=719503/11972", graph)
        self.assertIn("scale=1440:1080", graph)
        self.assertIn("-crf 14", shlex.join(cmd))
        native = recipes.push_in_args("m.mov", "z.mp4", w, 721, recipes.NTSC_RATE)
        self.assertNotIn("scale=", native[native.index("-filter_complex") + 1])


class Fit(unittest.TestCase):
    def test_stops_at_first_fit(self):
        tried = []

        def build(q):
            tried.append(q)
            return {90: 700, 80: 500, 70: 300}[q]

        self.assertEqual(recipes.fit(400, (90, 80, 70), build), (70, 300))
        self.assertEqual(tried, [90, 80, 70])
        tried.clear()
        self.assertEqual(recipes.fit(1000, (90, 80, 70), build), (90, 700))
        self.assertEqual(tried, [90])

    def test_reports_last_when_nothing_fits(self):
        self.assertEqual(recipes.fit(10, (90, 80), lambda q: q * 10), (80, 800))
        with self.assertRaises(recipes.RecipeError):
            recipes.fit(10, (), lambda q: 0)


if __name__ == "__main__":
    unittest.main()
