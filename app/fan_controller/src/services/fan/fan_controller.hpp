/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_FAN_CONTROLLER_HPP_
#define FAN_CONTROLLER_FAN_CONTROLLER_HPP_

#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/adc/voltage_divider.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "core/common.hpp"
#include "curve_profiles.hpp"
#include "fan_shared_state.hpp"

namespace fanctl {

/**
 * @brief 100Hz 风扇控制器
 * 
 * 控制循环以 100Hz 频率运行，执行：
 * - ADC 采样
 * - RPM 计算
 * - PWM 更新
 * - 状态 LED 控制
 * 
 * 所有状态数据写入共享内存，其他任务通过共享内存读取
 */
class FanController {
public:
	static constexpr int kControlLoopStackSize = 4096;
	static constexpr int kControlLoopPriority = 2;  // 高优先级实时任务
	
	FanController();

	// 初始化和生命周期
	void InitRuntime();
	int InitHardware();
	void LoadPersistedState();
	void StartControlLoop();
	void StopControlLoop();

	// 控制命令 (写入命令队列，由控制循环执行)
	int SetFan(size_t index, uint8_t percent, bool enabled, bool persist);
	int SetAdcTargetMode(size_t index, bool use_adc_target, bool persist);
	int ConfigureFan(size_t index, uint8_t percent, bool enabled, bool use_adc_target, bool persist);
	int ConfigureFanTargetRpm(size_t index, int32_t target_rpm, bool enabled, bool persist);
	int SetPwmConfig(size_t index, bool inverted, uint8_t min_percent, uint8_t max_percent, bool persist);

	// 数据读取 (从共享内存读取，无锁)
	void GetState(size_t index, FanState *state) const;
	void GetAllStates(FanState states[kFanCount]) const;
	const FanControllerSharedState *GetSharedState() const { return &shared_state_; }

	// 曲线配置
	const curves::CurveProfiles &GetCurves() const { return curves_; }
	curves::CurveProfiles &GetCurves() { return curves_; }

	// WiFi 状态同步 (由外部任务调用，原子写入)
	void SetWifiStatus(bool sta_connected, bool ap_enabled) {
		atomic_set(&sta_connected_, sta_connected ? 1 : 0);
		atomic_set(&ap_enabled_, ap_enabled ? 1 : 0);
	}

private:
	// 硬件通道配置
	struct Channel {
		const char *name;
		struct pwm_dt_spec pwm;
		struct gpio_dt_spec power;
		struct voltage_divider_dt_spec sense;
		struct gpio_dt_spec tach;
		struct gpio_callback tach_cb;
		uint8_t index;  // 通道索引，用于 ISR
	};

	// 命令类型
	enum class CommandType : uint8_t {
		SetFan,
		SetAdcTargetMode,
		SetPwmConfig,
	};

	// 命令结构
	struct Command {
		CommandType type;
		uint8_t index;
		uint8_t percent;
		uint8_t enabled;
		uint8_t use_adc_target;
		bool persist;
		// PWM配置专用字段
		uint8_t pwm_min_percent;
		uint8_t pwm_max_percent;
		bool pwm_inverted;
	};

	// 100Hz 控制循环
	static void ControlLoopThread(void *arg1, void *arg2, void *arg3);
	void RunControlLoop();
	
	// 控制循环内部方法
	void ProcessCommands();
	void UpdateAllChannels();
	void UpdateChannel(size_t index);
	int ReadAdcLocked(size_t index, int32_t *raw, int32_t *mv);
	void CalculateRpmLocked(size_t index, int64_t now_ms, int64_t delta_ms);
	void UpdateDerivedStateLocked(size_t index);
	int ApplyPwmLocked(size_t index);
	void UpdateStatusLedLocked(bool sta_connected, bool ap_enabled);

	// 静态回调
	static void TachCallback(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins);

	// 硬件配置 (只读，初始化后不变)
	Channel channels_[kFanCount];
	struct gpio_dt_spec status_led_;
	
	// 运行时状态 (仅控制循环访问，无需锁)
	struct RuntimeState {
		bool enabled;
		bool use_adc_target;
		uint8_t percent;
		uint8_t effective_percent;
		uint8_t pwm_percent;
		// PWM配置
		bool pwm_inverted;
		uint8_t pwm_min_percent;
		uint8_t pwm_max_percent;
	} runtime_state_[kFanCount];
	
	// 共享内存状态 (控制循环写入，其他任务读取)
	FanControllerSharedState shared_state_;
	
	// 曲线配置
	curves::CurveProfiles curves_;
	
	// 控制循环线程
	struct k_thread control_thread_;
	
	// 命令队列
	static constexpr int kCommandQueueSize = 16;
	struct k_msgq command_queue_;
	char command_queue_buffer_[kCommandQueueSize * sizeof(Command)];
	
	// 同步原语
	struct k_mutex hardware_mutex_;  // 保护硬件访问
	
	// 控制循环控制
	atomic_t control_loop_running_;
	atomic_t sta_connected_;
	atomic_t ap_enabled_;
	
	// 状态 LED
	bool blink_phase_;
	
	// 线程栈 (使用静态分配)
	k_thread_stack_t *control_stack_ptr_;
};

} // namespace fanctl

#endif // FAN_CONTROLLER_FAN_CONTROLLER_HPP_
