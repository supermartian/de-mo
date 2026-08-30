#!/usr/bin/env python3
"""Plot mvstab and vid.stab global motion on a shared timeline."""

import argparse

from compare_vidstab import align_records, compare, read_codec_csv, read_reference


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--codec", required=True, help="mvstab stats CSV")
    parser.add_argument("--vidstab", help="optional vid.stab transform or CSV")
    parser.add_argument("-o", "--output", help="write an image instead of opening a window")
    arguments = parser.parse_args()
    try:
        import matplotlib.pyplot as plot
    except ImportError as error:
        raise SystemExit("plot_motion.py requires matplotlib") from error

    codec = read_codec_csv(arguments.codec)
    reference = read_reference(arguments.vidstab) if arguments.vidstab else []
    sign = compare(codec, reference)["sign"] if reference else 1.0
    frames = [item["frame"] for item in codec]
    figure, axes = plot.subplots(4, 1, sharex=True, figsize=(11, 8))
    for axis, component in zip(axes[:2], ("dx", "dy")):
        axis.plot(frames, [sign * item[component] for item in codec],
                  label="codec MV (sign aligned)")
        if reference:
            aligned_codec, aligned_reference, _, _ = align_records(codec, reference)
            axis.plot([item["frame"] for item in aligned_codec],
                      [item[component] for item in aligned_reference],
                      label="vid.stab", alpha=0.8)
        axis.set_ylabel(f"{component} (px)")
        axis.legend()
        axis.grid(alpha=0.25)
    axes[2].plot(frames, [item["confidence"] for item in codec])
    axes[2].set_ylabel("confidence")
    axes[2].grid(alpha=0.25)
    axes[3].plot(frames, [item["residual"] for item in codec])
    axes[3].set_ylabel("residual (px)")
    axes[3].set_xlabel("display frame")
    axes[3].grid(alpha=0.25)
    figure.tight_layout()
    if arguments.output:
        figure.savefig(arguments.output, dpi=150)
    else:
        plot.show()


if __name__ == "__main__":
    main()
