"""Measure isolated scanline FWHM from paired 4K final linear captures."""
import argparse
import json
from pathlib import Path

import numpy as np

PRESETS = ("sony_pvm_14l2", "jvc_d_series_2000", "toshiba_14af43", "stass_favourite")


def read_pfm(path):
    with path.open("rb") as stream:
        if stream.readline().strip() != b"PF":
            raise ValueError(f"Not an RGB PFM: {path}")
        width, height = map(int, stream.readline().split())
        scale = float(stream.readline())
        if (width, height) != (3840, 2160) or scale != -1:
            raise ValueError("Expected 3840x2160 little-endian unit-scale capture")
        return np.frombuffer(stream.read(), "<f4").reshape(height, width, 3)[::-1]


def measure(profile):
    peak = int(profile.argmax())
    baseline = min(profile[0], profile[-1])
    half = baseline + (profile[peak] - baseline) / 2
    left = right = peak
    while left > 0 and profile[left] > half:
        left -= 1
    while right < len(profile) - 1 and profile[right] > half:
        right += 1
    if left == peak or right == peak or profile[left] > half or profile[right] > half:
        raise ValueError("The measurement window does not contain an isolated stroke")
    crossing_left = left + (half - profile[left]) / (profile[left + 1] - profile[left])
    crossing_right = right - 1 + (half - profile[right - 1]) / (profile[right] - profile[right - 1])
    return {
        "peak_y": peak + 1240,
        "fwhm_pixels": float(crossing_right - crossing_left),
        "peak_linear_luminance": float(profile[peak]),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", type=Path)
    directory = parser.parse_args().captures
    results = {}
    for preset in PRESETS:
        frames = [read_pfm(directory / f"{preset}.ppm{suffix}.linear.pfm")
                  for suffix in ("", ".next.ppm")]
        mean = (frames[0] + frames[1]) / 2
        results[preset] = []
        for x in (1110, 1830, 2550):
            region = mean[1240:1360, x - 48:x + 48]
            profile = (region * np.array([0.2126, 0.7152, 0.0722])).sum(2).mean(1)
            results[preset].append(measure(profile))
    output = json.dumps(results, indent=2) + "\n"
    (directory / "measurements.json").write_text(output)
    print(output, end="")


if __name__ == "__main__":
    main()
