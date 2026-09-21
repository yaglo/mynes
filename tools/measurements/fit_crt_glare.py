#!/usr/bin/env python3
"""Fit an effective Gaussian scatter approximation, not a measured CRT PSF.

No third-party dependencies. Physical picture height is an explicit assumption
when exporting a frontend preset; the measurement does not supply that height.
"""
import argparse
import json
import math
from pathlib import Path


def fit(measurement):
    first, second = measurement["glare"]
    r1 = first["dark_disk_diameter_mm"] / 2
    r2 = second["dark_disk_diameter_mm"] / 2
    q1, q2 = 1 / first["ratio"], 1 / second["ratio"]
    radius = measurement["bright_disk_diameter_mm"] / 2
    # Solve q1/q2 with the finite outer disk retained. The scatter fraction
    # and common bright-reference normalization cancel from this ratio.
    def tail(r, sigma):
        return math.exp(-r * r / (2 * sigma * sigma))

    lo, hi = r1 / 10, radius
    for _ in range(100):
        sigma = (lo + hi) / 2
        outer = tail(radius, sigma)
        ratio = (tail(r1, sigma) - outer) / (tail(r2, sigma) - outer)
        if ratio > q1 / q2:
            lo = sigma
        else:
            hi = sigma
    sigma = (lo + hi) / 2
    outer = tail(radius, sigma)
    fraction = q1 / (tail(r1, sigma) - outer + q1 * outer)
    assert 0 < fraction < 1
    predictions = []
    for point in measurement["glare"]:
        r = point["dark_disk_diameter_mm"] / 2
        q = fraction * (tail(r, sigma) - outer) / (1 - fraction * outer)
        predictions.append({"dark_disk_diameter_mm": 2 * r,
                            "measured_ratio": point["ratio"], "fitted_ratio": 1 / q})
        assert abs(1 / q - point["ratio"]) < 1e-8
    return {"model": "(1-a)*delta + a*normalized_2D_Gaussian(sigma)",
            "sigma_mm": sigma, "scatter_fraction": fraction,
            "predictions": predictions,
            "qualification": "Two fitted points, zero independent validation points; not a unique PSF or isolated glass measurement."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--measurement", type=Path,
                        default=Path(__file__).with_name("hitachi_751_glare.json"))
    parser.add_argument("--base-preset", type=Path)
    parser.add_argument("--output-preset", type=Path)
    parser.add_argument("--picture-height-mm", type=float,
                        help="Assumed physical height of the simulated picture, not host screen height")
    args = parser.parse_args()
    result = fit(json.loads(args.measurement.read_text()))
    if args.base_preset or args.output_preset:
        if not (args.base_preset and args.output_preset and args.picture_height_mm):
            parser.error("preset export requires --base-preset, --output-preset and --picture-height-mm")
        height = args.picture_height_mm
        if not math.isfinite(height) or height <= 0:
            parser.error("picture height must be finite and positive")
        width = result["sigma_mm"] / height
        if not 0.0005 <= width <= 0.05:
            parser.error("requested picture height exceeds the renderer's scatter-width range")
        preset = json.loads(args.base_preset.read_text())
        base_name = preset.get("name", "CRT")
        preset["name"] = "Measured glare experiment"
        preset["description"] = (
            f"{base_name} optics experiment: Hitachi 751 two-point glare fit, "
            f"assumed picture height {height:g} mm. Receiver and tube remain generic; not a Hitachi monitor emulation.")
        tv = preset["tv"]
        tv.update(halation=result["scatter_fraction"], halation_sigma=width,
                  halation_tint_r=1, halation_tint_g=1, halation_tint_b=1,
                  glass_reflection=0)
        args.output_preset.write_text(json.dumps(preset, indent=4) + "\n")
        result["preset"] = str(args.output_preset)
        result["assumed_picture_height_mm"] = height
        result["halation_sigma"] = width
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
