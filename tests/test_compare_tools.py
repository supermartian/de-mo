import math
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from compare_vidstab import (  # noqa: E402
    compare, percentile, read_codec_csv, spectral_band_energy)
from summarize_motion import summarize_motion  # noqa: E402


class CompareToolsTest(unittest.TestCase):
    def test_sign_alignment_and_metrics(self):
        codec = [
            {"frame": 0, "dx": -1.0, "dy": 2.0, "theta": 0.1, "confidence": 0.9},
            {"frame": 1, "dx": -2.0, "dy": 3.0, "theta": 0.2, "confidence": 0.4},
            {"frame": 2, "dx": -3.0, "dy": 4.0, "theta": 0.3, "confidence": 0.99},
        ]
        reference = [
            {"frame": 0, "dx": 1.0, "dy": -2.0, "theta": -0.1},
            {"frame": 1, "dx": 2.0, "dy": -3.0, "theta": -0.2},
            {"frame": 2, "dx": 3.0, "dy": -4.0, "theta": -0.3},
        ]
        result = compare(codec, reference)
        self.assertEqual(result["sign"], -1.0)
        self.assertEqual(result["rmse"], 0.0)
        self.assertEqual(result["theta_sign"], -1.0)
        self.assertEqual(result["theta_rmse"], 0.0)
        self.assertEqual(result["confidence"][0.8]["rmse"], 0.0)

    def test_percentile_interpolates(self):
        self.assertEqual(percentile([0.0, 10.0], 50), 5.0)
        self.assertTrue(math.isnan(percentile([], 50)))

    def test_records_align_by_frame_number(self):
        codec = [
            {"frame": 0, "dx": 1.0, "dy": 0.0, "confidence": 1.0},
            {"frame": 2, "dx": 3.0, "dy": 0.0, "confidence": 1.0},
        ]
        reference = [
            {"frame": 0, "dx": 1.0, "dy": 0.0},
            {"frame": 1, "dx": 99.0, "dy": 0.0},
            {"frame": 2, "dx": 3.0, "dy": 0.0},
        ]
        result = compare(codec, reference)
        self.assertEqual(result["count"], 2)
        self.assertEqual(result["unmatched_reference"], 1)
        self.assertEqual(result["rmse"], 0.0)

    def test_derivative_uses_elapsed_pts_time(self):
        codec = [
            {"frame": 0, "pts": 0.0, "dx": 0.0, "dy": 0.0, "confidence": 1.0},
            {"frame": 1, "pts": 0.1, "dx": 1.0, "dy": 0.0, "confidence": 1.0},
            {"frame": 2, "pts": 0.3, "dx": 3.0, "dy": 0.0, "confidence": 1.0},
        ]
        reference = [
            {"frame": item["frame"], "dx": 0.0, "dy": 0.0} for item in codec
        ]
        result = compare(codec, reference)
        self.assertAlmostEqual(result["derivative_rmse"], 10.0)
        self.assertAlmostEqual(result["acceleration_rmse"], 0.0)

    def test_jitter_energy_is_length_stable_and_does_not_alias(self):
        energies = []
        for count in (2048, 2049, 4096):
            alternating = [1.0 if index % 2 == 0 else -1.0
                           for index in range(count)]
            energies.append(spectral_band_energy(alternating, [0.0] * count))
        for energy in energies:
            self.assertGreater(energy, 0.99)
        sinusoid_short = [math.sin(2.0 * math.pi * 0.125 * index)
                          for index in range(1024)]
        sinusoid_long = [math.sin(2.0 * math.pi * 0.125 * index)
                         for index in range(4096)]
        self.assertAlmostEqual(
            spectral_band_energy(sinusoid_short, [0.0] * 1024),
            spectral_band_energy(sinusoid_long, [0.0] * 4096), places=6)

    def test_csv_reader_rejects_nonfinite_motion(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "motion.csv"
            path.write_text(
                "frame_index,dx,dy,confidence,residual_median,pts_seconds\n"
                "0,nan,0,1,0,0\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "non-finite dx at row 2"):
                read_codec_csv(path)

    def test_csv_reader_treats_nan_pts_as_missing(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "motion.csv"
            path.write_text(
                "frame_index,dx,dy,confidence,residual_median,pts_seconds\n"
                "0,1,0,1,0,nan\n", encoding="utf-8")
            self.assertIsNone(read_codec_csv(path)[0]["pts"])

    def test_residual_summary_uses_magnitude_and_degree_units(self):
        records = [
            {"frame": 0, "dx": 3.0, "dy": 0.0, "theta": math.pi / 180.0},
            {"frame": 1, "dx": 0.0, "dy": 4.0,
             "theta": 2.0 * math.pi / 180.0},
        ]
        result = summarize_motion(records)
        self.assertEqual(result["frames"], 2)
        self.assertEqual(result["translation_median_px"], 3.5)
        self.assertAlmostEqual(result["translation_rms_px"], math.sqrt(12.5))
        self.assertEqual(result["translation_p95_px"], 4.0)
        self.assertAlmostEqual(result["rotation_rms_degrees"], math.sqrt(2.5))

    def test_residual_summary_rejects_empty_input(self):
        with self.assertRaisesRegex(ValueError, "contains no records"):
            summarize_motion([])


if __name__ == "__main__":
    unittest.main()
