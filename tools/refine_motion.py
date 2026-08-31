#!/usr/bin/env python3
"""Refine mvstab transforms with low-resolution reconstructed luma."""

import argparse
import math
import os
from dataclasses import dataclass
from pathlib import Path
import sys
import tempfile

import cv2
import numpy as np


@dataclass(frozen=True)
class FlowMotion:
    dx: float = 0.0
    dy: float = 0.0
    theta: float = 0.0
    inlier_ratio: float = 0.0
    point_count: int = 0


SCALES = (2, 4, 8)
FLOW_SCALE = 4
PHASE_WEIGHT = 0.375
FLOW_THRESHOLD = 0.35
FLOW_MAXIMUM = 128.0


def read_transform(path):
    rows = []
    with open(path, encoding="utf-8") as source:
        for number, line in enumerate(source, 1):
            fields = line.split()
            if not fields:
                continue
            if len(fields) != 6:
                raise ValueError(f"invalid transform at line {number}")
            try:
                row = [float(value) for value in fields]
            except ValueError as error:
                raise ValueError(f"invalid transform at line {number}") from error
            if not all(math.isfinite(value) for value in row):
                raise ValueError(f"non-finite transform at line {number}")
            rows.append(row)
    if not rows:
        raise ValueError("transform contains no frames")
    return np.array(rows)


def image_pyramid(frame):
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    if gray.shape[0] < max(SCALES) or gray.shape[1] < max(SCALES):
        raise ValueError("video frames must be at least 8 by 8 pixels")
    return {scale: cv2.resize(
        gray, (gray.shape[1] // scale, gray.shape[0] // scale),
        interpolation=cv2.INTER_AREA) for scale in SCALES}


def phase_correction(previous, current, windows):
    correction = np.zeros(2)
    for scale in SCALES:
        before = previous[scale].astype(np.float32)
        after = current[scale].astype(np.float32)
        shift, response = cv2.phaseCorrelate(before, after, windows[scale])
        values = (*shift, response)
        if not all(math.isfinite(value) for value in values):
            continue
        weight = PHASE_WEIGHT * min(max(response, 0.0), 1.0)
        correction -= weight * scale * np.array(shift)
    return correction


def affine_center_motion(matrix, width, height):
    center = np.array([(width - 1) / 2.0, (height - 1) / 2.0, 1.0])
    return matrix @ center - center[:2]


def estimate_flow(previous, current):
    points = cv2.goodFeaturesToTrack(previous, maxCorners=120,
                                     qualityLevel=0.01, minDistance=6,
                                     blockSize=5)
    if points is None or len(points) < 8:
        return FlowMotion()
    moved, status, _ = cv2.calcOpticalFlowPyrLK(
        previous, current, points, None, winSize=(15, 15), maxLevel=3)
    if moved is None or status is None:
        return FlowMotion()
    valid = status.ravel() == 1
    if valid.sum() < 8:
        return FlowMotion(point_count=int(valid.sum()))
    matrix, inliers = cv2.estimateAffinePartial2D(
        points[valid], moved[valid], method=cv2.RANSAC,
        ransacReprojThreshold=1.5, maxIters=500, confidence=0.99)
    if matrix is None or inliers is None:
        return FlowMotion(point_count=int(valid.sum()))
    displacement = affine_center_motion(
        matrix, previous.shape[1], previous.shape[0]) * FLOW_SCALE
    angle = math.atan2(float(matrix[1, 0]), float(matrix[0, 0]))
    return FlowMotion(float(displacement[0]), float(displacement[1]),
                      angle, float(inliers.mean()), int(valid.sum()))


def flow_is_reliable(flow):
    values = (flow.dx, flow.dy, flow.theta, flow.inlier_ratio)
    return (all(math.isfinite(value) for value in values) and
            flow.point_count >= 8 and flow.inlier_ratio >= FLOW_THRESHOLD and
            math.hypot(flow.dx, flow.dy) <= FLOW_MAXIMUM)


def combine_transform(baseline, phase, flow):
    result = baseline.copy()
    result[1:3] = phase
    if flow_is_reliable(flow):
        result[1:3] = 0.6 * phase - 0.32 * np.array([flow.dx, flow.dy])
        result[3] = 0.5 * baseline[3] + 0.4 * flow.theta
    return result


def create_windows(pyramid):
    return {scale: cv2.createHanningWindow(
        (image.shape[1], image.shape[0]), cv2.CV_32F)
            for scale, image in pyramid.items()}


def refine_video(video_path, baseline):
    cv2.setRNGSeed(0)
    capture = cv2.VideoCapture(str(video_path))
    refined = []
    previous = None
    windows = None
    try:
        if not capture.isOpened():
            raise ValueError(f"cannot open input video: {video_path}")
        while True:
            success, frame = capture.read()
            if not success:
                break
            index = len(refined)
            if index >= len(baseline):
                raise ValueError("video has more frames than the transform")
            current = image_pyramid(frame)
            if previous is None:
                refined.append(baseline[index].copy())
                windows = create_windows(current)
            else:
                phase = phase_correction(previous, current, windows)
                flow = estimate_flow(previous[FLOW_SCALE], current[FLOW_SCALE])
                refined.append(combine_transform(baseline[index], phase, flow))
            previous = current
    finally:
        capture.release()
    if len(refined) != len(baseline):
        raise ValueError("video has fewer frames than the transform")
    return np.array(refined)


def write_transform(path, transforms):
    destination = Path(path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
                "w", dir=destination.parent, prefix=f".{destination.name}.",
                delete=False, encoding="utf-8") as output:
            temporary = Path(output.name)
            for row in transforms:
                output.write(f"{int(row[0])} {row[1]:.9f} {row[2]:.9f} "
                             f"{row[3]:.9f} {row[4]:.9f} {int(row[5])}\n")
        os.replace(temporary, destination)
    except Exception:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
        raise


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="refine mvstab motion with low-resolution luma")
    parser.add_argument("video")
    parser.add_argument("baseline", help="relative transform from mvstab analyze")
    parser.add_argument("-o", "--output", required=True)
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    try:
        baseline = read_transform(arguments.baseline)
        refined = refine_video(arguments.video, baseline)
        write_transform(arguments.output, refined)
    except (OSError, ValueError, cv2.error) as error:
        print(f"refine_motion: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
