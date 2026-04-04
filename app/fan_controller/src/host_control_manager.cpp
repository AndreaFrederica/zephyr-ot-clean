/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "host_control_manager.hpp"

#include <zephyr/sys/util.h>

namespace fanctl {

HostControlManager::HostControlManager(FanController &fan_controller)
	: fan_controller_(fan_controller), alive_check_enabled_(false), timed_out_(false),
	  timeout_ms_(5000U), last_alive_ms_(0)
{
}

int HostControlManager::Init()
{
	k_mutex_init(&mutex_);

	settings::AppConfig config = {};
	int rc = settings::LoadConfig(&config);
	if (rc != 0) {
		return rc;
	}

	Configure(config);
	return 0;
}

void HostControlManager::Configure(const settings::AppConfig &config)
{
	k_mutex_lock(&mutex_, K_FOREVER);
	alive_check_enabled_ = config.host_alive_check_enabled;
	timeout_ms_ = config.host_alive_timeout_ms;
	if (!alive_check_enabled_) {
		timed_out_ = false;
	}
	k_mutex_unlock(&mutex_);
}

void HostControlManager::MarkAlive()
{
	k_mutex_lock(&mutex_, K_FOREVER);
	last_alive_ms_ = k_uptime_get();
	timed_out_ = false;
	k_mutex_unlock(&mutex_);
}

void HostControlManager::Tick()
{
	k_mutex_lock(&mutex_, K_FOREVER);
	const bool enabled = alive_check_enabled_;
	const bool already_timed_out = timed_out_;
	const uint32_t timeout_ms = timeout_ms_;
	const int64_t last_alive_ms = last_alive_ms_;
	k_mutex_unlock(&mutex_);

	if (!enabled || already_timed_out) {
		return;
	}

	const int64_t now_ms = k_uptime_get();
	if (last_alive_ms == 0 || now_ms - last_alive_ms < static_cast<int64_t>(timeout_ms)) {
		return;
	}

	// 超时后将所有风扇PWM设为0（关闭风扇），但保持电源使能状态
	for (size_t i = 0; i < kFanCount; ++i) {
		FanState state = {};
		fan_controller_.GetState(i, &state);
		// 仅对非ADC模式的风扇应用超时保护
		if (state.enabled && !state.use_adc_target) {
			// 设置percent=0关闭风扇，但保持enabled=true
			(void)fan_controller_.ConfigureFan(i, 0, true, state.use_adc_target, false);
		}
	}

	k_mutex_lock(&mutex_, K_FOREVER);
	timed_out_ = true;
	k_mutex_unlock(&mutex_);
}

void HostControlManager::GetSnapshot(HostControlSnapshot *snapshot) const
{
	if (snapshot == nullptr) {
		return;
	}

	k_mutex_lock(const_cast<k_mutex *>(&mutex_), K_FOREVER);
	snapshot->alive_check_enabled = alive_check_enabled_;
	snapshot->timed_out = timed_out_;
	snapshot->timeout_ms = timeout_ms_;
	if (last_alive_ms_ <= 0) {
		snapshot->last_alive_ago_ms = 0U;
	} else {
		const int64_t delta = k_uptime_get() - last_alive_ms_;
		snapshot->last_alive_ago_ms = static_cast<uint32_t>(delta > 0 ? delta : 0);
	}
	k_mutex_unlock(const_cast<k_mutex *>(&mutex_));
}

} // namespace fanctl
