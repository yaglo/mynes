#!/usr/bin/env python3
"""MyNES showcase pipeline: record every shot with the GPU frontend's recorder
at each delivery size, SDR and HDR, encode the site's stage and lens clips,
stills and crops, the README media and the feature clips, and install the site
files into the mynes-web checkout. See tools/showcase/README.md.

Global options go before the subcommand:

  showcase.py --roms ~/roms check           validate shots.json, ROMs, states, tools
  showcase.py --roms ~/roms states          what is missing and how to create it
  showcase.py --roms ~/roms record          run the recorder (serial: it uses the GPU)
  showcase.py --jobs 4 encode               every derived output, in parallel
  showcase.py features                      five-televisions and raw-vs-pvm-vs-rf
  showcase.py install ~/src/mynes-web       copy into assets/hero and merge manifest.json
  showcase.py --roms ~/roms all --site ~/src/mynes-web
Add --dry-run to print every command instead of running it."""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from pipeline import jobs as jobs_mod  # noqa: E402
from pipeline import recipes, runner as runner_mod, shots as shots_mod  # noqa: E402
from pipeline.runner import PipelineError, Runner, run_jobs, summarize  # noqa: E402
from pipeline.shots import ShotListError  # noqa: E402

RECORDER_FLAGS = ["--record", "--record-seconds", "--record-after", "--load-state", "--input-replay",
                  "--record-hdr", "--record-headroom", "--record-hdr-white"]
ENCODERS = ["libx264", "libx265", "libsvtav1", "libwebp", "libwebp_anim", "aac", "png"]
FILTERS = ["drawtext", "hstack", "concat", "setparams", "scale", "crop", "select", "trim"]


def _csv(text: str | None) -> list[str] | None:
    if not text:
        return None
    return [t.strip() for t in text.split(",") if t.strip()]


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--shots-file", type=Path, default=shots_mod.DEFAULT_SHOTS_FILE, help="shot list (shots.json)")
    p.add_argument("--roms", type=Path, help="ROM directory, searched recursively with case-insensitive globs")
    p.add_argument("--build", type=Path, default=jobs_mod.DEFAULT_BUILD, help="CMake build directory (default build)")
    p.add_argument("--binary", type=Path, help="mynes_gpu executable (default <build>/bin/mynes_gpu)")
    p.add_argument("--out", type=Path, default=jobs_mod.DEFAULT_OUT, help="output directory (default tools/showcase/out)")
    p.add_argument("--states", type=Path, default=shots_mod.STATES_DIR, help="save-state directory")
    p.add_argument("--config-dir", type=Path, help="the emulator's config directory (default ~/.config/mynes)")
    p.add_argument("--shots", help="comma-separated shot ids (default all)")
    p.add_argument("--presets", help="comma-separated preset ids (default the shot's list)")
    p.add_argument("--features", help="comma-separated feature ids (default all)")
    p.add_argument("--jobs", type=int, default=None, help="parallel workers (record: 1, encode/features: 2)")
    p.add_argument("--fast", action="store_true",
                   help="hevc_videotoolbox for HEVC (no HDR10 SEI), faster x264 and SVT-AV1 presets")
    p.add_argument("--flicker-scale", help="render pixels per NES pixel for the crops, S or SXxSY "
                   "(default from the render size: 15x12 at 3840x2880)")
    p.add_argument("--font", help="font file for drawtext captions")
    p.add_argument("--ffmpeg", help="ffmpeg executable (default MYNES_FFMPEG, then "
                   "/opt/homebrew/opt/ffmpeg-full/bin/ffmpeg, then PATH)")
    p.add_argument("--ffprobe", help="ffprobe executable (default MYNES_FFPROBE, beside --ffmpeg, "
                   "then ffmpeg-full, then PATH)")
    p.add_argument("--force", action="store_true", help="rebuild outputs even when newer than their inputs")
    p.add_argument("--dry-run", action="store_true", help="print every command; run nothing")
    p.add_argument("--quiet", action="store_true")
    sub = p.add_subparsers(dest="command", required=True)
    sub.add_parser("check", help="validate the shot list, ROM discovery and tool availability")
    sub.add_parser("states", help="list missing save states and how to create each")
    sub.add_parser("record", help="record every render size, SDR and HDR (needs --roms and the states)")
    sub.add_parser("encode", help="stage and lens clips, posters, stills, crops, README media")
    sub.add_parser("features", help="build the feature clips from the full-size SDR renders")
    install_opts = argparse.ArgumentParser(add_help=False)
    install_opts.add_argument("--budget-mb", type=float, default=jobs_mod.DEFAULT_BUDGET_MB,
                              help="refuse when the site's assets/ would exceed this (default 900)")
    install_opts.add_argument("--with-crops", action="store_true",
                              help="also install the detail crops (crop-sdr.png, crop-hdr.avif and @1x)")
    ins = sub.add_parser("install", parents=[install_opts], help="copy site files into the mynes-web checkout")
    ins.add_argument("site", type=Path)
    al = sub.add_parser("all", parents=[install_opts],
                        help="record, encode, features, then install when --site is given")
    al.add_argument("--site", type=Path)
    return p


def make_context(args) -> jobs_mod.Context:
    shot_list = shots_mod.load(args.shots_file)
    return jobs_mod.Context(
        shot_list=shot_list, out=args.out, build=args.build, binary=args.binary, roms=args.roms,
        states_dir=args.states, config_dir=args.config_dir, flicker_scale=args.flicker_scale,
        font=args.font, site=getattr(args, "site", None), fast=args.fast,
        with_crops=getattr(args, "with_crops", False),
        budget_mb=getattr(args, "budget_mb", jobs_mod.DEFAULT_BUDGET_MB))


def selection(ctx, args):
    return ctx.shot_list.select(_csv(args.shots), _csv(args.presets))


def selected_features(ctx, args):
    ids = _csv(args.features)
    if not ids:
        return list(ctx.shot_list.features)
    return [ctx.shot_list.feature(i) for i in ids]


# ---------------------------------------------------------------------------

def cmd_check(ctx: jobs_mod.Context, args, runner: Runner) -> int:
    errors, warnings = [], []
    sl = ctx.shot_list
    d = sl.defaults
    say = runner.say
    say(f"shot list: {sl.path} ({len(sl.shots)} shots, {len(sl.features)} features)")
    say(f"  sizes: stage {', '.join(recipes.size_string(z) for z in d.stage_sizes)}, "
        f"full {recipes.size_string(d.lens_size)}, README {recipes.size_string(d.readme_size)}; "
        f"HDR white {d.hdr_white_nits:g} nits, headroom {d.hdr_headroom:g}")
    for s in sl.shots:
        rect = recipes.flicker_geometry(s.flicker_crop, d.lens_size, ctx.flicker_scale)
        say(f"  {s.id}: {s.seconds:g} s = {s.frames} frames, presets {len(s.presets)}, "
            f"crop {s.flicker_crop} = {rect.w}x{rect.h} px, lens {len(s.lens) or '-'}, readme {s.readme or '-'}")
        if s.thumbnail_frame != 0:
            warnings.append(f"{s.id}: thumbnail_frame {s.thumbnail_frame}: the site freezes clips at frame 0, "
                            f"so poster/still will not match the frozen picture")
        rp = ctx.replay_path(s)
        if rp:
            if not rp.exists():
                errors.append(f"{s.id}: replay {rp} is missing")
            else:
                try:
                    rows = shots_mod.parse_replay(rp.read_text())
                    if rows[-1][0] > s.frames:
                        warnings.append(f"{s.id}: replay row at frame {rows[-1][0]} is past the {s.frames}-frame recording")
                except ShotListError as e:
                    errors.append(f"{s.id}: replay {rp.name}: {e}")
    for f in sl.features:
        shot = sl.shot(f.shot)
        say(f"  feature {f.id}: {f.type} from {f.shot} ({', '.join(f.presets)}), "
            f"{shots_mod.feature_frames(f, shot)} frames")

    say("tools:")
    for tool in ("ffmpeg", "ffprobe"):
        v = runner_mod.tool_version(tool)
        say(f"  {tool}: {runner_mod.tool(tool)}: {v or 'MISSING'}")
        if not v:
            errors.append(f"{tool} not found: brew install ffmpeg-full, or pass --{tool} "
                          f"or set MYNES_{tool.upper()}")
    ver = runner_mod.ffmpeg_version_tuple(runner_mod.tool_version("ffmpeg"))
    if ver and ver < (5, 1):
        errors.append(f"ffmpeg {ver[0]}.{ver[1]} lacks -fps_mode; 5.1 or newer is required")
    enc = runner_mod.ffmpeg_has("encoders", ENCODERS + ["hevc_videotoolbox"])
    for name in ENCODERS:
        if not enc[name]:
            errors.append(f"ffmpeg lacks the {name} encoder (brew install ffmpeg-full)")
    if args.fast and not enc["hevc_videotoolbox"]:
        errors.append("--fast needs the hevc_videotoolbox encoder")
    dec = runner_mod.ffmpeg_has("decoders", ["prores"])
    if not dec["prores"]:
        errors.append("ffmpeg lacks the prores decoder the HDR renders need")
    flt = runner_mod.ffmpeg_has("filters", FILTERS)
    for name, ok in flt.items():
        if not ok:
            errors.append(f"ffmpeg lacks the {name} filter" + (" (build with libfreetype)" if name == "drawtext" else ""))
    say(f"  encoders: {', '.join(n for n, ok in enc.items() if ok) or 'none'}; "
        f"filters: {', '.join(n for n, ok in flt.items() if ok) or 'none'}")
    avifenc = runner_mod.tool_version("avifenc")
    say(f"  avifenc: {runner_mod.tool('avifenc')}: {avifenc or 'MISSING'}")
    if not avifenc:
        errors.append("avifenc not found (brew install libavif): the HDR stills need it")
    ok, reason = jobs_mod.gainmap_available()
    say(f"  gain-map JPEGs: {'swift ' + runner_mod.tool('swift') if ok else 'skipped: ' + reason}")
    for module, why in (("PIL", "Pillow verifies images and makes the SDR @1x crops (pip install Pillow)"),
                        ("numpy", "numpy handles the 16-bit HDR frames (pip install numpy)")):
        try:
            mod = __import__(module)
            say(f"  {module}: {getattr(mod, '__version__', 'present')}")
        except ImportError:
            errors.append(f"{module} is missing: {why}")
    font = runner_mod.find_font(args.font or sl.defaults.font)
    say(f"  caption font: {font or 'MISSING'}")
    if not font:
        errors.append("no caption font found: pass --font /path/to/font.ttf")
    say(f"  recorder: {ctx.binary}")
    if not ctx.binary.exists():
        errors.append(f"{ctx.binary} is missing: build the GPU frontend (cmake --build build) or pass --binary")
    else:
        try:
            help_text = runner.capture([str(ctx.binary), "--help"], timeout=60)
        except (PipelineError, OSError) as e:
            help_text = ""
            warnings.append(f"could not run {ctx.binary} --help: {e}")
        missing = [f for f in RECORDER_FLAGS if f not in help_text]
        if missing:
            errors.append(f"{ctx.binary} --help does not mention {', '.join(missing)}: "
                          f"rebuild with the HDR recorder")
        else:
            say("  recorder flags present: " + ", ".join(RECORDER_FLAGS))

    say("ROMs:")
    if not ctx.roms:
        warnings.append("no --roms DIR given: ROM discovery not checked")
    else:
        for s in sl.shots:
            try:
                rom = ctx.rom_for(s)
                info = shots_mod.read_rom_info(rom)
                say(f"  {s.id}: {rom} (crc {info.crc_hex}, {info.region})")
                if info.region != s.region:
                    warnings.append(f"{s.id}: the emulator runs this ROM as {info.region.upper()}, shots.json "
                                    f"says {s.region.upper()}: the frame count will be off")
            except (ShotListError, OSError) as e:
                errors.append(str(e))

    say("save states:")
    for s in sl.shots:
        p = ctx.state_path(s)
        say(f"  {'ok     ' if p.exists() else 'MISSING'} {p}")
    missing = jobs_mod.missing_states(ctx)
    if missing:
        warnings.append(f"{len(missing)} save state(s) missing; run `showcase.py states` for instructions")

    for w in warnings:
        say(f"warning: {w}")
    for e in errors:
        say(f"error: {e}")
    say(f"check: {len(errors)} error(s), {len(warnings)} warning(s)")
    return 1 if errors else 0


def cmd_states(ctx: jobs_mod.Context, args, runner: Runner) -> int:
    shots = [s for s, _ in selection(ctx, args)]
    seen, missing = set(), []
    for s in shots:
        if s.id not in seen:
            seen.add(s.id)
            if not ctx.state_path(s).exists():
                missing.append(s)
    if not missing:
        runner.say("all save states present")
        return 0
    runner.say(f"{len(missing)} save state(s) missing. For each: run the game, play to the scene, "
               f"press F5, then copy the slot file into {ctx.states_dir}:")
    for s in missing:
        print()
        print(f"[missing] {ctx.state_path(s)}")
        print(jobs_mod.state_instructions(ctx, s))
    print()
    print("The emulator names slot files <ROM file name without extension>-<CRC-32 of PRG+CHR>.s<slot>"
          " under the config directory (docs/gpu-controls.md, Saves and save states). Pass --roms to fill"
          " in the CRC, --config-dir if your config lives elsewhere.")
    return 1


def cmd_record(ctx, args, runner: Runner) -> int:
    pairs = selection(ctx, args)
    if not ctx.roms:
        raise PipelineError("record needs --roms DIR")
    missing = jobs_mod.missing_states(ctx, [s for s, _ in pairs])
    if missing:
        ids = ", ".join(sorted({s.id for s in missing}))
        if not runner.dry_run:
            runner.say("refusing to record: save states missing for " + ids)
            for s in {s.id: s for s in missing}.values():
                print(jobs_mod.state_instructions(ctx, s))
            return 1
        runner.say(f"note: save states missing for {ids}; a real run refuses (see `showcase.py states`)")
    result = run_jobs(jobs_mod.record_jobs(ctx, pairs), runner, workers=args.jobs or 1)
    summarize(result, runner)
    return 0 if result.success else 1


def cmd_encode(ctx, args, runner: Runner) -> int:
    pairs = selection(ctx, args)
    result = run_jobs(jobs_mod.encode_jobs(ctx, pairs), runner, workers=args.jobs or 2)
    summarize(result, runner)
    return 0 if result.success else 1


def cmd_features(ctx, args, runner: Runner) -> int:
    result = run_jobs(jobs_mod.feature_jobs(ctx, selected_features(ctx, args)), runner, workers=args.jobs or 2)
    summarize(result, runner)
    return 0 if result.success else 1


def cmd_install(ctx, args, runner: Runner) -> int:
    jobs_mod.install(ctx, runner, selection(ctx, args))
    return 0


def cmd_all(ctx, args, runner: Runner) -> int:
    if ctx.site:
        jobs_mod.check_site(ctx)  # a mistyped --site fails now, not after hours of recording
    stages = [("record", cmd_record), ("encode", cmd_encode)]
    shots = {s.id for s, _ in selection(ctx, args)}
    presets = {p for _, p in selection(ctx, args)}
    wanted = [f for f in selected_features(ctx, args) if f.shot in shots and set(f.presets) <= presets]
    if wanted:
        stages.append(("features", cmd_features))
    for name, fn in stages:
        runner.say(f"== {name}")
        rc = fn(ctx, args, runner)
        if rc:
            runner.say(f"{name} failed; stopping before the next stage")
            return rc
    if ctx.site:
        runner.say("== install")
        return cmd_install(ctx, args, runner)
    runner.say("no --site given: skipping install")
    return 0


COMMANDS = {"check": cmd_check, "states": cmd_states, "record": cmd_record, "encode": cmd_encode,
            "features": cmd_features, "install": cmd_install, "all": cmd_all}


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    try:
        ctx = make_context(args)
    except ShotListError as e:
        print(f"shots.json: {e}", file=sys.stderr)
        return 2
    log = None if args.command in ("check", "states") else args.out / "showcase.log"
    runner = Runner(dry_run=args.dry_run, force=args.force, log_path=log, quiet=args.quiet)
    try:
        runner.say(f"showcase {args.command}" + (" (dry run)" if args.dry_run else "")
                   + f" - {runner_mod.platform_note()}, cwd {os.getcwd()}")
        for choice in runner_mod.configure_tools(args.ffmpeg, args.ffprobe).values():
            runner.say("using " + choice.describe())
        return COMMANDS[args.command](ctx, args, runner)
    except (PipelineError, ShotListError, recipes.RecipeError) as e:
        runner.say(f"error: {e}")
        return 1
    finally:
        runner.close()


if __name__ == "__main__":
    sys.exit(main())
