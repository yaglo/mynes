"""Browser codecs strings (RFC 6381) derived from what ffprobe reports.

The site offers each clip in several codecs and asks the browser which it
plays (``canPlayType``, ``navigator.mediaCapabilities.decodingInfo``), so the
``type`` of every source must describe the file exactly: profile, tier, level,
bit depth and colour. The strings are built from the decoder configuration
record ffprobe prints with ``-show_data`` (hvcC, av1C, avcC, AudioSpecificConfig)
plus the stream's colour tags, never written by hand.

  HEVC  hvc1.2.4.L153.B0                 ISO/IEC 14496-15 Annex E
  AV1   av01.0.12M.10.0.110.09.16.09.0   AV1 ISOBMFF binding, codecs parameter
  H.264 avc1.640033                      RFC 6381 section 3.3
  AAC   mp4a.40.2                        RFC 6381 section 3.3
"""
from __future__ import annotations

import binascii
import re


class CodecStringError(ValueError):
    pass


# ffprobe colour names to ITU-T H.273 code points.
PRIMARIES = {"reserved0": 0, "bt709": 1, "unknown": 2, "reserved": 3, "bt470m": 4, "bt470bg": 5,
             "smpte170m": 6, "smpte240m": 7, "film": 8, "bt2020": 9, "smpte428": 10,
             "smpte431": 11, "smpte432": 12, "ebu3213": 22}
TRANSFER = {"reserved0": 0, "bt709": 1, "unknown": 2, "reserved": 3, "bt470m": 4, "bt470bg": 5,
            "smpte170m": 6, "smpte240m": 7, "linear": 8, "log100": 9, "log316": 10,
            "iec61966-2-4": 11, "bt1361e": 12, "iec61966-2-1": 13, "bt2020-10": 14,
            "bt2020-12": 15, "smpte2084": 16, "smpte428": 17, "arib-std-b67": 18}
MATRIX = {"gbr": 0, "bt709": 1, "unknown": 2, "reserved": 3, "fcc": 4, "bt470bg": 5,
          "smpte170m": 6, "smpte240m": 7, "ycgco": 8, "bt2020nc": 9, "bt2020c": 10,
          "smpte2085": 11, "chroma-derived-nc": 12, "chroma-derived-c": 13, "ictcp": 14}

_DUMP_LINE = re.compile(r"^\s*[0-9a-fA-F]{8}:\s(.{1,39})")


def parse_hexdump(text: str | None) -> bytes:
    """The bytes of ffprobe's ``extradata`` dump ("00000000: 0102 2000 ...  ascii")."""
    if not text:
        return b""
    out = bytearray()
    for line in text.splitlines():
        m = _DUMP_LINE.match(line)
        if m:
            out += binascii.unhexlify(m.group(1).replace(" ", "").strip())
    return bytes(out)


def _reverse32(value: int) -> int:
    return int(f"{value:032b}"[::-1], 2)


def hevc_string(hvcc: bytes, tag: str = "hvc1") -> str:
    """hvc1.<space><profile>.<compat, bit-reversed hex>.<tier><level>.<constraint bytes>."""
    if len(hvcc) < 13 or hvcc[0] != 1:
        raise CodecStringError("not an HEVCDecoderConfigurationRecord")
    space = hvcc[1] >> 6
    tier = "H" if hvcc[1] & 0x20 else "L"
    profile = hvcc[1] & 0x1F
    compat = int.from_bytes(hvcc[2:6], "big")
    constraints = list(hvcc[6:12])
    while constraints and constraints[-1] == 0:
        constraints.pop()
    level = hvcc[12]
    parts = [tag, ("" if space == 0 else "ABC"[space - 1]) + str(profile),
             f"{_reverse32(compat):X}", f"{tier}{level}"]
    parts += [f"{b:X}" for b in constraints]
    return ".".join(parts)


def av1_string(av1c: bytes, colour: dict | None = None) -> str:
    """av01.P.LLT.DD.M.CCC.cp.tc.mc.F with the colour fields always present."""
    if len(av1c) < 4 or av1c[0] != 0x81:
        raise CodecStringError("not an AV1CodecConfigurationRecord")
    profile = av1c[1] >> 5
    level = av1c[1] & 0x1F
    b = av1c[2]
    tier = "H" if b & 0x80 else "M"
    high_bitdepth, twelve_bit = bool(b & 0x40), bool(b & 0x20)
    depth = 12 if (high_bitdepth and twelve_bit) else 10 if high_bitdepth else 8
    mono = 1 if b & 0x10 else 0
    sub_x, sub_y, position = (b >> 3) & 1, (b >> 2) & 1, b & 3
    colour = colour or {}
    cp = PRIMARIES.get(colour.get("color_primaries") or "unknown", 2)
    tc = TRANSFER.get(colour.get("color_transfer") or "unknown", 2)
    mc = MATRIX.get(colour.get("color_space") or "unknown", 2)
    full = 1 if colour.get("color_range") in ("pc", "jpeg") else 0
    return (f"av01.{profile}.{level:02d}{tier}.{depth:02d}.{mono}.{sub_x}{sub_y}{position}"
            f".{cp:02d}.{tc:02d}.{mc:02d}.{full}")


def avc_string(avcc: bytes, tag: str = "avc1") -> str:
    """avc1.PPCCLL: profile_idc, constraint flags, level_idc from the avcC record."""
    if len(avcc) < 4 or avcc[0] != 1:
        raise CodecStringError("not an AVCDecoderConfigurationRecord")
    return f"{tag}.{avcc[1]:02x}{avcc[2]:02x}{avcc[3]:02x}"


def aac_string(asc: bytes) -> str:
    """mp4a.40.<audioObjectType> from the AudioSpecificConfig."""
    if len(asc) < 2:
        raise CodecStringError("AudioSpecificConfig too short")
    bits = int.from_bytes(asc[:2], "big")
    aot = bits >> 11
    if aot == 31:
        aot = 32 + ((bits >> 5) & 0x3F)
    return f"mp4a.40.{aot}"


def stream_codec_string(stream: dict) -> str:
    """The codecs string for one ffprobe stream (``-show_streams -show_data``)."""
    name = stream.get("codec_name")
    tag = stream.get("codec_tag_string") or ""
    data = parse_hexdump(stream.get("extradata"))
    if name == "hevc":
        if tag not in ("hvc1", "hev1"):
            raise CodecStringError(f"HEVC sample entry {tag!r}: mux with -tag:v hvc1")
        return hevc_string(data, tag)
    if name == "av1":
        return av1_string(data, stream)
    if name == "h264":
        return avc_string(data, tag if tag in ("avc1", "avc3") else "avc1")
    if name == "aac":
        return aac_string(data)
    raise CodecStringError(f"no codecs string for {name!r}")


def mime_type(video_codecs: str, container: str = "video/mp4") -> str:
    """The ``type`` of a <source>: the video stream only, so the same string
    works for canPlayType and for MediaCapabilities, which takes one codec."""
    return f'{container}; codecs="{video_codecs}"'
