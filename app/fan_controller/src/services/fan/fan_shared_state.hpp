/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_FAN_SHARED_STATE_HPP_
#define FAN_CONTROLLER_FAN_SHARED_STATE_HPP_

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <stdint.h>
#include <stdbool.h>
#include "core/common.hpp"

namespace fanctl {

/**
 * @brief 单个风扇的共享状态结构
 * 
 * 此结构用于控制循环和其他任务之间的数据共享
 * 控制循环负责更新，其他任务只读
 */
struct FanChannelSharedState {
	// 控制输出 (由控制循环写入)
	atomic_t enabled;           // 是否使能
	atomic_t percent;           // 目标百分比 (0-100)
	atomic_t effective_percent; // 实际生效百分比
	atomic_t pwm_percent;       // PWM 输出百分比 (经过曲线转换)
	atomic_t pwm_pulse_ns;      // PWM 脉冲宽度 (纳秒)
	
	// ADC 采集 (由控制循环写入)
	atomic_t adc_raw;           // ADC 原始值
	atomic_t adc_mv;            // ADC 电压 (mV)
	atomic_t mapped_voltage_mv; // 映射后的电压 (经过曲线转换)
	
	// RPM 反馈 (由控制循环写入)
	atomic_t actual_rpm;        // 实际转速
	atomic_t target_rpm;        // 目标转速
	atomic_t actual_percent;    // 根据 RPM 反算的百分比
	
	// 配置 (由控制循环读取/写入)
	atomic_t use_adc_target;    // 是否使用 ADC 目标模式
	
	// 原始脉冲计数 (由 ISR 写入，控制循环读取)
	atomic_t tach_edges;        // 脉冲边沿计数
};

/**
 * @brief 风扇控制器共享内存状态
 * 
 * 100Hz 控制循环更新此结构
 * 其他任务从此结构读取最新数据
 */
struct FanControllerSharedState {
	// 风扇通道状态
	FanChannelSharedState channels[kFanCount];
	
	// 系统状态标志
	atomic_t control_loop_running;  // 控制循环是否运行中
	atomic_t loop_counter;          // 循环计数器 (用于检测活性)
	
	// 控制循环时序统计
	atomic_t last_loop_duration_us; // 上次循环执行时间 (微秒)
	atomic_t max_loop_duration_us;  // 最大循环执行时间
	
	// 轻量级互斥锁 (用于批量读取时保证一致性)
	struct k_mutex snapshot_mutex;
	
	// 内部工作状态 (仅控制循环使用)
	uint32_t last_tach_edges[kFanCount];  // 上次的脉冲计数
	int64_t last_sample_time_ms;          // 上次采样时间
};

/**
 * @brief 读取单个风扇状态的辅助函数
 * 
 * 从共享内存原子读取所有字段，填充 FanState 结构
 * 
 * @param shared 共享状态指针
 * @param index 风扇索引
 * @param state 输出状态结构
 */
void FanSharedStateRead(const FanControllerSharedState *shared, size_t index, FanState *state);

/**
 * @brief 批量读取所有风扇状态
 * 
 * 使用互斥锁保证一致性，适合需要读取多个风扇的场景
 * 
 * @param shared 共享状态指针
 * @param states 输出状态数组
 */
void FanSharedStateReadAll(const FanControllerSharedState *shared, FanState states[kFanCount]);

/**
 * @brief 初始化共享内存状态
 * 
 * @param shared 共享状态指针
 */
void FanSharedStateInit(FanControllerSharedState *shared);

/**
 * @brief 控制循环内部使用：写入风扇输出状态
 */
void FanSharedStateWriteOutput(FanChannelSharedState *ch, bool enabled, uint8_t percent, 
                                uint8_t effective_percent, uint8_t pwm_percent, uint32_t pwm_pulse_ns);

/**
 * @brief 控制循环内部使用：写入 ADC 采集结果
 */
void FanSharedStateWriteAdc(FanChannelSharedState *ch, int32_t adc_raw, int32_t adc_mv, 
                             int32_t mapped_voltage_mv);

/**
 * @brief 控制循环内部使用：写入 RPM 反馈
 */
void FanSharedStateWriteRpm(FanChannelSharedState *ch, int32_t actual_rpm, int32_t target_rpm, 
                             uint8_t actual_percent);

/**
 * @brief 控制循环内部使用：读取配置
 */
bool FanSharedStateReadUseAdcTarget(const FanChannelSharedState *ch);

/**
 * @brief 控制循环内部使用：写入配置
 */
void FanSharedStateWriteConfig(FanChannelSharedState *ch, bool use_adc_target);

} // namespace fanctl

#endif // FAN_CONTROLLER_FAN_SHARED_STATE_HPP_
