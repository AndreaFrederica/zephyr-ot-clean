#!/usr/bin/env python3
"""
改进版ADC校准工具
- 增加按回车继续的暂停，给用户时间改电压源
- 增加更好的错误处理和重试机制
- 显示当前已采集的点数
"""
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


def fetch_status(base_url: str, retries: int = 3) -> dict:
    """获取设备状态，带重试"""
    url = f"{base_url.rstrip('/')}/api/status"
    last_error = None
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(url, timeout=5) as response:
                return json.loads(response.read().decode("utf-8"))
        except (urllib.error.URLError, TimeoutError) as e:
            last_error = e
            if attempt < retries - 1:
                print(f"  请求失败，{attempt + 1}/{retries} 次重试...")
                time.sleep(0.5)
    raise last_error


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
        print(f"    采样 {i+1}/{sample_count}: adc_raw={fan['adc_raw']}, adc_mv={fan['adc_mv']}mV")
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
        description="ADC校准工具 - 改进版（增加暂停，方便改电压源）"
    )
    parser.add_argument("--base-url", default="http://192.168.4.1", help="设备URL")
    parser.add_argument("--fan", type=int, default=1, choices=[1, 2], help="风扇通道")
    parser.add_argument("--samples", type=int, default=8, help="每点采样次数")
    parser.add_argument("--interval", type=float, default=0.15, help="采样间隔(秒)")
    parser.add_argument("--output", default="adc_to_voltage.generated.json", help="输出文件")
    parser.add_argument("--no-pause", action="store_true", help="禁用采集后的暂停（旧版行为）")
    args = parser.parse_args()

    fan_index = args.fan - 1
    samples: list[Sample] = []

    print("=" * 50)
    print(f"ADC校准工具 - 目标设备: {args.base_url}")
    print(f"使用风扇通道: {args.fan}")
    print("=" * 50)
    print("\n操作步骤:")
    print("1. 设置好电压源，用万用表确认实际电压")
    print("2. 输入实际电压值（如: 3.3, 3.3v, 3300mv, 12v）")
    print("3. 程序会自动采集ADC数据")
    print("4. 按回车键暂停，修改电压源")
    print("5. 重复步骤1-4，采集至少2个点（建议5-10个）")
    print("6. 输入空行（直接回车）结束校准\n")

    while True:
        print(f"\n【已采集 {len(samples)} 个点】")
        
        # 输入电压值
        try:
            user_input = input("> 输入实际电压 (或直接回车结束): ").strip()
        except EOFError:
            print("\n检测到EOF，结束校准")
            break

        if not user_input:
            if len(samples) < 2:
                print("需要至少2个校准点才能生成曲线！")
                continue
            print("结束校准")
            break

        try:
            voltage_mv = parse_voltage_to_mv(user_input)
        except ValueError:
            print("  错误: 电压格式无效。请用如: 3.3v, 3300mv, 12v")
            continue

        # 采集数据
        print(f"  开始采集 {args.samples} 次样本...")
        try:
            averaged = collect_average_sample(args.base_url, fan_index, args.samples, args.interval)
        except (urllib.error.URLError, TimeoutError, KeyError, ValueError) as exc:
            print(f"  错误: 获取ADC状态失败: {exc}")
            print(f"  请检查设备是否在线，然后重试")
            continue

        averaged.voltage_mv = voltage_mv
        samples.append(averaged)
        
        print(f"  ✓ 已记录: adc_raw={averaged.adc_raw} -> {voltage_mv}mV")
        print(f"    (当前曲线映射值: {averaged.mapped_voltage_mv}mV, ADC理论值: {averaged.adc_mv}mV)")

        # 关键改进：暂停，给用户时间改电压源
        if not args.no_pause:
            print("\n" + "-" * 30)
            try:
                input("[按回车键继续下一个点，或修改电压源后再按回车...]")
            except EOFError:
                pass

    # 生成曲线文件
    if len(samples) < 2:
        print("\n错误: 至少需要2个校准点！")
        return 1

    points = normalize_points(samples)
    curve = build_curve_json(points)
    output_path = Path(args.output)
    output_path.write_text(json.dumps(curve, indent=2) + "\n", encoding="utf-8")

    print("\n" + "=" * 50)
    print(f"✓ 已生成曲线文件: {output_path}")
    print(f"  共 {len(points)} 个校准点")
    print("\n下一步操作:")
    print(f"1. 检查文件内容: {output_path}")
    print(f"2. 上传到设备: /etc/fanctl/curves/adc_to_voltage.json")
    print(f"3. 重启设备或重新加载配置")
    print("=" * 50)
    
    # 打印预览
    print("\n生成的曲线预览:")
    for p in points:
        print(f"  adc_raw={p[0]:4d} -> {p[1]:5d}mV")
    
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
