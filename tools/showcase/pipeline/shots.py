"""The shot list: shots.json loading and validation, ROM discovery, save-state
names and controller replays. Documented in tools/showcase/README.md."""
from __future__ import annotations

import fnmatch
import json
import os
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

from . import recipes

SHOWCASE_DIR = Path(__file__).resolve().parents[1]
ROOT = SHOWCASE_DIR.parents[1]
PRESETS_DIR = ROOT / "presets"
DEFAULT_SHOTS_FILE = SHOWCASE_DIR / "shots.json"
STATES_DIR = SHOWCASE_DIR / "states"
REPLAYS_DIR = SHOWCASE_DIR / "replays"

DEFAULT_PRESETS = ["sony_pvm_14l2", "jvc_d_series_2000", "toshiba_14af43", "stass_favourite", "vhs_sp_consumer",
                   "bedroom_rf_1990", "famicom_kitchen"]
SECONDS_BY_KIND = {"hero": 6, "feature": 15}
# 100 NES pixels by 93.75 lines is 1358x1120 on a 3840x2880 render (about
# 13.58 x 11.95 render pixels per NES pixel), centred on the 256x240 frame.
DEFAULT_FLICKER_CROP = [78, 73, 100, 93.75]
MAX_REPLAY_ROWS = 128
FEATURE_TYPES = ("five-televisions", "side-by-side")


class ShotListError(ValueError):
    pass


@dataclass
class Shot:
    id: str
    title: str
    scene: str
    rom: str
    state: str
    seconds: float
    kind: str
    presets: list[str]
    flicker_crop: list[float]
    caption: str
    thumbnail_frame: int
    default_preset: str
    readme: list[str]
    readme_seconds: float
    replay: str | None = None
    rom_exclude: list[str] = field(default_factory=list)
    region: str = "ntsc"
    record_after: int = 2
    flicker_frame: int | None = None
    lens: list[str] = field(default_factory=list)
    crops: list[str] = field(default_factory=list)  # a still frame and its detail crop, no clip
    detail_crop: list[float] = field(default_factory=list)  # the site's crop; empty = flicker_crop

    @property
    def frames(self) -> int:
        return recipes.frame_count(self.seconds, self.region)

    @property
    def readme_frames(self) -> int:
        return min(self.frames, recipes.frame_count(self.readme_seconds, self.region))

    @property
    def flicker_first_frame(self) -> int:
        return self.thumbnail_frame if self.flicker_frame is None else self.flicker_frame

    @property
    def state_path(self) -> Path:
        return STATES_DIR / self.state

    @property
    def replay_path(self) -> Path | None:
        return REPLAYS_DIR / self.replay if self.replay else None


@dataclass
class Feature:
    id: str
    type: str
    shot: str
    presets: list[str]
    caption: str
    labels: list[str]
    seconds_per_preset: float = 3


@dataclass
class Defaults:
    seconds: dict
    presets: list[str]
    flicker_crop: list[float]
    record_after: int
    lens_size: tuple[int, int]
    stage_sizes: list[tuple[int, int]]
    readme_size: tuple[int, int]
    hdr_headroom: float
    hdr_white_nits: float
    thumbnail_frame: int
    region: str
    readme_seconds: float
    record_args: list[str]
    state_source: str
    state_slot: int
    font: str | None


@dataclass
class ShotList:
    path: Path
    defaults: Defaults
    preset_meta: dict[str, dict]
    shots: list[Shot]
    features: list[Feature]

    def shot(self, shot_id: str) -> Shot:
        for s in self.shots:
            if s.id == shot_id:
                return s
        raise ShotListError(f"unknown shot {shot_id!r}")

    def feature(self, feature_id: str) -> Feature:
        for f in self.features:
            if f.id == feature_id:
                return f
        raise ShotListError(f"unknown feature {feature_id!r}")

    def select(self, shot_ids: Iterable[str] | None = None,
               presets: Iterable[str] | None = None) -> list[tuple[Shot, str]]:
        """(shot, preset) pairs, filtered by id lists (None = all)."""
        wanted_shots = set(shot_ids) if shot_ids else None
        wanted_presets = set(presets) if presets else None
        if wanted_shots:
            unknown = wanted_shots - {s.id for s in self.shots}
            if unknown:
                raise ShotListError(f"unknown shots: {', '.join(sorted(unknown))}")
        if wanted_presets:
            unknown = wanted_presets - {p for s in self.shots for p in s.presets + s.crops}
            if unknown:
                raise ShotListError(f"presets not used by any shot: {', '.join(sorted(unknown))}")
        pairs = []
        for s in self.shots:
            if wanted_shots and s.id not in wanted_shots:
                continue
            for p in s.presets + s.crops:
                if wanted_presets and p not in wanted_presets:
                    continue
                pairs.append((s, p))
        return pairs

    def all_presets(self) -> list[str]:
        seen = list(self.defaults.presets)
        for s in self.shots:
            for p in s.presets + s.crops:
                if p not in seen:
                    seen.append(p)
        return seen


# ---------------------------------------------------------------------------
# Loading

def _require(obj: dict, key: str, where: str, kind=None):
    if key not in obj:
        raise ShotListError(f"{where}: missing {key!r}")
    value = obj[key]
    if kind is not None and not isinstance(value, kind):
        raise ShotListError(f"{where}: {key!r} must be {kind.__name__}")
    return value


def _ident(value: str, where: str) -> str:
    if not value or any(c for c in value if not (c.isalnum() or c in "-_")):
        raise ShotListError(f"{where}: id {value!r} must be [A-Za-z0-9_-]+")
    return value


def load(path: Path | str = DEFAULT_SHOTS_FILE, presets_dir: Path = PRESETS_DIR) -> ShotList:
    path = Path(path)
    try:
        data = json.loads(path.read_text())
    except (OSError, ValueError) as e:
        raise ShotListError(f"cannot read {path}: {e}") from e
    if not isinstance(data, dict):
        raise ShotListError(f"{path}: top level must be an object")
    d = data.get("defaults", {})
    seconds = d.get("seconds", SECONDS_BY_KIND)
    if not isinstance(seconds, dict) or set(seconds) - set(SECONDS_BY_KIND):
        raise ShotListError("defaults.seconds must be {hero: N, feature: N}")
    seconds = {**SECONDS_BY_KIND, **seconds}
    if "offscreen" in d:
        raise ShotListError("defaults.offscreen is gone: the render sizes are defaults.sizes "
                            "{lens, stage, readme}")
    sizes = d.get("sizes", {})
    hdr = d.get("hdr", {})
    if not isinstance(sizes, dict) or set(sizes) - {"lens", "stage", "readme"}:
        raise ShotListError("defaults.sizes must be {lens: WxH, stage: [WxH, ...], readme: WxH}")
    if not isinstance(hdr, dict) or set(hdr) - {"headroom", "white_nits"}:
        raise ShotListError("defaults.hdr must be {headroom: N, white_nits: N}")
    try:
        lens_size = recipes.parse_size(sizes.get("lens", recipes.size_string(recipes.LENS_SIZE)))
        stage_sizes = [recipes.parse_size(t) for t in
                       sizes.get("stage", [recipes.size_string(z) for z in recipes.STAGE_SIZES])]
        readme_size = recipes.parse_size(sizes.get("readme", recipes.size_string(recipes.README_SIZE)))
    except recipes.RecipeError as e:
        raise ShotListError(f"defaults.sizes: {e}") from e
    if not stage_sizes or len(set(stage_sizes)) != len(stage_sizes):
        raise ShotListError("defaults.sizes.stage must list distinct sizes")
    defaults = Defaults(
        seconds=seconds,
        presets=list(d.get("presets", DEFAULT_PRESETS)),
        flicker_crop=list(d.get("flicker_crop", DEFAULT_FLICKER_CROP)),
        record_after=int(d.get("record_after", 2)),
        lens_size=lens_size,
        stage_sizes=stage_sizes,
        readme_size=readme_size,
        hdr_headroom=float(hdr.get("headroom", recipes.HDR_HEADROOM)),
        hdr_white_nits=float(hdr.get("white_nits", recipes.HDR_WHITE_NITS)),
        thumbnail_frame=int(d.get("thumbnail_frame", 0)),
        region=str(d.get("region", "ntsc")).lower(),
        readme_seconds=float(d.get("readme_seconds", 6)),
        record_args=list(d.get("record_args", [])),
        state_source=str(d.get("state_source", "{config}/states/{rom_stem}-{crc}.s{slot}")),
        state_slot=int(d.get("state_slot", 1)),
        font=d.get("font"),
    )
    try:
        recipes.validate_nes_rect(defaults.flicker_crop)
    except recipes.RecipeError as e:
        raise ShotListError(f"defaults: {e}") from e
    if defaults.hdr_headroom < 1 or defaults.hdr_white_nits <= 0:
        raise ShotListError("defaults.hdr: headroom must be at least 1 and white_nits positive")
    if defaults.region not in recipes.FPS_BY_REGION:
        raise ShotListError(f"defaults.region must be ntsc or pal, got {defaults.region!r}")

    preset_meta = data.get("presets", {})
    if not isinstance(preset_meta, dict):
        raise ShotListError("presets must be an object {id: {name, blurb}}")

    shots, ids = [], set()
    for i, raw in enumerate(_require(data, "shots", path.name, list)):
        where = f"shots[{i}]"
        if not isinstance(raw, dict):
            raise ShotListError(f"{where}: must be an object")
        sid = _ident(_require(raw, "id", where, str), where)
        if sid in ids:
            raise ShotListError(f"{where}: duplicate id {sid!r}")
        ids.add(sid)
        where = f"shot {sid!r}"
        kind = raw.get("kind", "hero")
        if kind not in SECONDS_BY_KIND:
            raise ShotListError(f"{where}: kind must be hero or feature")
        presets = list(raw.get("presets", defaults.presets))
        if not presets:
            raise ShotListError(f"{where}: presets must not be empty")
        region = str(raw.get("region", defaults.region)).lower()
        if region not in recipes.FPS_BY_REGION:
            raise ShotListError(f"{where}: region must be ntsc or pal")
        crop = list(raw.get("flicker_crop", defaults.flicker_crop))
        try:
            recipes.validate_nes_rect(crop)
        except recipes.RecipeError as e:
            raise ShotListError(f"{where}: {e}") from e
        detail = list(raw.get("detail_crop", crop))
        try:
            recipes.validate_nes_rect(detail)
        except recipes.RecipeError as e:
            raise ShotListError(f"{where}: detail_crop: {e}") from e
        readme = list(raw.get("readme", []))
        lens = raw.get("lens", False)
        if lens is True:
            lens = list(presets)
        elif lens is False:
            lens = []
        elif not isinstance(lens, list):
            raise ShotListError(f"{where}: lens must be true, false or a list of presets")
        crops = raw.get("crops", [])
        if not isinstance(crops, list) or not all(isinstance(p, str) for p in crops):
            raise ShotListError(f"{where}: crops must be a list of presets")
        if len(set(crops)) != len(crops):
            raise ShotListError(f"{where}: crops lists a preset twice")
        bad = [p for p in crops if p in presets]
        if bad:
            raise ShotListError(f"{where}: crops repeat clip presets: {bad}")
        default_preset = raw.get("default_preset", presets[0])
        shot = Shot(
            id=sid,
            title=_require(raw, "title", where, str),
            scene=_require(raw, "scene", where, str),
            rom=_require(raw, "rom", where, str),
            rom_exclude=list(raw.get("rom_exclude", [])),
            state=str(raw.get("state", f"{sid}.s1")),
            replay=raw.get("replay"),
            seconds=float(raw.get("seconds", seconds[kind])),
            kind=kind,
            presets=presets,
            flicker_crop=crop,
            caption=str(raw.get("caption", "")),
            thumbnail_frame=int(raw.get("thumbnail_frame", defaults.thumbnail_frame)),
            default_preset=default_preset,
            readme=readme,
            readme_seconds=float(raw.get("readme_seconds", defaults.readme_seconds)),
            region=region,
            record_after=int(raw.get("record_after", defaults.record_after)),
            flicker_frame=raw.get("flicker_frame"),
            lens=lens,
            crops=list(crops),
            detail_crop=detail,
        )
        if shot.seconds <= 0:
            raise ShotListError(f"{where}: seconds must be positive")
        if shot.thumbnail_frame < 0 or shot.thumbnail_frame >= shot.frames:
            raise ShotListError(f"{where}: thumbnail_frame {shot.thumbnail_frame} outside 0..{shot.frames - 1}")
        if shot.flicker_first_frame + recipes.FLICKER_FRAMES > shot.frames:
            raise ShotListError(f"{where}: flicker frames {shot.flicker_first_frame}.."
                                f"{shot.flicker_first_frame + recipes.FLICKER_FRAMES - 1} exceed the clip")
        if default_preset not in presets:
            raise ShotListError(f"{where}: default_preset {default_preset!r} is not in presets")
        bad = [p for p in readme if p not in presets]
        if bad:
            raise ShotListError(f"{where}: readme presets not in presets: {bad}")
        bad = [p for p in lens if p not in presets]
        if bad:
            raise ShotListError(f"{where}: lens presets not in presets: {bad}")
        if "/" in shot.state or "/" in (shot.replay or ""):
            raise ShotListError(f"{where}: state and replay are file names inside states/ and replays/")
        shots.append(shot)

    features, fids = [], set()
    for i, raw in enumerate(data.get("features", [])):
        where = f"features[{i}]"
        if not isinstance(raw, dict):
            raise ShotListError(f"{where}: must be an object")
        fid = _ident(_require(raw, "id", where, str), where)
        if fid in fids or fid in ids:
            raise ShotListError(f"{where}: duplicate id {fid!r}")
        fids.add(fid)
        where = f"feature {fid!r}"
        ftype = _require(raw, "type", where, str)
        if ftype not in FEATURE_TYPES:
            raise ShotListError(f"{where}: type must be one of {', '.join(FEATURE_TYPES)}")
        shot_id = _require(raw, "shot", where, str)
        if shot_id not in ids:
            raise ShotListError(f"{where}: unknown shot {shot_id!r}")
        shot = next(s for s in shots if s.id == shot_id)
        presets = list(raw.get("presets", []))
        if not presets:
            raise ShotListError(f"{where}: presets must not be empty")
        missing = [p for p in presets if p not in shot.presets]
        if missing:
            raise ShotListError(f"{where}: shot {shot_id!r} does not record presets {missing}")
        labels = list(raw.get("labels", []))
        if labels and len(labels) != len(presets):
            raise ShotListError(f"{where}: labels must match presets one to one")
        feature = Feature(
            id=fid, type=ftype, shot=shot_id, presets=presets,
            caption=str(raw.get("caption", "")), labels=labels,
            seconds_per_preset=float(raw.get("seconds_per_preset", 3)),
        )
        if ftype == "five-televisions":
            need = len(presets) * recipes.frame_count(feature.seconds_per_preset, shot.region)
            if need > shot.frames:
                raise ShotListError(f"{where}: needs {need} frames ({len(presets)} x "
                                    f"{feature.seconds_per_preset} s) but shot {shot_id!r} "
                                    f"records {shot.frames}; lengthen the shot")
        elif ftype == "side-by-side":
            if len(presets) != 3:
                raise ShotListError(f"{where}: side-by-side takes exactly three presets")
        features.append(feature)

    shot_list = ShotList(path=path, defaults=defaults, preset_meta=preset_meta,
                         shots=shots, features=features)
    missing = [p for p in shot_list.all_presets() if not (presets_dir / f"{p}.json").exists()]
    if missing:
        raise ShotListError(f"presets without a file in {presets_dir}: {', '.join(missing)}")
    return shot_list


def preset_file(preset: str, presets_dir: Path = PRESETS_DIR) -> Path:
    return presets_dir / f"{preset}.json"


def preset_name(preset: str, presets_dir: Path = PRESETS_DIR) -> str:
    try:
        return str(json.loads(preset_file(preset, presets_dir).read_text()).get("name", preset))
    except (OSError, ValueError):
        return preset


def feature_frames(feature: Feature, shot: Shot) -> int:
    if feature.type == "five-televisions":
        return len(feature.presets) * recipes.frame_count(feature.seconds_per_preset, shot.region)
    return shot.frames


# ---------------------------------------------------------------------------
# ROM discovery

def find_roms(roms_dir: Path | str, pattern: str, exclude: Iterable[str] = ()) -> list[Path]:
    """Case-insensitive recursive glob on file names; excludes are globs too."""
    roms_dir = Path(roms_dir)
    if not roms_dir.is_dir():
        raise ShotListError(f"ROM directory {roms_dir} does not exist")
    pattern_l = pattern.lower()
    excludes = [e.lower() for e in exclude]
    found = []
    for dirpath, _dirnames, filenames in os.walk(roms_dir):
        for name in filenames:
            low = name.lower()
            if fnmatch.fnmatchcase(low, pattern_l) and not any(
                    fnmatch.fnmatchcase(low, e) for e in excludes):
                found.append(Path(dirpath) / name)
    return sorted(found)


def resolve_rom(roms_dir: Path | str, shot: Shot) -> Path:
    """Exactly one ROM per shot, or an error naming what matched."""
    matches = find_roms(roms_dir, shot.rom, shot.rom_exclude)
    if not matches:
        raise ShotListError(f"shot {shot.id!r}: no ROM matches {shot.rom!r} under {roms_dir}")
    if len(matches) > 1:
        listing = "\n  ".join(str(m) for m in matches)
        raise ShotListError(f"shot {shot.id!r}: {shot.rom!r} matches more than one file; "
                            f"narrow \"rom\" or add \"rom_exclude\" in shots.json:\n  {listing}")
    return matches[0]


# ---------------------------------------------------------------------------
# Save states

INES_MAGIC = b"NES\x1a"


@dataclass(frozen=True)
class RomInfo:
    crc: int
    region: str
    prg_size: int
    chr_size: int

    @property
    def crc_hex(self) -> str:
        return f"{self.crc:08x}"


PAL_NAME_TAGS = ("(e)", "(europe)", "(pal)", "(australia)", "(europe, australia)")


MAX_ROM_SIZE = 64 * 1024 * 1024  # INES_MAX_ROM_SIZE in src/nes/rom.h


def _nes2_size(lsb: int, msb: int, unit: int) -> int | None:
    """NES 2.0 ROM size from the iNES size byte and its byte-9 nibble, as
    nes_rom_nes2_size reads it; $F selects the exponent-multiplier form."""
    if msb == 0x0F:
        if lsb >> 2 > 26:
            return None
        size = (1 << (lsb >> 2)) * ((lsb & 0x03) * 2 + 1)
    else:
        size = ((msb << 8) | lsb) * unit
    return size if size <= MAX_ROM_SIZE else None


def header_layout(header: bytes, image_size: int) -> tuple[bool, int, int]:
    """(NES 2.0, PRG bytes, CHR bytes), the way nes_rom_parse_header decides:
    the NES 2.0 bits in byte 7 count only when the sizes they give, byte 9
    included, fit the image; otherwise the header is read as iNES."""
    if (header[7] & 0x0C) == 0x08:
        prg = _nes2_size(header[4], header[9] & 0x0F, 16384)
        chr_ = _nes2_size(header[5], header[9] >> 4, 8192)
        need = 16 + (512 if header[6] & 0x04 else 0)
        if prg is not None and chr_ is not None and need + prg + chr_ <= image_size:
            return True, prg, chr_
    return False, header[4] * 16384, header[5] * 8192


def header_region(header: bytes, name: str | None = None, nes2: bool | None = None) -> str:
    """The TV system the emulator runs, read the way src/nes/rom.h reads it.

    NES 2.0 keeps it in byte 12. ``nes2`` is header_layout's answer; without
    it the byte-7 bits alone decide. An iNES 1.0 header keeps it in byte 9
    bit 0, but only when the header is not archaic: bytes 12-15 zero and no
    NES 2.0 bits that failed the size check. Old dumps such as the
    "DiskDude!" ones carry ASCII in bytes 7-15, so byte 9 is junk and the
    emulator runs NTSC. Without NES 2.0, an NTSC result becomes PAL when the
    file name has a tag such as "(E)" or "(Europe)"."""
    nes2_bits = (header[7] & 0x0C) == 0x08
    if nes2 is None:
        nes2 = nes2_bits
    if nes2:
        return "pal" if header[12] & 0x03 == 1 else "ntsc"
    archaic = nes2_bits or any(header[12:16])
    if not archaic and header[9] & 0x01:
        return "pal"
    if name and any(tag in name.lower() for tag in PAL_NAME_TAGS):
        return "pal"
    return "ntsc"


def rom_info(data: bytes, name: str | None = None) -> RomInfo:
    """CRC-32 over PRG then CHR, as frontends/shared/saves.c names save files."""
    if len(data) < 16 or data[:4] != INES_MAGIC:
        raise ShotListError("not an iNES file")
    nes2, prg, chr_ = header_layout(data[:16], len(data))
    offset = 16 + (512 if data[6] & 0x04 else 0)
    if len(data) < offset + prg + chr_:
        raise ShotListError("iNES file is shorter than its header claims")
    crc = zlib.crc32(data[offset:offset + prg])
    if chr_:
        crc = zlib.crc32(data[offset + prg:offset + prg + chr_], crc)
    return RomInfo(crc & 0xFFFFFFFF, header_region(data[:16], name, nes2), prg, chr_)


def read_rom_info(path: Path | str) -> RomInfo:
    path = Path(path)
    return rom_info(path.read_bytes(), path.name)


def config_dir(explicit: Path | str | None = None) -> Path:
    """The frontend's config directory (frontends/shared/config.c)."""
    if explicit:
        return Path(explicit)
    xdg = os.environ.get("XDG_CONFIG_HOME")
    if xdg:
        return Path(xdg) / "mynes"
    home = Path.home()
    if (home / ".config").is_dir():
        return home / ".config" / "mynes"
    return home / ".mynes"


def state_source_path(template: str, rom: Path | None, info: RomInfo | None,
                      slot: int, config: Path) -> str:
    """Where the emulator wrote slot ``slot`` for ``rom`` (placeholders when unknown)."""
    stem = rom.stem if rom else "<rom name without extension>"
    crc = info.crc_hex if info else "<crc32>"
    return template.format(config=str(config), rom_stem=stem, crc=crc, slot=slot)


# ---------------------------------------------------------------------------
# Controller replays

def parse_replay(text: str) -> list[tuple[int, int]]:
    """Rows of "frame hexmask": frames ascending from 1, at most 128 rows.

    Mirrors load_review_input in frontends/gpu/playback.c: no comments, no
    blank-line tolerance beyond whitespace, masks are 8-bit."""
    rows = []
    previous = 0
    for lineno, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        parts = line.split()
        if len(parts) != 2:
            raise ShotListError(f"line {lineno}: expected 'frame hexmask', got {line!r}")
        try:
            frame = int(parts[0], 10)
            mask = int(parts[1], 16)
        except ValueError as e:
            raise ShotListError(f"line {lineno}: {line!r} is not 'decimal hex'") from e
        if frame <= previous:
            raise ShotListError(f"line {lineno}: frame {frame} must exceed {previous} (ascending, from 1)")
        if mask > 0xFF:
            raise ShotListError(f"line {lineno}: mask {parts[1]} exceeds 8 bits")
        rows.append((frame, mask))
        previous = frame
        if len(rows) > MAX_REPLAY_ROWS:
            raise ShotListError(f"more than {MAX_REPLAY_ROWS} rows")
    if not rows:
        raise ShotListError("replay is empty")
    return rows


def format_replay(rows: Iterable[tuple[int, int]]) -> str:
    return "".join(f"{frame} {mask:02x}\n" for frame, mask in rows)
