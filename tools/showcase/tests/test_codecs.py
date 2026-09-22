import unittest

from pipeline import codecs
from pipeline.codecs import CodecStringError

# The first lines of `ffprobe -show_data` for a 320x240 libx265 Main10 encode
# and a 1920x1440 libsvtav1 10-bit encode (tools/showcase/tests/test_encode.py
# makes such files and checks the strings against them end to end).
HVCC_DUMP = """
00000000: 0102 2000 0000 9000 0000 0000 3ff0 00fc  .. .........?...
00000010: fdfa fa00 000f 04a0 0001 0018 4001 0c01  ............@...
"""
AV1C_DUMP = """
00000000: 810c 4c00 0a0f 0200 0062 955d fecf 805f  ..L......b.]..._
00000010: 0a04 0412 08                             .....
"""
HDR_TAGS = {"color_primaries": "bt2020", "color_transfer": "smpte2084", "color_space": "bt2020nc",
            "color_range": "tv"}


class Hexdump(unittest.TestCase):
    def test_parse(self):
        data = codecs.parse_hexdump(AV1C_DUMP)
        self.assertEqual(len(data), 21)
        self.assertEqual(data[:4], bytes.fromhex("810c4c00"))
        self.assertEqual(data[-1], 0x08)
        self.assertEqual(codecs.parse_hexdump(None), b"")
        # hex-looking ASCII in the right-hand column is not data
        self.assertEqual(codecs.parse_hexdump("00000000: 0102                                     cafe"),
                         b"\x01\x02")


class Strings(unittest.TestCase):
    def test_hevc_main10(self):
        data = codecs.parse_hexdump(HVCC_DUMP)
        self.assertEqual(codecs.hevc_string(data), "hvc1.2.4.L63.90")
        # The handoff's example: Main10, level 5.1, constraint byte B0.
        rec = bytearray(data[:13])
        rec[6], rec[12] = 0xB0, 153
        self.assertEqual(codecs.hevc_string(bytes(rec)), "hvc1.2.4.L153.B0")

    def test_hevc_main_high_tier_and_profile_space(self):
        rec = bytearray(13)
        rec[0] = 1
        rec[1] = 0x20 | 1                      # high tier, Main
        rec[2:6] = (0x60000000).to_bytes(4, "big")  # Main and Main10 compatible
        rec[6], rec[7] = 0x90, 0x01
        rec[12] = 183
        self.assertEqual(codecs.hevc_string(bytes(rec)), "hvc1.1.6.H183.90.1")
        rec[1] = 0x40 | 2                      # profile space 1
        self.assertTrue(codecs.hevc_string(bytes(rec)).startswith("hvc1.A2."))
        with self.assertRaises(CodecStringError):
            codecs.hevc_string(b"\x00" * 13)

    def test_av1(self):
        data = codecs.parse_hexdump(AV1C_DUMP)
        self.assertEqual(codecs.av1_string(data, HDR_TAGS), "av01.0.12M.10.0.110.09.16.09.0")
        self.assertEqual(codecs.av1_string(data, {}), "av01.0.12M.10.0.110.02.02.02.0")
        rec = bytearray(data[:4])
        rec[1] = (2 << 5) | 16                 # profile 2, level 6.0
        rec[2] = 0x80 | 0x40 | 0x20            # high tier, 12-bit, 4:4:4
        self.assertEqual(codecs.av1_string(bytes(rec), {**HDR_TAGS, "color_range": "pc"}),
                         "av01.2.16H.12.0.000.09.16.09.1")
        with self.assertRaises(CodecStringError):
            codecs.av1_string(b"\x01\x02\x03\x04")

    def test_avc_and_aac(self):
        self.assertEqual(codecs.avc_string(bytes([1, 0x64, 0x00, 0x33, 0xff])), "avc1.640033")
        self.assertEqual(codecs.aac_string(bytes([0x12, 0x08])), "mp4a.40.2")   # AAC-LC 44.1 kHz mono
        self.assertEqual(codecs.aac_string(bytes([0x2b, 0x88])), "mp4a.40.5")   # HE-AAC
        self.assertEqual(codecs.aac_string(bytes([0xf8, 0x40])), "mp4a.40.34")  # escape: 32 + 2

    def test_stream_dispatch(self):
        stream = {"codec_name": "hevc", "codec_tag_string": "hvc1", "extradata": HVCC_DUMP}
        self.assertEqual(codecs.stream_codec_string(stream), "hvc1.2.4.L63.90")
        with self.assertRaises(CodecStringError):  # hev1 plays nowhere in Safari; the pipeline muxes hvc1
            codecs.stream_codec_string({**stream, "codec_tag_string": "[0][0][0][0]"})
        stream = {"codec_name": "av1", "codec_tag_string": "av01", "extradata": AV1C_DUMP, **HDR_TAGS}
        self.assertEqual(codecs.stream_codec_string(stream), "av01.0.12M.10.0.110.09.16.09.0")
        with self.assertRaises(CodecStringError):
            codecs.stream_codec_string({"codec_name": "prores"})

    def test_mime_type(self):
        self.assertEqual(codecs.mime_type("hvc1.2.4.L153.B0"), 'video/mp4; codecs="hvc1.2.4.L153.B0"')


if __name__ == "__main__":
    unittest.main()
