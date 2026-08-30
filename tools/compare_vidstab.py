#!/usr/bin/env python3
"""Compare mvstab per-frame statistics with global vid.stab motion."""

import argparse
import csv
import math
from pathlib import Path


def finite_float(text, field, row):
    try:
        value = float(text)
    except (TypeError, ValueError) as error:
        raise ValueError(f"invalid {field} at row {row}: {text!r}") from error
    if not math.isfinite(value):
        raise ValueError(f"non-finite {field} at row {row}: {text!r}")
    return value


def read_codec_csv(path):
    records = []
    with Path(path).open(newline="", encoding="utf-8") as source:
        for row_number, row in enumerate(csv.DictReader(source), start=2):
            pts_text = row.get("pts_seconds")
            records.append({
                "frame": int(row["frame_index"]),
                "dx": finite_float(row["dx"], "dx", row_number),
                "dy": finite_float(row["dy"], "dy", row_number),
                "confidence": finite_float(
                    row.get("confidence", 1.0), "confidence", row_number),
                "residual": finite_float(
                    row.get("residual_median", 0.0), "residual", row_number),
                "pts": None if pts_text in (None, "") or
                pts_text.lower() == "nan" else finite_float(
                    pts_text, "pts_seconds", row_number),
            })
    return records


def read_reference(path):
    lines = Path(path).read_text(encoding="utf-8").splitlines()
    content = [line.strip() for line in lines
               if line.strip() and not line.lstrip().startswith("#")]
    if not content:
        return []
    if "," in content[0]:
        return read_codec_csv(path)
    records = []
    for frame, line in enumerate(content):
        fields = line.split()
        if len(fields) < 3:
            raise ValueError(f"invalid transform line {frame + 1}: {line}")
        records.append({"frame": frame,
                        "dx": finite_float(fields[1], "dx", frame + 1),
                        "dy": finite_float(fields[2], "dy", frame + 1),
                        "confidence": 1.0, "residual": 0.0, "pts": None})
    return records


def percentile(values, percent):
    if not values:
        return math.nan
    ordered = sorted(values)
    index = (len(ordered) - 1) * percent / 100.0
    lower = math.floor(index)
    upper = math.ceil(index)
    fraction = index - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def pearson(left, right):
    if len(left) < 2:
        return math.nan
    left_mean = sum(left) / len(left)
    right_mean = sum(right) / len(right)
    covariance = sum((a - left_mean) * (b - right_mean)
                     for a, b in zip(left, right))
    left_energy = sum((a - left_mean) ** 2 for a in left)
    right_energy = sum((b - right_mean) ** 2 for b in right)
    denominator = math.sqrt(left_energy * right_energy)
    return covariance / denominator if denominator else math.nan


def align_records(codec, reference):
    def indexed(records, label):
        result = {}
        for item in records:
            frame = item["frame"]
            if frame in result:
                raise ValueError(f"duplicate {label} frame {frame}")
            result[frame] = item
        return result

    codec_by_frame = indexed(codec, "codec")
    reference_by_frame = indexed(reference, "reference")
    common = sorted(codec_by_frame.keys() & reference_by_frame.keys())
    return ([codec_by_frame[frame] for frame in common],
            [reference_by_frame[frame] for frame in common],
            len(codec_by_frame) - len(common),
            len(reference_by_frame) - len(common))


def sample_times(records):
    pts = [item.get("pts") for item in records]
    if all(value is not None and math.isfinite(value) for value in pts) and all(
            current > previous for previous, current in zip(pts, pts[1:])):
        return pts
    return [float(item["frame"]) for item in records]


def difference_rates(values, times):
    rates = []
    rate_times = []
    for index in range(1, len(values)):
        elapsed = times[index] - times[index - 1]
        if elapsed <= 0.0 or not math.isfinite(elapsed):
            continue
        rates.append((values[index] - values[index - 1]) / elapsed)
        rate_times.append((times[index] + times[index - 1]) / 2.0)
    return rates, rate_times


def radix2_fft(values):
    result = [complex(value) for value in values]
    count = len(result)
    target = 0
    for index in range(1, count):
        bit = count >> 1
        while target & bit:
            target ^= bit
            bit >>= 1
        target ^= bit
        if index < target:
            result[index], result[target] = result[target], result[index]
    width = 2
    while width <= count:
        root = complex(math.cos(-2.0 * math.pi / width),
                       math.sin(-2.0 * math.pi / width))
        for start in range(0, count, width):
            factor = 1.0 + 0.0j
            for offset in range(width // 2):
                even = result[start + offset]
                odd = factor * result[start + offset + width // 2]
                result[start + offset] = even + odd
                result[start + offset + width // 2] = even - odd
                factor *= root
        width *= 2
    return result


def spectral_band_energy(dx_values, dy_values, low=0.1, high=0.5):
    sample_count = len(dx_values)
    if sample_count < 2:
        return math.nan
    fft_count = 1 << (sample_count - 1).bit_length()
    dx_mean = sum(dx_values) / sample_count
    dy_mean = sum(dy_values) / sample_count
    dx_fft = radix2_fft([value - dx_mean for value in dx_values] +
                        [0.0] * (fft_count - sample_count))
    dy_fft = radix2_fft([value - dy_mean for value in dy_values] +
                        [0.0] * (fft_count - sample_count))
    energy = 0.0
    for frequency_bin in range(1, fft_count // 2 + 1):
        frequency = frequency_bin / fft_count
        if low <= frequency <= high:
            multiplier = 1.0 if frequency_bin == fft_count // 2 else 2.0
            energy += multiplier * (abs(dx_fft[frequency_bin]) ** 2 +
                                    abs(dy_fft[frequency_bin]) ** 2)
    return energy / (fft_count * sample_count)


def validate_records(records, label):
    for index, item in enumerate(records):
        for field in ("dx", "dy", "confidence", "residual"):
            value = item.get(field, 1.0 if field == "confidence" else 0.0)
            if not math.isfinite(value):
                raise ValueError(f"non-finite {label} {field} at record {index}")
        pts = item.get("pts")
        if pts is not None and not math.isfinite(pts):
            raise ValueError(f"non-finite {label} pts at record {index}")


def error_metrics(codec, reference, sign):
    dx_error = [sign * item["dx"] - target["dx"]
                for item, target in zip(codec, reference)]
    dy_error = [sign * item["dy"] - target["dy"]
                for item, target in zip(codec, reference)]
    magnitudes = [math.hypot(dx, dy) for dx, dy in zip(dx_error, dy_error)]
    squared = [value * value for value in magnitudes]
    times = sample_times(codec)
    dx_velocity, velocity_times = difference_rates(dx_error, times)
    dy_velocity, _ = difference_rates(dy_error, times)
    dx_acceleration, _ = difference_rates(dx_velocity, velocity_times)
    dy_acceleration, _ = difference_rates(dy_velocity, velocity_times)
    derivative = [math.hypot(dx, dy)
                  for dx, dy in zip(dx_velocity, dy_velocity)]
    acceleration = [math.hypot(dx, dy)
                    for dx, dy in zip(dx_acceleration, dy_acceleration)]
    return {
        "median_absolute_error": percentile(magnitudes, 50),
        "rmse": math.sqrt(sum(squared) / len(squared)) if squared else math.nan,
        "p90": percentile(magnitudes, 90),
        "p95": percentile(magnitudes, 95),
        "p99": percentile(magnitudes, 99),
        "derivative_rmse": math.sqrt(sum(x * x for x in derivative) /
                                     len(derivative)) if derivative else math.nan,
        "acceleration_rmse": math.sqrt(sum(x * x for x in acceleration) /
                                       len(acceleration)) if acceleration else math.nan,
        "jitter_band_energy": spectral_band_energy(dx_error, dy_error),
    }


def compare(codec, reference):
    validate_records(codec, "codec")
    validate_records(reference, "reference")
    codec, reference, unmatched_codec, unmatched_reference = align_records(
        codec, reference)
    count = len(codec)
    if count == 0:
        raise ValueError("no overlapping motion records")
    positive = error_metrics(codec, reference, 1.0)
    negative = error_metrics(codec, reference, -1.0)
    sign = 1.0 if positive["rmse"] <= negative["rmse"] else -1.0
    result = error_metrics(codec, reference, sign)
    result["sign"] = sign
    result["count"] = count
    result["unmatched_codec"] = unmatched_codec
    result["unmatched_reference"] = unmatched_reference
    result["pearson_dx"] = pearson(
        [sign * item["dx"] for item in codec], [item["dx"] for item in reference])
    result["pearson_dy"] = pearson(
        [sign * item["dy"] for item in codec], [item["dy"] for item in reference])
    result["confidence"] = {}
    for threshold in (0.5, 0.8, 0.95):
        selected = [index for index, item in enumerate(codec)
                    if item["confidence"] > threshold]
        subset_codec = [codec[index] for index in selected]
        subset_reference = [reference[index] for index in selected]
        result["confidence"][threshold] = (
            error_metrics(subset_codec, subset_reference, sign)
            if selected else None)
    return result


def print_report(result):
    print(f"frames: {result['count']}")
    print(f"unmatched codec frames: {result['unmatched_codec']}")
    print(f"unmatched reference frames: {result['unmatched_reference']}")
    print(f"aligned codec sign: {result['sign']:+.0f}")
    print(f"pearson dx: {result['pearson_dx']:.6f}")
    print(f"pearson dy: {result['pearson_dy']:.6f}")
    for name in ("median_absolute_error", "rmse", "p90", "p95", "p99",
                 "derivative_rmse", "acceleration_rmse", "jitter_band_energy"):
        print(f"{name}: {result[name]:.6f}")
    for threshold, metrics in result["confidence"].items():
        value = "no frames" if metrics is None else f"rmse={metrics['rmse']:.6f}"
        print(f"confidence > {threshold:.2f}: {value}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--codec", required=True, help="mvstab stats CSV")
    parser.add_argument("--vidstab", required=True,
                        help="vid.stab global transform or comparable CSV")
    arguments = parser.parse_args()
    result = compare(read_codec_csv(arguments.codec),
                     read_reference(arguments.vidstab))
    print_report(result)


if __name__ == "__main__":
    main()
