import tempfile
import unittest
from unittest import mock
from pathlib import Path
import sys

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from refine_motion import (  # noqa: E402
    FlowMotion, affine_center_motion, combine_transform, estimate_flow,
    flow_is_reliable, image_pyramid, phase_correction, read_transform,
    refine_video, write_transform)


class FakeCapture:
    def __init__(self, frames, opened=True):
        self.frames = list(frames)
        self.opened = opened
        self.released = False

    def isOpened(self):
        return self.opened

    def read(self):
        if not self.frames:
            return False, None
        return True, self.frames.pop(0)

    def release(self):
        self.released = True


def synthetic_motion_frames():
    first = np.zeros((128, 192, 3), np.uint8)
    for y in range(12, 120, 18):
        for x in range(12, 184, 20):
            color = ((x * 3) % 255, (y * 5) % 255, 200)
            cv2.circle(first, (x, y), 3, color, -1)
    center = ((first.shape[1] - 1) / 2.0, (first.shape[0] - 1) / 2.0)
    matrix = cv2.getRotationMatrix2D(center, 2.0, 1.0)
    matrix[:, 2] += (6.0, -4.0)
    second = cv2.warpAffine(first, matrix, (first.shape[1], first.shape[0]),
                            borderMode=cv2.BORDER_REFLECT)
    return first, second


class RefineMotionTest(unittest.TestCase):
    def test_image_pyramid_rejects_tiny_frames(self):
        with self.assertRaisesRegex(ValueError, "at least 8 by 8"):
            image_pyramid(np.zeros((4, 4, 3), np.uint8))

    def test_affine_translation_is_measured_at_frame_center(self):
        matrix = np.array([[1.0, -0.1, 4.0], [0.1, 1.0, -3.0]])
        displacement = affine_center_motion(matrix, 101, 51)
        np.testing.assert_allclose(displacement, [1.5, 2.0])

    def test_flow_reliability_rejects_weak_and_extreme_models(self):
        self.assertTrue(flow_is_reliable(FlowMotion(3, 4, 0.1, 0.8, 20)))
        self.assertFalse(flow_is_reliable(FlowMotion(3, 4, 0.1, 0.2, 20)))
        self.assertFalse(flow_is_reliable(FlowMotion(200, 0, 0.1, 0.8, 20)))
        self.assertFalse(flow_is_reliable(FlowMotion(3, 4, 0.1, 0.8, 4)))

    def test_flow_recovers_synthetic_center_motion_and_rotation(self):
        first, second = synthetic_motion_frames()
        before = image_pyramid(first)[4]
        after = image_pyramid(second)[4]
        flow = estimate_flow(before, after)
        self.assertTrue(flow_is_reliable(flow))
        self.assertAlmostEqual(flow.dx, 6.0, delta=1.0)
        self.assertAlmostEqual(flow.dy, -4.0, delta=1.0)
        self.assertAlmostEqual(flow.theta, -np.radians(2.0), delta=0.02)

    def test_reliable_flow_blends_center_motion_and_rotation(self):
        baseline = np.array([0.0, 9.0, 8.0, 0.2, 0.0, 0.0])
        phase = np.array([-10.0, 5.0])
        flow = FlowMotion(8.0, -4.0, 0.1, 0.8, 20)
        result = combine_transform(baseline, phase, flow)
        np.testing.assert_allclose(result[1:3], [-8.56, 4.28])
        self.assertAlmostEqual(result[3], 0.14)

    def test_unreliable_flow_keeps_phase_and_baseline_rotation(self):
        baseline = np.array([0.0, 9.0, 8.0, 0.2, 0.0, 0.0])
        result = combine_transform(
            baseline, np.array([-10.0, 5.0]), FlowMotion())
        np.testing.assert_allclose(result[1:3], [-10.0, 5.0])
        self.assertEqual(result[3], 0.2)

    def test_phase_correction_uses_response_and_scale(self):
        previous = {scale: np.zeros((4, 4), np.uint8) for scale in (2, 4, 8)}
        current = {scale: np.zeros((4, 4), np.uint8) for scale in (2, 4, 8)}
        windows = {scale: np.ones((4, 4), np.float32) for scale in (2, 4, 8)}
        import refine_motion
        original = refine_motion.cv2.phaseCorrelate
        refine_motion.cv2.phaseCorrelate = lambda *unused: ((1.0, -2.0), 0.5)
        try:
            result = phase_correction(previous, current, windows)
        finally:
            refine_motion.cv2.phaseCorrelate = original
        np.testing.assert_allclose(result, [-2.625, 5.25])

    def test_transform_io_rejects_nonfinite_and_round_trips(self):
        with tempfile.TemporaryDirectory() as directory:
            invalid = Path(directory) / "invalid.trf"
            invalid.write_text("0 nan 0 0 0 0\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "non-finite"):
                read_transform(invalid)
            output = Path(directory) / "motion.trf"
            rows = np.array([[0.0, 1.25, -2.5, 0.1, 0.0, 0.0]])
            write_transform(output, rows)
            np.testing.assert_allclose(read_transform(output), rows)

    def test_refine_video_runs_real_estimators_and_releases_capture(self):
        capture = FakeCapture(synthetic_motion_frames())
        baseline = np.zeros((2, 6))
        with mock.patch("refine_motion.cv2.VideoCapture", return_value=capture):
            result = refine_video("synthetic", baseline)
        self.assertTrue(capture.released)
        self.assertLess(result[1, 1], -1.0)
        self.assertGreater(result[1, 2], 1.0)
        self.assertLess(result[1, 3], 0.0)

    def test_refine_video_rejects_both_frame_count_mismatches(self):
        cases = ((synthetic_motion_frames(), np.zeros((1, 6)), "more"),
                 ((synthetic_motion_frames()[0],), np.zeros((2, 6)), "fewer"))
        for frames, baseline, message in cases:
            capture = FakeCapture(frames)
            with self.subTest(message=message), mock.patch(
                    "refine_motion.cv2.VideoCapture", return_value=capture):
                with self.assertRaisesRegex(ValueError, message):
                    refine_video("synthetic", baseline)
                self.assertTrue(capture.released)

    def test_refine_video_releases_capture_on_open_and_processing_errors(self):
        cases = ((FakeCapture([], opened=False), np.zeros((1, 6)), "cannot open"),
                 (FakeCapture([np.zeros((4, 4, 3), np.uint8)]),
                  np.zeros((1, 6)), "at least 8 by 8"))
        for capture, baseline, message in cases:
            with self.subTest(message=message), mock.patch(
                    "refine_motion.cv2.VideoCapture", return_value=capture):
                with self.assertRaisesRegex(ValueError, message):
                    refine_video("synthetic", baseline)
                self.assertTrue(capture.released)


if __name__ == "__main__":
    unittest.main()
