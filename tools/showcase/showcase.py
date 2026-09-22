#!/usr/bin/env python3
"""MyNES showcase pipeline: record 4K masters with the GPU frontend's recorder,
encode every site/README/reddit/youtube output, build the feature clips and
install them into the mynes-web checkout. See tools/showcase/README.md.

  showcase.py check --roms ~/roms            validate shots.json, ROMs, states, tools
  showcase.py states --roms ~/roms           what is missing and how to create it
  showcase.py record --roms ~/roms           run the recorder (serial: it uses the GPU)
  showcase.py encode --jobs 4                every derived output, in parallel
  showcase.py features                       five-televisions, raw-vs-pvm-vs-rf, push-in
  showcase.py install ~/src/mynes-web        copy into assets/hero and merge manifest.json
  showcase.py all --roms ~/roms --site ~/src/mynes-web
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
    p.add_argument("--flicker-scale", help="master pixels per NES pixel for the flicker crop, S or SXxSY (default from the master size: 15x12)")
    p.add_argument("--font", help="font file for drawtext captions")
    p.add_argument("--force", action="store_true", help="rebuild outputs even when newer than their inputs")
    p.add_argument("--dry-run", action="store_true", help="print every command; run nothing")
    p.add_argument("--quiet", action="store_true")
    sub = p.add_subparsers(dest="command", required=True)
    sub.add_parser("check", help="validate the shot list, ROM discovery and tool availability")
    sub.add_parser("states", help="list missing save states and how to create each")
    sub.add_parser("record", help="record 4K masters (needs --roms and the states)")
    sub.add_parser("encode", help="hero, still, reddit, youtube, README and flicker outputs")
    sub.add_parser("features", help="build the feature clips from the masters")
    ins = sub.add_parser("install", help="copy site outputs into the mynes-web checkout")
    ins.add_argument("site", type=Path)
    al = sub.add_parser("all", help="record, encode, features, then install when --site is given")
    al.add_argument("--site", type=Path)
    return p


def make_context(args) -> jobs_mod.Context:
    shot_list = shots_mod.load(args.shots_file)
    return jobs_mod.Context(
        shot_list=shot_list, out=args.out, build=args.build, binary=args.binary, roms=args.roms,
        states_dir=args.states, config_dir=args.config_dir, flicker_scale=args.flicker_scale,
        font=args.font, site=getattr(args, "site", None))


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
    say = runner.say
    say(f"shot list: {sl.path} ({len(sl.shots)} shots, {len(sl.features)} features)")
    for s in sl.shots:
        say(f"  {s.id}: {s.seconds:g} s = {s.frames} frames, presets {len(s.presets)}, "
            f"flicker crop {s.flicker_crop}, readme {s.readme or '-'}")
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
        say(f"  {tool}: {v or 'MISSING'}")
        if not v:
            errors.append(f"{tool} is not on PATH (brew install ffmpeg)")
    ver = runner_mod.ffmpeg_version_tuple(runner_mod.tool_version("ffmpeg"))
    if ver and ver < (5, 1):
        errors.append(f"ffmpeg {ver[0]}.{ver[1]} lacks -fps_mode; 5.1 or newer is required")
    enc = runner_mod.ffmpeg_has("encoders", ["libx264", "libwebp", "libwebp_anim", "aac", "png", "gif"])
    for name, ok in enc.items():
        if not ok:
            errors.append(f"ffmpeg lacks the {name} encoder")
    flt = runner_mod.ffmpeg_has("filters", ["zoompan", "drawtext", "palettegen", "paletteuse", "hstack", "scale"])
    for name, ok in flt.items():
        if not ok:
            errors.append(f"ffmpeg lacks the {name} filter" + (" (build with libfreetype)" if name == "drawtext" else ""))
    say(f"  encoders: {', '.join(n for n, ok in enc.items() if ok) or 'none'}; "
        f"filters: {', '.join(n for n, ok in flt.items() if ok) or 'none'}")
    try:
        import PIL  # noqa: F401
        say(f"  Pillow: {PIL.__version__}")
    except ImportError:
        errors.append("Pillow is missing (pip install Pillow); it verifies WebP/GIF/PNG outputs")
    font = runner_mod.find_font(args.font or sl.defaults.font)
    say(f"  caption font: {font or 'MISSING'}")
    if not font:
        errors.append("no caption font found: pass --font /path/to/font.ttf")
    say(f"  recorder: {ctx.binary}")
    if not ctx.binary.exists():
        errors.append(f"{ctx.binary} is missing: build the GPU frontend (cmake --build build)")
    else:
        try:
            help_text = runner.capture([str(ctx.binary), "--help"], timeout=60)
        except (PipelineError, OSError) as e:
            help_text = ""
            warnings.append(f"could not run {ctx.binary} --help: {e}")
        flags = ["--record", "--record-seconds", "--record-after", "--load-state", "--input-replay"]
        missing = [f for f in flags if f not in help_text]
        if missing:
            errors.append(f"{ctx.binary} --help does not mention {', '.join(missing)}: rebuild with the recorder")
        else:
            say("  recorder flags present: " + ", ".join(flags))

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
                    warnings.append(f"{s.id}: ROM header says {info.region}, shots.json says {s.region}: "
                                    f"the frame count will be off")
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
        runner.say("refusing to record: save states missing for " + ", ".join(sorted({s.id for s in missing})))
        for s in {s.id: s for s in missing}.values():
            print(jobs_mod.state_instructions(ctx, s))
        return 1
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
    jobs_mod.install(ctx, runner, selection(ctx, args), selected_features(ctx, args))
    return 0


def cmd_all(ctx, args, runner: Runner) -> int:
    for name, fn in (("record", cmd_record), ("encode", cmd_encode), ("features", cmd_features)):
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
        return COMMANDS[args.command](ctx, args, runner)
    except (PipelineError, ShotListError, recipes.RecipeError) as e:
        runner.say(f"error: {e}")
        return 1
    finally:
        runner.close()


if __name__ == "__main__":
    sys.exit(main())
