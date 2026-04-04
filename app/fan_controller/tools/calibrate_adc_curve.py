#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Sample:
    voltage_mv: int
    adc_raw: int
    adc_mv: int
    mapped_voltage_mv: int


def fetch_status(base_url: str) -> dict:
    url = f"{base_url.rstrip('/')}/api/status"
    with urllib.request.urlopen(url, timeout=5) as response:
        return json.loads(response.read().decode("utf-8"))


def parse_voltage_to_mv(text: str) -> int:
    raw = text.strip().lower()
    if raw.endswith("mv"):
        return int(float(raw[:-2].strip()))
    if raw.endswith("v"):
        return int(round(float(raw[:-1].strip()) * 1000.0))
    value = float(raw)
    if value <= 24.0:
        return int(round(value * 1000.0))
    return int(round(value))


def collect_average_sample(base_url: str, fan_index: int, sample_count: int, interval_s: float) -> Sample:
    raw_values: list[int] = []
    adc_mv_values: list[int] = []
    mapped_mv_values: list[int] = []

    for i in range(sample_count):
        status = fetch_status(base_url)
        fan = status["fans"][fan_index]
        raw_values.append(int(fan["adc_raw"]))
        adc_mv_values.append(int(fan["adc_mv"]))
        mapped_mv_values.append(int(fan["mapped_voltage_mv"]))
        if i + 1 < sample_count:
            time.sleep(interval_s)

    return Sample(
        voltage_mv=0,
        adc_raw=int(round(statistics.mean(raw_values))),
        adc_mv=int(round(statistics.mean(adc_mv_values))),
        mapped_voltage_mv=int(round(statistics.mean(mapped_mv_values))),
    )


def normalize_points(samples: list[Sample]) -> list[list[int]]:
    grouped: dict[int, list[int]] = {}
    for item in samples:
        grouped.setdefault(item.adc_raw, []).append(item.voltage_mv)

    points = [[adc_raw, int(round(statistics.mean(volts)))] for adc_raw, volts in grouped.items()]
    points.sort(key=lambda pair: pair[0])
    return points


def build_curve_json(points: list[list[int]]) -> dict:
    return {
        "kind": "adc_raw_to_voltage",
        "fit": "piecewise_linear",
        "input_unit": "adc_raw",
        "output_unit": "mv",
        "points": points,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Collect ADC calibration samples from fan controller and generate adc_to_voltage.json."
    )
    parser.add_argument("--base-url", default="http://192.168.4.1", help="Device base URL")
    parser.add_argument("--fan", type=int, default=1, choices=[1, 2], help="Fan channel to sample")
    parser.add_argument("--samples", type=int, default=8, help="Number of samples to average per point")
    parser.add_argument("--interval", type=float, default=0.15, help="Seconds between samples")
    parser.add_argument(
        "--output",
        default="adc_to_voltage.generated.json",
        help="Output curve JSON file",
    )
    args = parser.parse_args()

    fan_index = args.fan - 1
    samples: list[Sample] = []

    print(f"Target device: {args.base_url}")
    print(f"Sampling fan channel: {args.fan}")
    print("Input voltage values like: 3.3, 3300, 12v, 5000mv")
    print("Press Enter on an empty line to finish.\n")

    while True:
        try:
            user_input = input("Actual voltage> ").strip()
        except EOFError:
            print()
            break

        if not user_input:
            break

        try:
            voltage_mv = parse_voltage_to_mv(user_input)
        except ValueError:
            print("Invalid voltage format.")
            continue

        print("Sampling ADC...")
        try:
            averaged = collect_average_sample(args.base_url, fan_index, args.samples, args.interval)
        except (urllib.error.URLError, TimeoutError, KeyError, ValueError) as exc:
            print(f"Failed to fetch ADC status: {exc}")
            return 1

        averaged.voltage_mv = voltage_mv
        samples.append(averaged)
        print(
            f"  captured adc_raw={averaged.adc_raw} adc_mv={averaged.adc_mv} "
            f"current_curve_voltage={averaged.mapped_voltage_mv} target_voltage={voltage_mv}"
        )

    if len(samples) < 2:
        print("Need at least 2 calibration points.")
        return 1

    points = normalize_points(samples)
    curve = build_curve_json(points)
    output_path = Path(args.output)
    output_path.write_text(json.dumps(curve, indent=2) + "\n", encoding="utf-8")

    print(f"\nWrote {output_path}")
    print("Recommended next step:")
    print(f"  1. Review the points in {output_path}")
    print("  2. Copy it to /etc/fanctl/curves/adc_to_voltage.json through the web file editor or serial shell")
    print("  3. Reboot or reload the fan controller")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
