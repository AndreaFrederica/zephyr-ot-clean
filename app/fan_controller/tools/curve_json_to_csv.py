#!/usr/bin/env python3
"""
将ADC校准曲线JSON转换为CSV，方便用Excel/Python画图分析
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def json_to_csv(input_path: str, output_path: str | None = None) -> None:
    """将JSON曲线转换为CSV"""
    input_file = Path(input_path)
    
    if not input_file.exists():
        print(f"错误: 文件不存在: {input_path}", file=sys.stderr)
        sys.exit(1)
    
    # 读取JSON
    with open(input_file, 'r', encoding='utf-8') as f:
        data = json.load(f)
    
    points = data.get('points', [])
    if not points:
        print("错误: JSON中没有points数据", file=sys.stderr)
        sys.exit(1)
    
    # 确定输出路径
    if output_path is None:
        output_file = input_file.with_suffix('.csv')
    else:
        output_file = Path(output_path)
    
    # 计算一些辅助列
    # 1. 每mV对应的ADC值变化（斜率）
    # 2. 与前一点的差值
    
    lines = []
    lines.append("adc_raw,voltage_mv,step_adc,step_mv,slope_adc_per_v,note")
    
    for i, (adc_raw, voltage_mv) in enumerate(points):
        step_adc = ""
        step_mv = ""
        slope = ""
        note = ""
        
        if i > 0:
            prev_adc, prev_mv = points[i-1]
            d_adc = adc_raw - prev_adc
            d_mv = voltage_mv - prev_mv
            step_adc = str(d_adc)
            step_mv = str(d_mv)
            
            if d_mv > 0:
                # 每伏特多少ADC值
                slope = f"{d_adc * 1000 / d_mv:.1f}"
            
            # 线性插值检查
            if i >= 2:
                # 计算与前一个线段的斜率差异
                prev_slope = (prev_adc - points[i-2][0]) / (prev_mv - points[i-2][1]) * 1000 if (prev_mv - points[i-2][1]) > 0 else 0
                curr_slope = d_adc / d_mv * 1000 if d_mv > 0 else 0
                slope_diff = abs(curr_slope - prev_slope)
                if slope_diff > 50:  # 斜率变化超过50认为异常
                    note = f"斜率变化大({slope_diff:.0f})"
        else:
            note = "起点"
        
        lines.append(f"{adc_raw},{voltage_mv},{step_adc},{step_mv},{slope},{note}")
    
    # 写入CSV
    output_file.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    
    print(f"✓ 已转换: {input_file} -> {output_file}")
    print(f"  共 {len(points)} 个点")
    
    # 打印统计信息
    if len(points) >= 2:
        first_adc, first_mv = points[0]
        last_adc, last_mv = points[-1]
        
        print(f"\n统计信息:")
        print(f"  ADC范围: {first_adc} ~ {last_adc} ({last_adc - first_adc})")
        print(f"  电压范围: {first_mv}mV ~ {last_mv}mV ({(last_mv - first_mv)/1000:.2f}V)")
        
        if last_mv > first_mv:
            avg_slope = (last_adc - first_adc) / ((last_mv - first_mv) / 1000)
            print(f"  平均斜率: {avg_slope:.1f} ADC值/伏特")
            print(f"  平均分辨率: {1000/avg_slope:.1f} mV/ADC")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="将ADC校准曲线JSON转换为CSV"
    )
    parser.add_argument("input", help="输入JSON文件路径")
    parser.add_argument("-o", "--output", help="输出CSV文件路径（默认与输入同名）")
    args = parser.parse_args()
    
    json_to_csv(args.input, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
