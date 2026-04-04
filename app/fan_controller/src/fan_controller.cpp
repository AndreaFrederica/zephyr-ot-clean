/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fan_controller.hpp"

#include <errno.h>

#include <zephyr/sys/util.h>

#include "settings_store.hpp"

namespace fanctl {

namespace {

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
const char *const kFanNames[kFanCount] = {
	"fan1",
	"fan2",
};
constexpr uint32_t kTachPulsesPerRevolution = 2U;

} // namespace

FanController::FanController()
	: fans_{
		  { kFanNames[0], kFanPwms[0], kFanPowers[0], kFanSenses[0], kFanTachs[0], {}, true, false,
		    40, 40, 40, 0, 0, 0, 0, 0, 0U, ATOMIC_INIT(0), 0U },
		  { kFanNames[1], kFanPwms[1], kFanPowers[1], kFanSenses[1], kFanTachs[1], {}, true, false,
		    40, 40, 40, 0, 0, 0, 0, 0, 0U, ATOMIC_INIT(0), 0U },
	  },
	  status_led_(kStatusLed),
	  curves_(),
	  blink_phase_(false),
	  last_sample_ms_(0)
{
}

void FanController::InitRuntime()
{
	k_mutex_init(&mutex_);
}

void FanController::TachCallback(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	Channel *channel = CONTAINER_OF(cb, Channel, tach_cb);
	atomic_inc(&channel->tach_edges);
}

int FanController::Init()
{
	int rc = curves_.Init();
	if (rc != 0) {
		return rc;
	}

	if (!gpio_is_ready_dt(&status_led_)) {
		return -ENODEV;
	}

	rc = gpio_pin_configure_dt(&status_led_, GPIO_OUTPUT_INACTIVE);
	if (rc != 0) {
		return rc;
	}

	for (size_t i = 0; i < kFanCount; ++i) {
		if (!pwm_is_ready_dt(&fans_[i].pwm) || !gpio_is_ready_dt(&fans_[i].power) ||
		    !adc_is_ready_dt(&fans_[i].sense.port) || !gpio_is_ready_dt(&fans_[i].tach)) {
			return -ENODEV;
		}

		rc = gpio_pin_configure_dt(&fans_[i].power, GPIO_OUTPUT_INACTIVE);
		if (rc != 0) {
			return rc;
		}

		rc = adc_channel_setup_dt(&fans_[i].sense.port);
		if (rc != 0) {
			return rc;
		}

		rc = gpio_pin_configure_dt(&fans_[i].tach, GPIO_INPUT);
		if (rc != 0) {
			return rc;
		}

		gpio_init_callback(&fans_[i].tach_cb, TachCallback, BIT(fans_[i].tach.pin));
		rc = gpio_add_callback(fans_[i].tach.port, &fans_[i].tach_cb);
		if (rc != 0) {
			return rc;
		}

		rc = gpio_pin_interrupt_configure_dt(&fans_[i].tach, GPIO_INT_EDGE_TO_ACTIVE);
		if (rc != 0) {
			return rc;
		}
	}

	k_mutex_lock(&mutex_, K_FOREVER);
	for (size_t i = 0; i < kFanCount; ++i) {
		UpdateDerivedStateLocked(i);
		rc = ApplyLocked(i);
		if (rc != 0) {
			break;
		}
	}
	last_sample_ms_ = k_uptime_get();
	k_mutex_unlock(&mutex_);

	return rc;
}

void FanController::LoadPersistedState()
{
	k_mutex_lock(&mutex_, K_FOREVER);
	settings::AppConfig config = {};
	bool loaded = settings::LoadConfig(&config) == 0;

	for (size_t i = 0; i < kFanCount; ++i) {
		if (loaded) {
			fans_[i].enabled = config.fan_enabled[i];
			fans_[i].percent = MIN(config.fan_percent[i], 100U);
			fans_[i].use_adc_target = config.fan_use_adc_target[i];
		}
	}

	k_mutex_unlock(&mutex_);
}

int FanController::ApplyLocked(size_t index)
{
	uint32_t pulse = 0U;
	fans_[index].pwm_percent = curves_.EvaluatePercentToPwm(fans_[index].effective_percent);

	if (fans_[index].enabled) {
		pulse = static_cast<uint32_t>((static_cast<uint64_t>(fans_[index].pwm.period) *
					       fans_[index].pwm_percent) / 100U);
	}
	fans_[index].pwm_pulse_ns = pulse;

	int rc = gpio_pin_set_dt(&fans_[index].power, fans_[index].enabled ? 1 : 0);

	if (rc != 0) {
		return rc;
	}

	return pwm_set_pulse_dt(&fans_[index].pwm, pulse);
}

int FanController::ReadSenseLocked(size_t index, int32_t *raw, int32_t *mv)
{
	int16_t sample = 0;
	struct adc_sequence sequence = {
		.buffer = &sample,
		.buffer_size = sizeof(sample),
	};
	int32_t value = 0;
	int rc = adc_sequence_init_dt(&fans_[index].sense.port, &sequence);

	if (rc != 0) {
		return rc;
	}

	rc = adc_read_dt(&fans_[index].sense.port, &sequence);
	if (rc != 0) {
		return rc;
	}

	if (raw != nullptr) {
		*raw = sample;
	}

	value = sample;
	rc = adc_raw_to_millivolts_dt(&fans_[index].sense.port, &value);
	if (rc != 0) {
		return rc;
	}

	rc = voltage_divider_scale_dt(&fans_[index].sense, &value);
	if (rc != 0) {
		return rc;
	}

	*mv = value;
	return 0;
}

void FanController::UpdateDerivedStateLocked(size_t index)
{
	fans_[index].mapped_voltage_mv = curves_.EvaluateAdcRawToVoltage(fans_[index].adc_raw);
	fans_[index].adc_target_percent =
		curves_.EvaluateVoltageToPercent(fans_[index].mapped_voltage_mv);
	fans_[index].effective_percent =
		fans_[index].use_adc_target ? fans_[index].adc_target_percent : fans_[index].percent;
	fans_[index].target_rpm = curves_.EvaluatePercentToRpm(fans_[index].effective_percent);
	fans_[index].actual_percent = curves_.EvaluateRpmToPercent(fans_[index].actual_rpm);
	fans_[index].pwm_percent = curves_.EvaluatePercentToPwm(fans_[index].effective_percent);
}

int FanController::SetFan(size_t index, uint8_t percent, bool enabled, bool persist)
{
	if (index >= kFanCount) {
		return -EINVAL;
	}

	k_mutex_lock(&mutex_, K_FOREVER);
	fans_[index].percent = MIN(percent, 100U);
	fans_[index].enabled = enabled;
	UpdateDerivedStateLocked(index);
	int rc = ApplyLocked(index);
	k_mutex_unlock(&mutex_);

	if (rc == 0 && persist) {
		settings::SaveFanState(index, enabled, static_cast<uint8_t>(MIN(percent, 100U)));
	}

	return rc;
}

int FanController::SetAdcTargetMode(size_t index, bool use_adc_target, bool persist)
{
	if (index >= kFanCount) {
		return -EINVAL;
	}

	k_mutex_lock(&mutex_, K_FOREVER);
	fans_[index].use_adc_target = use_adc_target;
	UpdateDerivedStateLocked(index);
	int rc = ApplyLocked(index);
	k_mutex_unlock(&mutex_);

	if (rc == 0 && persist) {
		settings::SaveFanAdcTargetMode(index, use_adc_target);
	}

	return rc;
}

int FanController::ConfigureFan(size_t index, uint8_t percent, bool enabled, bool use_adc_target, bool persist)
{
	if (index >= kFanCount) {
		return -EINVAL;
	}

	k_mutex_lock(&mutex_, K_FOREVER);
	fans_[index].percent = MIN(percent, 100U);
	fans_[index].enabled = enabled;
	fans_[index].use_adc_target = use_adc_target;
	UpdateDerivedStateLocked(index);
	int rc = ApplyLocked(index);
	k_mutex_unlock(&mutex_);

	if (rc == 0 && persist) {
		settings::SaveFanState(index, enabled, static_cast<uint8_t>(MIN(percent, 100U)));
		settings::SaveFanAdcTargetMode(index, use_adc_target);
	}

	return rc;
}

int FanController::ConfigureFanTargetRpm(size_t index, int32_t target_rpm, bool enabled, bool persist)
{
	if (index >= kFanCount || target_rpm < 0) {
		return -EINVAL;
	}

	const uint8_t percent = curves_.EvaluateRpmToPercent(target_rpm);
	return ConfigureFan(index, percent, enabled, false, persist);
}

void FanController::UpdateStatusLedLocked(bool sta_connected, bool ap_enabled)
{
	if (sta_connected) {
		(void)gpio_pin_set_dt(&status_led_, 1);
	} else if (ap_enabled) {
		blink_phase_ = !blink_phase_;
		(void)gpio_pin_set_dt(&status_led_, blink_phase_ ? 1 : 0);
	} else {
		(void)gpio_pin_set_dt(&status_led_, 0);
	}
}

void FanController::UpdateTelemetry(bool sta_connected, bool ap_enabled)
{
	k_mutex_lock(&mutex_, K_FOREVER);
	int64_t now_ms = k_uptime_get();
	int64_t delta_ms = now_ms - last_sample_ms_;
	if (delta_ms <= 0) {
		delta_ms = 1000;
	}

	for (size_t i = 0; i < kFanCount; ++i) {
		int32_t raw = fans_[i].adc_raw;
		int32_t mv = fans_[i].adc_mv;

		if (ReadSenseLocked(i, &raw, &mv) == 0) {
			fans_[i].adc_raw = raw;
			fans_[i].adc_mv = mv;
		}

		uint32_t edges = static_cast<uint32_t>(atomic_get(&fans_[i].tach_edges));
		uint32_t delta_edges = edges - fans_[i].last_tach_edges;
		fans_[i].last_tach_edges = edges;

		if (fans_[i].enabled && delta_edges > 0U) {
			fans_[i].actual_rpm = static_cast<int32_t>(
				(static_cast<uint64_t>(delta_edges) * 60000ULL) /
				(static_cast<uint64_t>(delta_ms) * kTachPulsesPerRevolution));
		} else {
			fans_[i].actual_rpm = 0;
		}

		UpdateDerivedStateLocked(i);
	}

	last_sample_ms_ = now_ms;
	UpdateStatusLedLocked(sta_connected, ap_enabled);
	k_mutex_unlock(&mutex_);
}

void FanController::GetState(size_t index, FanState *state)
{
	if (index >= kFanCount || state == nullptr) {
		return;
	}

	k_mutex_lock(&mutex_, K_FOREVER);
	state->enabled = fans_[index].enabled;
	state->use_adc_target = fans_[index].use_adc_target;
	state->percent = fans_[index].percent;
	state->effective_percent = fans_[index].effective_percent;
	state->pwm_percent = fans_[index].pwm_percent;
	state->adc_target_percent = fans_[index].adc_target_percent;
	state->actual_percent = fans_[index].actual_percent;
	state->adc_raw = fans_[index].adc_raw;
	state->adc_mv = fans_[index].adc_mv;
	state->mapped_voltage_mv = fans_[index].mapped_voltage_mv;
	state->actual_rpm = fans_[index].actual_rpm;
	state->target_rpm = fans_[index].target_rpm;
	state->pwm_pulse_ns = fans_[index].pwm_pulse_ns;
	state->tach_valid = true;
	k_mutex_unlock(&mutex_);
}

void FanController::GetAllStates(FanState states[kFanCount])
{
	if (states == nullptr) {
		return;
	}

	k_mutex_lock(&mutex_, K_FOREVER);
	for (size_t i = 0; i < kFanCount; ++i) {
		states[i].enabled = fans_[i].enabled;
		states[i].use_adc_target = fans_[i].use_adc_target;
		states[i].percent = fans_[i].percent;
		states[i].effective_percent = fans_[i].effective_percent;
		states[i].pwm_percent = fans_[i].pwm_percent;
		states[i].adc_target_percent = fans_[i].adc_target_percent;
		states[i].actual_percent = fans_[i].actual_percent;
		states[i].adc_raw = fans_[i].adc_raw;
		states[i].adc_mv = fans_[i].adc_mv;
		states[i].mapped_voltage_mv = fans_[i].mapped_voltage_mv;
		states[i].actual_rpm = fans_[i].actual_rpm;
		states[i].target_rpm = fans_[i].target_rpm;
		states[i].pwm_pulse_ns = fans_[i].pwm_pulse_ns;
		states[i].tach_valid = true;
	}
	k_mutex_unlock(&mutex_);
}

const curves::CurveProfiles &FanController::GetCurves() const
{
	return curves_;
}

} // namespace fanctl
