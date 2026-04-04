/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_HOST_CONTROL_MANAGER_HPP_
#define FAN_CONTROLLER_HOST_CONTROL_MANAGER_HPP_

#include <zephyr/kernel.h>

#include "fan_controller.hpp"
#include "settings_store.hpp"

namespace fanctl {

class HostControlManager {
public:
	explicit HostControlManager(FanController &fan_controller);

	int Init();
	void Configure(const settings::AppConfig &config);
	void MarkAlive();
	void Tick();
	void GetSnapshot(HostControlSnapshot *snapshot) const;

private:
	FanController &fan_controller_;
	struct k_mutex mutex_;
	bool alive_check_enabled_;
	bool timed_out_;
	uint32_t timeout_ms_;
	int64_t last_alive_ms_;
};

} // namespace fanctl

#endif
