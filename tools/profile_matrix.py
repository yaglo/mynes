#!/usr/bin/env python3
"""Run serial, alternating fixed-work Instruments captures from a JSON workload list."""
import argparse
import json
import hashlib
import os
from pathlib import Path
import statistics
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("workloads", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--binary", action="append", required=True, metavar="LABEL=PATH")
    parser.add_argument("--rounds", type=int, default=3)
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("rounds must be positive")
    binaries = [item.split("=", 1) for item in args.binary]
    if any(len(item) != 2 for item in binaries):
        parser.error("binary must be LABEL=PATH")
    binaries = [(label, str(Path(path).resolve())) for label, path in binaries]
    workloads = json.loads(args.workloads.read_text())
    for workload in workloads:
        workload["args"] = [os.path.expandvars(os.path.expanduser(arg)) for arg in workload["args"]]
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    (output / "workloads.json").write_text(json.dumps(workloads, indent=2) + "\n")
    (output / "binaries.json").write_text(json.dumps([
        {"label": label, "path": path, "sha256": hashlib.sha256(Path(path).read_bytes()).hexdigest()}
        for label, path in binaries
    ], indent=2) + "\n")
    results = []
    outputs = {}
    for round_number in range(args.rounds):
        order = binaries if round_number % 2 == 0 else list(reversed(binaries))
        for workload in workloads:
            for label, binary in order:
                name = f"{round_number + 1}-{workload['name']}-{label}"
                trace = output / (name + ".trace")
                stdout = output / (name + ".stdout")
                xml = output / (name + ".xml")
                print(f"Recording {name}", flush=True)
                with (output / (name + ".log")).open("w") as log:
                    result = subprocess.run([
                        "xcrun", "xctrace", "record", "--template", "Time Profiler",
                        "--time-limit", "60s", "--output", str(trace),
                        "--target-stdout", str(stdout), "--launch", "--", binary,
                        *workload["args"],
                    ], stdout=log, stderr=subprocess.STDOUT)
                    if result.returncode or not stdout.exists() or "checksum=" not in stdout.read_text():
                        raise RuntimeError(f"Incomplete workload: see {name}.log")
                    subprocess.run([
                        "xcrun", "xctrace", "export", "--input", str(trace),
                        "--xpath", '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]',
                        "--output", str(xml),
                    ], check=True, stdout=log, stderr=subprocess.STDOUT)
                stats = json.loads(subprocess.check_output([
                    sys.executable, str(Path(__file__).with_name("profile_samples.py")),
                    str(xml), "--skip-seconds", "0", "--json",
                ], text=True))
                summary = next(line for line in stdout.read_text().splitlines() if line.startswith("frames="))
                fields = dict(item.split("=", 1) for item in summary.split())
                signature = {key: fields[key] for key in
                             ("frames", "samples", "audio_sum", "audio_energy", "checksum", "mapper", "region")}
                if outputs.setdefault(workload["name"], signature) != signature:
                    raise RuntimeError(f"Emulation output mismatch: {name}: {summary}")
                stats.update(round=round_number + 1, workload=workload["name"],
                             binary=label, summary=summary, arguments=workload["args"])
                results.append(stats)
                (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
                print(f"  {stats['cpu_ms']:.0f} active CPU ms; {summary}", flush=True)
    print("\nActive CPU ms: median [min, max]; compare distributions, not wall-clock FPS.")
    for workload in workloads:
        for label, _ in binaries:
            values = [r["cpu_ms"] for r in results if r["workload"] == workload["name"] and r["binary"] == label]
            print(f"{workload['name']:18} {label:12} {statistics.median(values):8.0f} [{min(values):.0f}, {max(values):.0f}]")


if __name__ == "__main__":
    main()
