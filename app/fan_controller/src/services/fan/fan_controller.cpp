/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fan_controller.hpp"

#include <errno.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

#include "settings_store.hpp"

LOG_MODULE_REGISTER(fan_controller, LOG_LEVEL_INF);

namespace fanctl {

namespace {

// 全局 FanController 实例指针 (用于 ISR)
static FanController *g_ctrl_instance = nullptr;

const struct pwm_dt_spec kFanPwms[kFanCount] = {
	PWM_DT_SPEC_GET(DT_ALIAS(fan_pwm0)),
	PWM_DT_SPEC_GET(DT_ALIAS(fan_pwm1)),
};

const struct gpio_dt_spec kFanPowers[kFanCount] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(fan_power0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(fan_power1), gpios),
};

const struct voltage_divider_dt_spec kFanSenses[kFanCount] = {
	VOLTAGE_DIVIDER_DT_SPEC_GET(DT_ALIAS(fan_adc0)),
	VOLTAGE_DIVIDER_DT_SPEC_GET(DT_ALIAS(fan_adc1)),
};

const struct gpio_dt_spec kFanTachs[kFanCount] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(fan_tach0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(fan_tach1), gpios),
};

const struct gpio_dt_spec kStatusLed = GPIO_DT_SPEC_GET(DT_ALIAS(status_led), gpios);
const char *const kFanNames[kFanCount] = { "fan1", "fan2" };
constexpr uint32_t kTachPulsesPerRevolution = 2U;

// 100Hz = 10ms 周期
constexpr uint32_t kControlLoopPeriodMs = 10;

// 静态脉冲计数器 (ISR 安全)
static atomic_t g_tach_edges[kFanCount] = { ATOMIC_INIT(0), ATOMIC_INIT(0) };

} // namespace

FanController::FanController()
	: channels_{ { kFanNames[0], kFanPwms[0], kFanPowers[0], kFanSenses[0], kFanTachs[0], {}, 0 },
		     { kFanNames[1], kFanPwms[1], kFanPowers[1], kFanSenses[1], kFanTachs[1], {}, 1 } },
	  status_led_(kStatusLed),
	  blink_phase_(false),
	  control_stack_ptr_(nullptr)
{
	for (size_t i = 0; i < kFanCount; ++i) {
		runtime_state_[i] = { true, false, 40, 40, 40 };
	}
	atomic_set(&control_loop_running_, 0);
	atomic_set(&sta_connected_, 0);
	atomic_set(&ap_enabled_, 0);
	g_ctrl_instance = this;
}

void FanController::InitRuntime()
{
	FanSharedStateInit(&shared_state_);
	k_mutex_init(&hardware_mutex_);
	k_msgq_init(&command_queue_, command_queue_buffer_, sizeof(Command), kCommandQueueSize);
}

void FanController::TachCallback(const struct device *port, struct gpio_callback *cb,
				 gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	Channel *channel = CONTAINER_OF(cb, Channel, tach_cb);
	// 直接使用通道索引
	if (channel->index < kFanCount) {
		atomic_inc(&g_tach_edges[channel->index]);
	}
}

int FanController::InitHardware()
{
	int rc = curves_.Init();
	if (rc != 0) {
		LOG_ERR("Failed to init curve profiles: %d", rc);
		return rc;
	}

	if (!gpio_is_ready_dt(&status_led_)) {
		LOG_ERR("Status LED GPIO not ready");
		return -ENODEV;
	}

	rc = gpio_pin_configure_dt(&status_led_, GPIO_OUTPUT_INACTIVE);
	if (rc != 0) {
		LOG_ERR("Failed to configure status LED: %d", rc);
		return rc;
	}

	for (size_t i = 0; i < kFanCount; ++i) {
		if (!pwm_is_ready_dt(&channels_[i].pwm) || !gpio_is_ready_dt(&channels_[i].power) ||
		    !adc_is_ready_dt(&channels_[i].sense.port) || !gpio_is_ready_dt(&channels_[i].tach)) {
			LOG_ERR("Hardware not ready for fan %zu", i);
			return -ENODEV;
		}

		rc = gpio_pin_configure_dt(&channels_[i].power, GPIO_OUTPUT_INACTIVE);
		if (rc != 0) {
			LOG_ERR("Failed to configure power GPIO for fan %zu: %d", i, rc);
			return rc;
		}

		rc = adc_channel_setup_dt(&channels_[i].sense.port);
		if (rc != 0) {
			LOG_ERR("Failed to setup ADC for fan %zu: %d", i, rc);
			return rc;
		}

		rc = gpio_pin_configure_dt(&channels_[i].tach, GPIO_INPUT);
		if (rc != 0) {
			LOG_ERR("Failed to configure tach GPIO for fan %zu: %d", i, rc);
			return rc;
		}

		gpio_init_callback(&channels_[i].tach_cb, TachCallback, BIT(channels_[i].tach.pin));
		rc = gpio_add_callback(channels_[i].tach.port, &channels_[i].tach_cb);
		if (rc != 0) {
			LOG_ERR("Failed to add tach callback for fan %zu: %d", i, rc);
			return rc;
		}

		rc = gpio_pin_interrupt_configure_dt(&channels_[i].tach, GPIO_INT_EDGE_TO_ACTIVE);
		if (rc != 0) {
			LOG_ERR("Failed to configure tach interrupt for fan %zu: %d", i, rc);
			return rc;
		}
	}

	// 初始状态应用
	for (size_t i = 0; i < kFanCount; ++i) {
		UpdateDerivedStateLocked(i);
		rc = ApplyPwmLocked(i);
		if (rc != 0) {
			LOG_ERR("Failed to apply initial PWM for fan %zu: %d", i, rc);
			return rc;
		}
	}

	shared_state_.last_sample_time_ms = k_uptime_get();
	LOG_INF("Fan hardware initialized");
	return 0;
}

void FanController::LoadPersistedState()
{
	settings::AppConfig config = {};
	bool loaded = settings::LoadConfig(&config) == 0;

	for (size_t i = 0; i < kFanCount; ++i) {
		if (loaded) {
			runtime_state_[i].enabled = config.fan_enabled[i];
			runtime_state_[i].percent = MIN(config.fan_percent[i], 100U);
			runtime_state_[i].use_adc_target = config.fan_use_adc_target[i];
		}
		// 同步到共享内存
		FanSharedStateWriteConfig(&shared_state_.channels[i], runtime_state_[i].use_adc_target);
	}

	LOG_INF("Loaded persisted state (loaded=%s)", loaded ? "yes" : "no");
}

// 控制循环线程栈 (静态分配)
static K_THREAD_STACK_DEFINE(s_control_stack, fanctl::FanController::kControlLoopStackSize);

// 静态 FanController 实例指针定义 (用于 ISR)
// 已在匿名命名空间中声明

void FanController::StartControlLoop()
{
	atomic_set(&control_loop_running_, 1);
	atomic_set(&shared_state_.control_loop_running, 1);
	control_stack_ptr_ = s_control_stack;

	k_thread_create(&control_thread_, control_stack_ptr_,
			K_THREAD_STACK_SIZEOF(s_control_stack),
			ControlLoopThread, this, nullptr, nullptr,
			kControlLoopPriority, 0, K_NO_WAIT);
	k_thread_name_set(&control_thread_, "fanctl_ctrl");

	LOG_INF("Control loop started at 100Hz");
}

void FanController::StopControlLoop()
{
	atomic_set(&control_loop_running_, 0);
	k_thread_join(&control_thread_, K_FOREVER);
	atomic_set(&shared_state_.control_loop_running, 0);
	LOG_INF("Control loop stopped");
}

void FanController::ControlLoopThread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);
	FanController *self = static_cast<FanController *>(arg1);
	self->RunControlLoop();
}

void FanController::RunControlLoop()
{
	int64_t next_wakeup = k_uptime_get();
	uint32_t loop_counter = 0;

	while (atomic_get(&control_loop_running_)) {
		int64_t loop_start = k_uptime_get();

		// 1. 处理命令队列
		ProcessCommands();

		// 2. 更新所有通道 (ADC, RPM, PWM)
		UpdateAllChannels();

		// 3. 更新状态 LED
		bool sta = atomic_get(&sta_connected_) != 0;
		bool ap = atomic_get(&ap_enabled_) != 0;
		UpdateStatusLedLocked(sta, ap);

		// 4. 更新循环计数器和时序统计
		loop_counter++;
		atomic_set(&shared_state_.loop_counter, static_cast<atomic_val_t>(loop_counter));

		int64_t loop_end = k_uptime_get();
		int32_t loop_duration_us = static_cast<int32_t>((loop_end - loop_start) * 1000);
		atomic_set(&shared_state_.last_loop_duration_us, loop_duration_us);

		static int32_t max_duration = 0;
		if (loop_duration_us > max_duration) {
			max_duration = loop_duration_us;
			atomic_set(&shared_state_.max_loop_duration_us, max_duration);
		}

		// 精确 100Hz 定时
		next_wakeup += kControlLoopPeriodMs;
		int64_t sleep_ms = next_wakeup - k_uptime_get();
		if (sleep_ms > 0) {
			k_sleep(K_MSEC(sleep_ms));
		} else {
			// 如果执行超时，记录警告并重新同步
			if (sleep_ms < -kControlLoopPeriodMs) {
				LOG_WRN("Control loop overrun: %lld ms", -sleep_ms);
				next_wakeup = k_uptime_get() + kControlLoopPeriodMs;
			}
		}
	}
}

void FanController::ProcessCommands()
{
	Command cmd;
	while (k_msgq_get(&command_queue_, &cmd, K_NO_WAIT) == 0) {
		if (cmd.index >= kFanCount) {
			continue;
		}

		switch (cmd.type) {
		case CommandType::SetFan:
			runtime_state_[cmd.index].enabled = cmd.enabled != 0;
			runtime_state_[cmd.index].percent = cmd.percent;
			if (cmd.persist) {
				settings::SaveFanState(cmd.index, cmd.enabled != 0, cmd.percent);
			}
			break;

		case CommandType::SetAdcTargetMode:
			runtime_state_[cmd.index].use_adc_target = cmd.use_adc_target != 0;
			FanSharedStateWriteConfig(&shared_state_.channels[cmd.index],
						  runtime_state_[cmd.index].use_adc_target);
			if (cmd.persist) {
				settings::SaveFanAdcTargetMode(cmd.index, cmd.use_adc_target != 0);
			}
			break;
		}
	}
}

void FanController::UpdateAllChannels()
{
	int64_t now_ms = k_uptime_get();
	int64_t delta_ms = now_ms - shared_state_.last_sample_time_ms;

	// 限制 delta_ms 在合理范围内 (用于 RPM 计算)
	if (delta_ms <= 0 || delta_ms > 1000) {
		delta_ms = kControlLoopPeriodMs;
	}

	for (size_t i = 0; i < kFanCount; ++i) {
		// 读取 ADC
		int32_t raw = 0, mv = 0;
		if (ReadAdcLocked(i, &raw, &mv) == 0) {
			FanSharedStateWriteAdc(&shared_state_.channels[i], raw, mv, 
					       curves_.EvaluateAdcRawToVoltage(raw));
		}

		// 计算 RPM
		CalculateRpmLocked(i, now_ms, delta_ms);

		// 更新派生状态
		UpdateDerivedStateLocked(i);

		// 应用 PWM
		ApplyPwmLocked(i);
	}

	shared_state_.last_sample_time_ms = now_ms;
}

void FanController::UpdateChannel(size_t index)
{
	if (index >= kFanCount) {
		return;
	}

	UpdateDerivedStateLocked(index);
	ApplyPwmLocked(index);
}

int FanController::ReadAdcLocked(size_t index, int32_t *raw, int32_t *mv)
{
	int16_t sample = 0;
	struct adc_sequence sequence = { .buffer = &sample, .buffer_size = sizeof(sample) };

	int rc = adc_sequence_init_dt(&channels_[index].sense.port, &sequence);
	if (rc != 0) {
		return rc;
	}

	rc = adc_read_dt(&channels_[index].sense.port, &sequence);
	if (rc != 0) {
		return rc;
	}

	if (raw != nullptr) {
		*raw = sample;
	}

	int32_t value = sample;
	rc = adc_raw_to_millivolts_dt(&channels_[index].sense.port, &value);
	if (rc != 0) {
		return rc;
	}

	rc = voltage_divider_scale_dt(&channels_[index].sense, &value);
	if (rc != 0) {
		return rc;
	}

	if (mv != nullptr) {
		*mv = value;
	}
	return 0;
}

void FanController::CalculateRpmLocked(size_t index, int64_t now_ms, int64_t delta_ms)
{
	FanChannelSharedState *ch = &shared_state_.channels[index];
	uint32_t edges = static_cast<uint32_t>(atomic_get(&g_tach_edges[index]));
	uint32_t last_edges = shared_state_.last_tach_edges[index];
	uint32_t delta_edges = edges - last_edges;
	shared_state_.last_tach_edges[index] = edges;

	bool enabled = runtime_state_[index].enabled;
	int32_t actual_rpm = 0;

	if (enabled && delta_edges > 0 && delta_ms > 0) {
		actual_rpm = static_cast<int32_t>((static_cast<uint64_t>(delta_edges) * 60000ULL) /
					  (static_cast<uint64_t>(delta_ms) * kTachPulsesPerRevolution));
	}

	int32_t target_rpm = curves_.EvaluatePercentToRpm(runtime_state_[index].effective_percent);
	uint8_t actual_percent = curves_.EvaluateRpmToPercent(actual_rpm);

	FanSharedStateWriteRpm(ch, actual_rpm, target_rpm, actual_percent);
}

void FanController::UpdateDerivedStateLocked(size_t index)
{
	RuntimeState &rt = runtime_state_[index];
	FanChannelSharedState *ch = &shared_state_.channels[index];

	// 读取当前 ADC 值
	int32_t adc_raw = static_cast<int32_t>(atomic_get(&ch->adc_raw));
	int32_t mapped_voltage = curves_.EvaluateAdcRawToVoltage(adc_raw);

	// 计算有效百分比
	if (rt.use_adc_target) {
		rt.effective_percent = curves_.EvaluateVoltageToPercent(mapped_voltage);
	} else {
		rt.effective_percent = rt.percent;
	}

	// 计算 PWM 百分比 (通过曲线转换)
	rt.pwm_percent = curves_.EvaluatePercentToPwm(rt.effective_percent);

	// 更新共享内存
	FanSharedStateWriteConfig(ch, rt.use_adc_target);
}

int FanController::ApplyPwmLocked(size_t index)
{
	RuntimeState &rt = runtime_state_[index];
	FanChannelSharedState *ch = &shared_state_.channels[index];

	uint32_t pulse = 0;
	if (rt.enabled) {
		pulse = static_cast<uint32_t>((static_cast<uint64_t>(channels_[index].pwm.period) *
					       rt.pwm_percent) / 100U);
	}

	int rc = gpio_pin_set_dt(&channels_[index].power, rt.enabled ? 1 : 0);
	if (rc != 0) {
		return rc;
	}

	rc = pwm_set_pulse_dt(&channels_[index].pwm, pulse);
	if (rc != 0) {
		return rc;
	}

	// 更新共享内存
	FanSharedStateWriteOutput(ch, rt.enabled, rt.percent, rt.effective_percent,
				  rt.pwm_percent, pulse);
	return 0;
}

void FanController::UpdateStatusLedLocked(bool sta_connected, bool ap_enabled)
{
	if (sta_connected) {
		(void)gpio_pin_set_dt(&status_led_, 1);
	} else if (ap_enabled) {
		// 10Hz 闪烁 (100Hz 循环，每 10 次翻转)
		static uint8_t blink_counter = 0;
		if (++blink_counter >= 10) {
			blink_counter = 0;
			blink_phase_ = !blink_phase_;
			(void)gpio_pin_set_dt(&status_led_, blink_phase_ ? 1 : 0);
		}
	} else {
		(void)gpio_pin_set_dt(&status_led_, 0);
	}
}

// 公共 API 实现

int FanController::SetFan(size_t index, uint8_t percent, bool enabled, bool persist)
{
	if (index >= kFanCount) {
		return -EINVAL;
	}

	Command cmd = {};
	cmd.type = CommandType::SetFan;
	cmd.index = static_cast<uint8_t>(index);
	cmd.percent = static_cast<uint8_t>(MIN(percent, 100U));
	cmd.enabled = enabled ? 1U : 0U;
	cmd.use_adc_target = 0;
	cmd.persist = persist;

	return k_msgq_put(&command_queue_, &cmd, K_MSEC(100));
}

int FanController::SetAdcTargetMode(size_t index, bool use_adc_target, bool persist)
{
	if (index >= kFanCount) {
		return -EINVAL;
	}

	Command cmd = {};
	cmd.type = CommandType::SetAdcTargetMode;
	cmd.index = static_cast<uint8_t>(index);
	cmd.percent = 0;
	cmd.enabled = 0;
	cmd.use_adc_target = use_adc_target ? 1U : 0U;
	cmd.persist = persist;

	return k_msgq_put(&command_queue_, &cmd, K_MSEC(100));
}

int FanController::ConfigureFan(size_t index, uint8_t percent, bool enabled,
				bool use_adc_target, bool persist)
{
	if (index >= kFanCount) {
		return -EINVAL;
	}

	// 发送两个命令
	int rc = SetFan(index, percent, enabled, persist);
	if (rc != 0) {
		return rc;
	}

	rc = SetAdcTargetMode(index, use_adc_target, persist);
	if (rc != 0 && persist) {
		// 回滚第一个命令的持久化？简化处理，返回错误
		return rc;
	}

	return 0;
}

int FanController::ConfigureFanTargetRpm(size_t index, int32_t target_rpm, bool enabled,
					 bool persist)
{
	if (index >= kFanCount || target_rpm < 0) {
		return -EINVAL;
	}

	uint8_t percent = curves_.EvaluateRpmToPercent(target_rpm);
	return ConfigureFan(index, percent, enabled, false, persist);
}

void FanController::GetState(size_t index, FanState *state) const
{
	FanSharedStateRead(&shared_state_, index, state);
}

void FanController::GetAllStates(FanState states[kFanCount]) const
{
	FanSharedStateReadAll(&shared_state_, states);
}

} // namespace fanctl
