#!/usr/bin/env python3
"""Summarize residual vid.stab global-motion transforms."""

import argparse
import math

from compare_vidstab import percentile, read_reference, validate_records


def nearest_rank(values, percent):
    ordered = sorted(values)
    index = max(0, math.ceil(len(ordered) * percent / 100.0) - 1)
    return ordered[index]


def summarize_motion(records):
    validate_records(records, "motion")
    if not records:
        raise ValueError("motion transform contains no records")
    translations = [math.hypot(item["dx"], item["dy"]) for item in records]
    rotations = [abs(item.get("theta", 0.0)) * 180.0 / math.pi
                 for item in records]
    return {
        "frames": len(records),
        "translation_median_px": percentile(translations, 50),
        "translation_rms_px": math.sqrt(
            math.fsum(value * value for value in translations) / len(records)),
        "translation_p95_px": nearest_rank(translations, 95),
        "rotation_rms_degrees": math.sqrt(
            math.fsum(value * value for value in rotations) / len(records)),
    }


def print_summary(path, summary):
    print(path)
    print(f"  frames: {summary['frames']}")
    for name in ("translation_median_px", "translation_rms_px",
                 "translation_p95_px", "rotation_rms_degrees"):
        print(f"  {name}: {summary[name]:.6f}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("transforms", nargs="+",
                        help="vid.stab global_motions.trf files")
    arguments = parser.parse_args()
    for path in arguments.transforms:
        print_summary(path, summarize_motion(read_reference(path)))


if __name__ == "__main__":
    main()
