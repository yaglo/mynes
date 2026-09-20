#!/usr/bin/env python3
"""Summarize active self samples from an Instruments time-profile XML export."""
import argparse
from collections import Counter
import xml.etree.ElementTree as ET


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xml")
    parser.add_argument("--skip-seconds", type=float, default=2.0)
    args = parser.parse_args()
    root = ET.parse(args.xml).getroot()
    ids = {node.attrib["id"]: node for node in root.iter() if "id" in node.attrib}

    def resolve(node):
        return ids[node.attrib["ref"]] if "ref" in node.attrib else node

    functions = Counter()
    groups = Counter()
    count = 0
    for row in root.iter("row"):
        if int(resolve(row[0]).text) < args.skip_seconds * 1e9:
            continue
        if resolve(row[4]).text != "Running":
            continue
        stack = resolve(row[6])
        backtrace = resolve(stack[0]) if len(stack) else None
        name = (resolve(backtrace[0]).get("name", "unknown")
                if backtrace is not None and len(backtrace) else "unknown")
        weight = int(resolve(row[5]).text)
        functions[name] += weight
        group = next((prefix.upper() for prefix in ("ppu", "cpu", "apu", "mapper")
                      if name.startswith(prefix)), "Other")
        groups[group] += weight
        count += 1
    total = sum(functions.values())
    if not total:
        parser.error("no active samples remain after the cutoff")
    print(f"{count} active samples; {total / 1e6:.0f} weighted CPU ms")
    for title, counts in (("Subsystem self samples", groups), ("Top function self samples", functions)):
        print(f"\n{title}")
        for name, weight in counts.most_common(20):
            print(f"{100 * weight / total:6.2f}%  {name}")


if __name__ == "__main__":
    main()
