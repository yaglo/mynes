import shlex
import unittest

from pipeline import recipes
from pipeline.recipes import Rect

HDR_STAGE, AV1_STAGE, SDR_STAGE = recipes.STAGE_OUTPUTS
HDR_LENS, AV1_LENS, SDR_LENS = recipes.LENS_OUTPUTS


def every_command():
    """One of each command the recipes build, for the resampling guard."""
    rect = recipes.flicker_geometry([78, 73, 100, 93.75])
    cmds = [recipes.video_args("r.mov", "o.mp4", spec, cll=(812, 50), fast=fast)
            for spec in recipes.STAGE_OUTPUTS + recipes.LENS_OUTPUTS for fast in (False, True)]
    cmds += [recipes.poster_args("r.mov", "p.webp"), recipes.sdr_png_args("r.mov", "s.png", rect=rect),
             recipes.hdr_raw_args("r.mov", "h.yuv"), recipes.readme_webp_args("r.mov", "r.webp", 361, 80),
             recipes.flicker_webp_args("r.mov", "f.webp", rect, 0),
             recipes.five_televisions_args([f"m{i}.mov" for i in range(5)], [f"c{i}" for i in range(5)],
                                           "five.mp4", 180, recipes.NTSC_RATE, font="/f.ttf"),
             recipes.side_by_side_args(["a", "b", "c"], ["a", "b", "c"], "s.mp4", 901, recipes.NTSC_RATE)]
    return cmds


class FrameCounts(unittest.TestCase):
    def test_ntsc(self):
        self.assertEqual(recipes.frame_count(6), 361)
        self.assertEqual(recipes.frame_count(15), 901)
        self.assertEqual(recipes.frame_count(3), 180)
        self.assertEqual(recipes.frame_count(1), 60)

    def test_pal(self):
        self.assertEqual(recipes.frame_count(6, "pal"), 300)
        self.assertEqual(recipes.frame_count(15, "PAL"), 750)

    def test_rounding_is_half_up(self):
        self.assertEqual(recipes.frame_count(0.5), 30)
        self.assertEqual(int(recipes.frame_count(2.5 / recipes.NTSC_FPS * 1)), 3)

    def test_rejects_nonpositive(self):
        with self.assertRaises(recipes.RecipeError):
            recipes.frame_count(0)
        with self.assertRaises(recipes.RecipeError):
            recipes.seconds_for_frames(0)

    def test_seconds_for_frames_round_trips(self):
        for region in ("ntsc", "pal"):
            for frames in range(1, 2001):
                seconds = recipes.seconds_for_frames(frames, region)
                self.assertEqual(recipes.frame_count(float(seconds), region), frames, (region, frames))
        self.assertEqual(recipes.seconds_for_frames(1), "0.016639267")

    def test_exact_rates(self):
        self.assertAlmostEqual(float(recipes.NTSC_RATE), 60.0988, places=4)
        self.assertAlmostEqual(float(recipes.PAL_RATE), 50.007, places=3)
        self.assertEqual(recipes.rate_string(recipes.NTSC_RATE), "39375000/655171")
        self.assertEqual(recipes.rate_string("150247/2500"), "150247/2500")

    def test_sizes(self):
        self.assertEqual(recipes.parse_size("1920x1440"), (1920, 1440))
        for bad in ("1920", "32x32", "1921x1440", "axb"):
            with self.assertRaises(recipes.RecipeError, msg=bad):
                recipes.parse_size(bad)


class Geometry(unittest.TestCase):
    def test_nes_scale(self):
        self.assertEqual(recipes.nes_scale((3840, 2880)), (15.0, 12.0))
        self.assertEqual(recipes.nes_scale((1920, 1440)), (7.5, 6.0))
        self.assertEqual(recipes.nes_scale((3840, 2880), "15"), (15.0, 15.0))
        self.assertEqual(recipes.nes_scale((3840, 2880), "15x12"), (15.0, 12.0))
        with self.assertRaises(recipes.RecipeError):
            recipes.nes_scale((3840, 2880), "big")

    def test_readme_crop_is_1500x1125(self):
        # One NES pixel is 15x12 render pixels, so 100 pixels by 93.75 lines is
        # 1500x1125; a 100x75 region would be 1500x900.
        self.assertEqual(recipes.flicker_geometry([62, 105, 100, 93.75]), Rect(930, 1260, 1500, 1125))
        self.assertEqual(recipes.flicker_geometry([62, 105, 100, 75]), Rect(930, 1260, 1500, 900))
        self.assertEqual(recipes.flicker_geometry([78, 73, 100, 93.75]), Rect(1170, 876, 1500, 1125))

    def test_detail_crop_is_a_multiple_of_6(self):
        r = recipes.flicker_geometry([62, 105, 100, 93.75], align=6)
        self.assertEqual(r, Rect(930, 1260, 1500, 1122))
        r = recipes.flicker_geometry([1, 1, 3, 3], (1920, 1440), align=2)
        self.assertEqual((r.w % 2, r.h % 2), (0, 0))

    def test_square_scale_override(self):
        self.assertEqual(recipes.flicker_geometry([64, 72, 128, 96], scale="15"), Rect(960, 1080, 1920, 1440))

    def test_clamps_to_render(self):
        r = recipes.flicker_geometry([200, 200, 56, 40])
        self.assertEqual((r.x + r.w, r.y + r.h), (3840, 2880))
        self.assertEqual((r.w, r.h), (840, 480))

    def test_validation(self):
        for bad in ([0, 0, 0, 10], [200, 0, 100, 10], [0, 0, 10], [-1, 0, 10, 10], [0, 200, 10, 50.5],
                    ["a", 0, 1, 1]):
            with self.assertRaises(recipes.RecipeError, msg=bad):
                recipes.flicker_geometry(bad)

    def test_rect(self):
        r = Rect(10, 20, 30, 40)
        self.assertEqual(r.box, (10, 20, 40, 60))
        self.assertEqual(r.crop_filter(), "crop=30:40:10:20")

    def test_thirds(self):
        self.assertEqual(recipes.third_bands(3840), [(0, 1280), (1280, 1280), (2560, 1280)])
        self.assertEqual(sum(w for _, w in recipes.third_bands(1441)), 1441)


class Filters(unittest.TestCase):
    def test_escape_and_drawtext(self):
        self.assertEqual(recipes.filter_escape("a:b,c'd"), "a\\:b\\,c\\'d")
        f = recipes.drawtext("/tmp/cap.txt", 2880, font="/Fonts/A B.ttf")
        self.assertIn("textfile=/tmp/cap.txt", f)
        self.assertIn("fontfile=/Fonts/A B.ttf", f)
        self.assertIn("fontsize=130", f)

    def test_render_matrix(self):
        # The recorder's SDR path leaves swscale's BT.601 conversion untagged.
        self.assertEqual(recipes.sws_matrix({}, False), "bt601")
        self.assertEqual(recipes.sws_matrix({"color_space": "bt709"}, False), "bt709")
        self.assertEqual(recipes.sws_matrix({"color_space": "smpte170m"}, False), "bt601")
        self.assertEqual(recipes.sws_matrix({}, True), "bt2020")
        self.assertEqual(recipes.sws_matrix({"color_space": "bt2020nc"}, True), "bt2020")
        self.assertEqual(recipes.sws_range({}), "tv")
        self.assertEqual(recipes.sws_range({"color_range": "pc"}), "pc")

    def test_colour_filters(self):
        self.assertEqual(recipes.to_rgb("bt601", "tv", "rgb24"), "scale=in_color_matrix=bt601:in_range=tv,format=rgb24")
        f = recipes.sdr_video_filter()
        self.assertTrue(f.startswith("scale=in_color_matrix=bt601:in_range=tv,format=gbrp16le,"
                                     "scale=out_color_matrix=bt709:out_range=tv,format=yuv420p,setparams="))
        self.assertIn("colorspace=bt709", f)
        self.assertEqual(recipes.hdr_video_filter(), "format=yuv420p10le," + recipes.HDR_PARAMS)


class Recorder(unittest.TestCase):
    def test_sdr_pass(self):
        cmd = recipes.record_command("build/bin/mynes_gpu", "r.nes", "presets/p.json", "s.s1",
                                     "out/1920x1440/sdr.mov", 6, replay="r.replay", record_after=2,
                                     size=(1920, 1440))
        self.assertEqual(cmd, ["build/bin/mynes_gpu", "--offscreen", "1920x1440", "--sdr",
                               "--mask-alignment", "pixels", "--preset", "presets/p.json",
                               "--load-state", "s.s1", "--input-replay", "r.replay",
                               "--record", "out/1920x1440/sdr.mov", "--record-seconds", "6",
                               "--record-after", "2", "r.nes"])

    def test_hdr_pass(self):
        cmd = recipes.record_command("g", "r.nes", "p.json", "s", "hdr.mov", "0.133116136", size=(3840, 2880),
                                     hdr=True, headroom=4.0, white_nits=203, extra_args=["--room-reflections"])
        s = shlex.join(cmd)
        self.assertNotIn("--sdr", cmd)
        self.assertIn("--offscreen 3840x2880 --mask-alignment pixels", s)
        self.assertIn("--record hdr.mov --record-hdr --record-headroom 4 --record-hdr-white 203 "
                      "--record-seconds 0.133116136 --record-after 2", s)
        self.assertNotIn("--input-replay", cmd)
        self.assertEqual(cmd[-2:], ["--room-reflections", "r.nes"])  # extra args, then the ROM last
        cmd = recipes.record_command("g", "r.nes", "p", "s", "h.mov", 2.5, hdr=True, headroom=2.5, white_nits=100)
        self.assertEqual(cmd[cmd.index("--record-headroom") + 1], "2.5")
        self.assertEqual(cmd[cmd.index("--record-seconds") + 1], "2.5")


class Encodes(unittest.TestCase):
    def test_stage_hevc_hdr10(self):
        s = shlex.join(recipes.video_args("hdr.mov", "stage-hdr-hevc.mp4", HDR_STAGE, cll=(812, 50)))
        self.assertIn("-map 0:v:0 -map 0:a:0 -vf format=yuv420p10le,setparams=", s)
        self.assertIn("-c:v libx265 -preset slow -crf 18 -profile:v main10 -pix_fmt yuv420p10le -tag:v hvc1", s)
        self.assertIn("hdr10=1:repeat-headers=1:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc", s)
        self.assertIn(":max-cll=812,50:master-display=G(13250,34500)B(7500,3000)R(34000,16000)"
                      "WP(15635,16450)L(10000000,1)", s)
        self.assertIn("-color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc -color_range tv", s)
        self.assertIn("-fps_mode passthrough -c:a aac -b:a 128k -movflags +faststart stage-hdr-hevc.mp4", s)
        self.assertNotIn(" -r ", s)

    def test_stage_av1(self):
        s = shlex.join(recipes.video_args("hdr.mov", "a.mp4", AV1_STAGE, cll=(812, 50)))
        self.assertIn("-c:v libsvtav1 -preset 6 -crf 24 -pix_fmt yuv420p10le", s)
        self.assertIn("enable-hdr=1:mastering-display=G(0.265,0.690)B(0.150,0.060)R(0.680,0.320)"
                      "WP(0.3127,0.3290)L(1000,0.0001):content-light=812,50", s)
        self.assertIn("setparams=color_primaries=bt2020:color_trc=smpte2084", s)
        self.assertIn("-colorspace bt2020nc", s)

    def test_stage_sdr(self):
        s = shlex.join(recipes.video_args("sdr.mov", "s.mp4", SDR_STAGE, matrix="bt601"))
        self.assertIn("-vf scale=in_color_matrix=bt601:in_range=tv,format=gbrp16le,"
                      "scale=out_color_matrix=bt709:out_range=tv,format=yuv420p", s)
        self.assertIn("-c:v libx264 -profile:v high -preset slow -crf 18 -pix_fmt yuv420p", s)
        self.assertIn("-color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv", s)

    def test_lens(self):
        s = shlex.join(recipes.video_args("hdr.mov", "l.mp4", HDR_LENS, cll=(900, 60)))
        self.assertIn("-map 0:v:0 -an", s)
        self.assertIn("-crf 14", s)
        self.assertNotIn("-c:a", s)
        s = shlex.join(recipes.video_args("sdr.mov", "l.mp4", SDR_LENS))
        self.assertIn("-c:v libx265 -preset slow -crf 14 -profile:v main -pix_fmt yuv420p -tag:v hvc1", s)
        self.assertIn("colorprim=bt709:transfer=bt709:colormatrix=bt709", s)
        self.assertIn("-crf 20", shlex.join(recipes.video_args("hdr.mov", "l.mp4", AV1_LENS, cll=(1, 1))))

    def test_fast(self):
        s = shlex.join(recipes.video_args("hdr.mov", "h.mp4", HDR_STAGE, cll=(812, 50), fast=True))
        self.assertIn("format=p010le", s)
        self.assertIn("-c:v hevc_videotoolbox -profile:v main10 -q:v 70 -tag:v hvc1", s)
        self.assertIn("-color_trc smpte2084", s)
        s = shlex.join(recipes.video_args("sdr.mov", "h.mp4", SDR_LENS, fast=True))
        self.assertIn("format=nv12", s)
        self.assertIn("-profile:v main -q:v 80", s)
        self.assertIn("-preset 10", shlex.join(recipes.video_args("h.mov", "a.mp4", AV1_STAGE, cll=(1, 1), fast=True)))
        self.assertIn("-preset veryfast", shlex.join(recipes.video_args("s.mov", "s.mp4", SDR_STAGE, fast=True)))

    def test_errors(self):
        with self.assertRaises(recipes.RecipeError):
            recipes.video_args("hdr.mov", "h.mp4", HDR_STAGE)  # no MaxCLL/MaxFALL
        with self.assertRaises(recipes.RecipeError):
            recipes.video_args("s.mov", "a.mp4", recipes.VideoOutput("x", "av1", False, 20, 0, False))
        with self.assertRaises(recipes.RecipeError):
            recipes.video_args("s.mov", "a.mp4", recipes.VideoOutput("x", "vp9", False, 20, 0, False))


class Stills(unittest.TestCase):
    def test_poster_and_pngs(self):
        s = shlex.join(recipes.poster_args("sdr.mov", "poster.webp", 0))
        self.assertIn("select='\"'\"'eq(n\\,0)'\"'\"',scale=in_color_matrix=bt601:in_range=tv,format=bgra", s)
        self.assertIn("-c:v libwebp -lossless 0 -quality 85", s)
        r = Rect(930, 1260, 1500, 1125)
        cmd = recipes.sdr_png_args("sdr.mov", "f.png", 7, rect=r)
        self.assertIn("select='eq(n\\,7)',crop=1500:1125:930:1260,scale=in_color_matrix=bt601", cmd[cmd.index("-vf") + 1])
        self.assertIn("-c:v png", shlex.join(cmd))
        # The HDR frame leaves ffmpeg as the decoder's own samples: no scale, no format change.
        cmd = recipes.hdr_raw_args("hdr.mov", "h.yuv", 3)
        self.assertEqual(cmd[cmd.index("-vf") + 1], "select='eq(n\\,3)'")
        self.assertEqual(cmd[-5:], ["-f", "rawvideo", "-pix_fmt", "yuv444p12le", "h.yuv"])
        self.assertEqual(recipes.hdr_raw_args("hdr.mov", "h.yuv", pix_fmt="yuv444p10le")[-2], "yuv444p10le")
        with self.assertRaises(recipes.RecipeError):
            recipes.hdr_raw_args("hdr.mov", "h.yuv", pix_fmt="yuv420p10le")

    def test_avifenc(self):
        cmd = recipes.avifenc_args("still-hdr.png", "still-hdr.avif", clli=(812, 50))
        self.assertEqual(cmd[:9], ["avifenc", "--cicp", "9/16/9", "--depth", "10", "--yuv", "444", "--range", "full"])
        self.assertIn("--ignore-icc", cmd)
        self.assertEqual(cmd[cmd.index("--clli") + 1], "812,50")
        self.assertEqual(cmd[-2:], ["still-hdr.png", "still-hdr.avif"])
        self.assertNotIn("--clli", recipes.avifenc_args("a.png", "a.avif"))

    def test_gainmap(self):
        self.assertEqual(recipes.gainmap_args("g.swift", "s.png", "h.png", "o.jpg"),
                         ["swift", "g.swift", "s.png", "h.png", "o.jpg", "0.90"])

    def test_readme_and_flicker(self):
        self.assertEqual(recipes.readme_frames(361), 181)
        s = shlex.join(recipes.readme_webp_args("sdr.mov", "r.webp", 361, 80))
        self.assertIn("trim=end_frame=361,select=", s)
        self.assertIn("setpts=N/(30*TB),scale=in_color_matrix=bt601:in_range=tv,format=bgra", s)
        self.assertIn("-frames:v 181", s)
        self.assertIn("-fps_mode passthrough -enc_time_base 1:1000 -c:v libwebp_anim -lossless 0 -quality 80", s)
        r = Rect(930, 1260, 1500, 1125)
        s = shlex.join(recipes.flicker_webp_args("sdr.mov", "f.webp", r, 10))
        self.assertIn("trim=start_frame=10:end_frame=18,setpts=N/(8*TB),crop=1500:1125:930:1260", s)
        self.assertIn("-enc_time_base 1:1000 -c:v libwebp_anim -lossless 1", s)  # whole-ms durations
        self.assertIn("-frames:v 8", s)
        self.assertIn("-lossless 0 -quality 90", shlex.join(recipes.flicker_webp_args("s", "f", r, 10, quality=90)))


class Features(unittest.TestCase):
    def test_five_televisions(self):
        masters = [f"m{i}.mov" for i in range(5)]
        caps = [f"c{i}.txt" for i in range(5)]
        cmd = recipes.five_televisions_args(masters, caps, "five.mp4", 180, recipes.NTSC_RATE, font="/f.ttf")
        graph = cmd[cmd.index("-filter_complex") + 1]
        self.assertEqual(cmd.count("-i"), 5)
        self.assertIn("[2:v]trim=start_frame=360:end_frame=540", graph)
        self.assertIn("concat=n=5:v=1:a=0,setpts=N/FRAME_RATE/TB,scale=in_color_matrix=bt601", graph)
        self.assertIn("textfile=c3.txt", graph)
        self.assertIn("[0:a]atrim=end=14.975", graph)
        s = shlex.join(cmd)
        self.assertIn("-frames:v 900", s)
        self.assertIn("-crf 14", s)
        self.assertIn("-colorspace bt709", s)
        with self.assertRaises(recipes.RecipeError):
            recipes.five_televisions_args(masters, caps[:2], "x.mp4", 180, recipes.NTSC_RATE)

    def test_side_by_side(self):
        cmd = recipes.side_by_side_args(["a.mov", "b.mov", "c.mov"], ["a", "b", "c"], "sbs.mp4", 901,
                                        recipes.NTSC_RATE, font="/f.ttf")
        graph = cmd[cmd.index("-filter_complex") + 1]
        self.assertIn("[0:v]trim=end_frame=901,setpts=PTS-STARTPTS,crop=1280:2880:0:0", graph)
        self.assertIn("[2:v]trim=end_frame=901,setpts=PTS-STARTPTS,crop=1280:2880:2560:0", graph)
        self.assertIn("hstack=inputs=3", graph)
        self.assertIn("-frames:v 901", shlex.join(cmd))


class NoResampling(unittest.TestCase):
    def test_every_recipe_passes_the_guard(self):
        for cmd in every_command():
            self.assertIsNone(recipes.resampling_problem(cmd), shlex.join(cmd))

    def test_guard_rejects_size_changes(self):
        bad = [
            ["ffmpeg", "-i", "m.mov", "-vf", "scale=1440:1080:flags=lanczos,format=yuv420p", "o.mp4"],
            ["ffmpeg", "-i", "m.mov", "-vf", "scale=w=960:h=720", "o.mp4"],
            ["ffmpeg", "-i", "m.mov", "-vf", "scale=iw/2:-1", "o.mp4"],
            ["ffmpeg", "-i", "m.mov", "-vf", "format=rgb24,scale=in_color_matrix=bt601:w=10", "o.png"],
            ["ffmpeg", "-i", "m.mov", "-filter_complex", "[0:v]zoompan=z=2:d=1:s=1920x1440[v]", "o.mp4"],
            ["ffmpeg", "-i", "m.mov", "-vf", "zscale=w=960:h=720", "o.mp4"],
            ["ffmpeg", "-i", "m.mov", "-s", "960x720", "o.mp4"],
            ["ffmpeg", "-i", "m.mov", "-vf", "scale", "o.mp4"],
            ["ffmpeg", "-i", "m.mov", "-vf", "fps=30", "o.mp4"],
            # Aliases, stream specifiers and instance names reach the same check.
            ["ffmpeg", "-i", "m.mov", "-lavfi", "scale=960:720", "o.mp4"],
            ["ffmpeg", "-i", "m.mov", "-filter:v:0", "scale=960:720", "o.mp4"],
            ["ffmpeg", "-i", "m.mov", "-filter", "scale=960:720", "o.mp4"],
            ["ffmpeg", "-i", "m.mov", "-vf:0", "scale=960:720", "o.mp4"],
            ["ffmpeg", "-i", "m.mov", "-vf", "scale@a=960:720", "o.mp4"],
            ["ffmpeg", "-i", "m.mov", "-s:v:0", "960x720", "o.mp4"],
            ["ffmpeg", "-i", "m.mov", "-video_size:v", "960x720", "o.mp4"],
            # Graphs in files cannot be checked, so they are refused.
            ["ffmpeg", "-i", "m.mov", "-filter_complex_script", "g.txt", "o.mp4"],
            ["ffmpeg", "-i", "m.mov", "-filter_script:v", "g.txt", "o.mp4"],
            ["ffmpeg", "-i", "m.mov", "-/vf", "g.txt", "o.mp4"],
            ["ffmpeg", "-i", "m.mov", "-/filter_complex", "g.txt", "o.mp4"],
        ]
        for cmd in bad:
            self.assertIsNotNone(recipes.resampling_problem(cmd), shlex.join(cmd))

    def test_guard_allows_input_sizes_and_quoted_commas(self):
        ok = ["ffmpeg", "-f", "rawvideo", "-video_size", "64x48", "-i", "-",
              "-vf", "select='eq(n\\,3)',crop=10:10:0:0", "o.png"]
        self.assertIsNone(recipes.resampling_problem(ok))
        audio = ["ffmpeg", "-i", "m.mov", "-filter:a", "aresample=48000", "-af", "volume=2",
                 "-filter_complex_threads", "4", "-vf", "scale@c=in_color_matrix=bt601,format=rgb24", "o.mp4"]
        self.assertIsNone(recipes.resampling_problem(audio))


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
