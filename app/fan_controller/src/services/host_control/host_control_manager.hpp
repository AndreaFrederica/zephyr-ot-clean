/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_HOST_CONTROL_MANAGER_HPP_
#define FAN_CONTROLLER_HOST_CONTROL_MANAGER_HPP_

#include <zephyr/kernel.h>

#include "core/common.hpp"
#include "fan_shared_state.hpp"
#include "storage/settings_store.hpp"

namespace fanctl {

class FanController;

/**
 * @brief 主机控制管理器
 * 
 * 监控主机活性，超时后自动关闭风扇
 * 从共享内存读取风扇状态
 */
class HostControlManager {
public:
	HostControlManager(const FanControllerSharedState *shared_state, FanController *fan_controller);

	int Init();
	void Configure(const settings::AppConfig &config);
	void MarkAlive();
	void Tick();
	void GetSnapshot(HostControlSnapshot *snapshot) const;

private:
	const FanControllerSharedState *shared_state_;
	
	struct k_mutex mutex_;
	bool alive_check_enabled_;
	bool timed_out_;
	uint32_t timeout_ms_;
	int64_t last_alive_ms_;
	
	// 控制命令接口 (用于超时后控制风扇)
	FanController *fan_controller_;
	
	void HandleTimeout();
};

} // namespace fanctl

#endif // FAN_CONTROLLER_HOST_CONTROL_MANAGER_HPP_
