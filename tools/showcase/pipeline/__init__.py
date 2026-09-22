"""Showcase capture and encoding pipeline for MyNES (see tools/showcase/README.md).

recipes   pure: frame counts, crop geometry, ffmpeg argument lists
shots     the shot list (shots.json), ROM discovery, save-state names, replays
manifest  the site's assets/hero/manifest.json
codecs    browser codecs strings from ffprobe's decoder configuration records
runner    running and logging commands, ffprobe/Pillow verification, job pool
jobs      turning the shot list into record / encode / feature / install jobs
"""

__all__ = ["recipes", "shots", "manifest", "codecs", "runner", "jobs"]
