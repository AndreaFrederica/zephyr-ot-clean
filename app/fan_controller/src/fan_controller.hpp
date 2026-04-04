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

#include "common.hpp"
#include "curve_profiles.hpp"

namespace fanctl {

class FanController {
public:
	FanController();

	void InitRuntime();
	int Init();
	void LoadPersistedState();
	int SetFan(size_t index, uint8_t percent, bool enabled, bool persist);
	int SetAdcTargetMode(size_t index, bool use_adc_target, bool persist);
	int ConfigureFan(size_t index, uint8_t percent, bool enabled, bool use_adc_target, bool persist);
	int ConfigureFanTargetRpm(size_t index, int32_t target_rpm, bool enabled, bool persist);
	void UpdateTelemetry(bool sta_connected, bool ap_enabled);
	void GetState(size_t index, FanState *state);
	void GetAllStates(FanState states[kFanCount]);
	const curves::CurveProfiles &GetCurves() const;

private:
	struct Channel {
		const char *name;
		struct pwm_dt_spec pwm;
		struct gpio_dt_spec power;
		struct voltage_divider_dt_spec sense;
		struct gpio_dt_spec tach;
		struct gpio_callback tach_cb;
		bool enabled;
		bool use_adc_target;
		uint8_t percent;
		uint8_t effective_percent;
		uint8_t pwm_percent;
		uint8_t adc_target_percent;
		uint8_t actual_percent;
		int32_t adc_raw;
		int32_t adc_mv;
		int32_t mapped_voltage_mv;
		int32_t actual_rpm;
		int32_t target_rpm;
		uint32_t pwm_pulse_ns;
		atomic_t tach_edges;
		uint32_t last_tach_edges;
	};

	static void TachCallback(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins);
	int ApplyLocked(size_t index);
	int ReadSenseLocked(size_t index, int32_t *raw, int32_t *mv);
	void UpdateDerivedStateLocked(size_t index);
	void UpdateStatusLedLocked(bool sta_connected, bool ap_enabled);

	Channel fans_[kFanCount];
	struct gpio_dt_spec status_led_;
	curves::CurveProfiles curves_;
	struct k_mutex mutex_;
	bool blink_phase_;
	int64_t last_sample_ms_;
};

} // namespace fanctl

#endif
