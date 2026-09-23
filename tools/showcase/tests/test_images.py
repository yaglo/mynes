import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

try:
    import numpy as np
    from PIL import Image
    HAVE_LIBS = True
except ImportError:  # pragma: no cover
    HAVE_LIBS = False

from pipeline.recipes import Rect
from pipeline.runner import have_tool, tool

if HAVE_LIBS:
    from pipeline import images


def read_png_chunks(path):
    data = Path(path).read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, chunks = 8, []
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        chunks.append((tag, data[pos + 8:pos + 8 + length]))
        pos += 12 + length
    return chunks


@unittest.skipUnless(HAVE_LIBS, "needs numpy and Pillow")
class BoxAverage(unittest.TestCase):
    def test_matches_pillow_reduce(self):
        rng = np.random.default_rng(7)
        rgb8 = rng.integers(0, 256, (48, 64, 3), dtype=np.uint8)
        want = np.asarray(Image.fromarray(rgb8).reduce(2))
        got = images.box_average_2x2(rgb8.astype(np.uint16))
        self.assertTrue(np.array_equal(got, want))

    def test_rounds_half_up_in_16_bit(self):
        block = np.array([[[0, 1, 65535], [1, 1, 65535]], [[1, 2, 65535], [1, 2, 65534]]], dtype=np.uint16)
        self.assertEqual(images.box_average_2x2(block).tolist(), [[[1, 2, 65535]]])  # 3/4 -> 1, 6/4 -> 2 (1.5 up)
        with self.assertRaises(ValueError):
            images.box_average_2x2(np.zeros((3, 4, 3), np.uint16))

    def test_crop_bounds(self):
        rgb = np.zeros((10, 20, 3), np.uint16)
        self.assertEqual(images.crop(rgb, Rect(2, 3, 4, 5)).shape, (5, 4, 3))
        with self.assertRaises(ValueError):
            images.crop(rgb, Rect(18, 0, 4, 4))


@unittest.skipUnless(HAVE_LIBS, "needs numpy and Pillow")
class Png16(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        rng = np.random.default_rng(3)
        self.rgb = rng.integers(0, 65536, (30, 40, 3), dtype=np.uint16)
        self.png = self.dir / "a.png"
        images.write_png16(self.png, self.rgb)

    def tearDown(self):
        self.tmp.cleanup()

    def test_structure(self):
        chunks = dict(read_png_chunks(self.png))
        w, h, depth, colour = struct.unpack(">IIBB", chunks[b"IHDR"][:10])
        self.assertEqual((w, h, depth, colour), (40, 30, 16, 2))
        self.assertEqual(chunks[b"cICP"], bytes([9, 16, 0, 1]))
        raw = zlib.decompress(chunks[b"IDAT"])
        rows = np.frombuffer(raw, np.uint8).reshape(30, 1 + 40 * 6)
        self.assertTrue((rows[:, 0] == 0).all())
        pixels = rows[:, 1:].copy().view(">u2").reshape(30, 40, 3)
        self.assertTrue(np.array_equal(pixels, self.rgb))
        with Image.open(self.png) as im:  # Pillow opens it (as 8-bit), at the right size
            self.assertEqual(im.size, (40, 30))

    @unittest.skipUnless(have_tool("ffmpeg"), "needs ffmpeg")
    def test_ffmpeg_decodes_it_exactly(self):
        raw = self.dir / "a.rgb48"
        subprocess.run([tool("ffmpeg"), "-v", "error", "-i", str(self.png), "-f", "rawvideo",
                        "-pix_fmt", "rgb48le", str(raw)], check=True)
        self.assertTrue(np.array_equal(images.read_rgb48(raw, (40, 30)), self.rgb))
        with self.assertRaises(ValueError):
            images.read_rgb48(raw, (41, 30))


@unittest.skipUnless(HAVE_LIBS, "needs numpy and Pillow")
class YCbCr(unittest.TestCase):
    """Limited-range BT.2020 Y'CbCr to 16-bit R'G'B', exactly (BT.2100 codes)."""

    def planes(self, y, cb, cr):
        return np.array([[[v] for v in y], [[v] for v in cb], [[v] for v in cr]], dtype=np.uint16)

    def test_grey_levels_at_12_bits(self):
        # 10-bit Y' 64, 502, 940 are 256, 2008, 3760 at 12 bits (ProRes 4444 as ffmpeg decodes it).
        got = images.yuv_to_rgb48(self.planes([256, 2008, 3760], [2048] * 3, [2048] * 3), 12)
        self.assertEqual(got[:, 0].tolist(), [[0, 0, 0], [32768, 32768, 32768], [65535, 65535, 65535]])

    def test_every_depth_agrees(self):
        for depth in (10, 12, 16):
            s = 1 << (depth - 8)
            got = images.yuv_to_rgb48(self.planes([16 * s, 235 * s], [128 * s] * 2, [128 * s] * 2), depth)
            self.assertEqual(got[:, 0, 0].tolist(), [0, 65535], depth)
        full = images.yuv_to_rgb48(self.planes([0, 4095], [2048, 2048], [2048, 2048]), 12, full_range=True)
        self.assertEqual(full[:, 0, 0].tolist(), [0, 65535])

    def test_bt2020_red(self):
        # BT.2020 R'G'B' (1, 0, 0) is Y' 0.2627, Cb -0.2627/1.8814, Cr 0.5.
        yuv = self.planes([round(4096 + 56064 * 0.2627)], [round(32768 - 57344 * 0.2627 / 1.8814)],
                          [round(32768 + 57344 * 0.5)])
        got = images.yuv_to_rgb48(yuv, 16)[0, 0].astype(int)
        self.assertLessEqual(np.abs(got - [65535, 0, 0]).max(), 4)

    def test_round_trip(self):
        rng = np.random.default_rng(5)
        rgb = rng.integers(0, 65536, (20, 30, 3), dtype=np.uint16)
        back = images.yuv_to_rgb48(images.rgb48_to_yuv(rgb), 16).astype(int)
        self.assertLessEqual(np.abs(back - rgb).max(), 3)
        with self.assertRaises(ValueError):
            images.yuv_to_rgb48(images.rgb48_to_yuv(rgb), 16, "smpte240m")

    def test_read_planes(self):
        with tempfile.TemporaryDirectory() as tmp:
            raw = Path(tmp) / "f.yuv"
            planes = np.arange(3 * 4 * 6, dtype="<u2").reshape(3, 4, 6)
            planes.tofile(raw)
            self.assertTrue(np.array_equal(images.read_yuv444(raw, (6, 4)), planes))
            with self.assertRaises(ValueError):
                images.read_yuv444(raw, (6, 5))


@unittest.skipUnless(HAVE_LIBS, "needs numpy and Pillow")
class LightLevels(unittest.TestCase):
    def test_pq_reference_points(self):
        # BT.2408 reference white: 203 nits is PQ 0.5806 (the recorder's test value).
        self.assertAlmostEqual(float(images.nits_to_pq(203)), 0.5806, delta=1e-4)
        self.assertAlmostEqual(float(images.nits_to_pq(10000)), 1.0, places=9)
        codes = np.array([0, 38056, 65535], dtype=np.uint16)
        nits = images.pq_to_nits(codes)
        self.assertEqual(nits[0], 0.0)
        self.assertAlmostEqual(float(nits[1]), 203, delta=0.2)
        self.assertAlmostEqual(float(nits[2]), 10000, places=6)

    def test_max_cll_and_fall(self):
        frame = np.zeros((10, 10, 3), dtype=np.uint16)
        code = lambda n: int(round(float(images.nits_to_pq(n)) * 65535))  # noqa: E731
        frame[:, :, 1] = code(100)        # every pixel's brightest component is 100 nits
        frame[0, 0, 0] = code(800)        # one pixel reaches 800 in red
        cll, fall = images.light_levels(frame)
        self.assertAlmostEqual(cll, 800, delta=1)
        self.assertAlmostEqual(fall, (99 * 100 + 800) / 100, delta=1)


@unittest.skipUnless(HAVE_LIBS, "needs numpy and Pillow")
class SdrCrops(unittest.TestCase):
    def test_crop_and_reduce(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            rng = np.random.default_rng(1)
            rgb = rng.integers(0, 256, (60, 80, 3), dtype=np.uint8)
            Image.fromarray(rgb).save(d / "still.png")
            self.assertEqual(images.sdr_crop(d / "still.png", d / "crop.png", Rect(10, 20, 30, 16)), (30, 16))
            with Image.open(d / "crop.png") as im:
                self.assertTrue(np.array_equal(np.asarray(im), rgb[20:36, 10:40]))
            self.assertEqual(images.sdr_reduce(d / "crop.png", d / "crop@1x.png"), (15, 8))
            with Image.open(d / "crop@1x.png") as im:
                want = images.box_average_2x2(rgb[20:36, 10:40].astype(np.uint16))
                self.assertTrue(np.array_equal(np.asarray(im), want))
            images.sdr_crop(d / "still.png", d / "odd.png", Rect(0, 0, 3, 3))
            with self.assertRaises(ValueError):
                images.sdr_reduce(d / "odd.png", d / "odd@1x.png")


if __name__ == "__main__":
    unittest.main()
