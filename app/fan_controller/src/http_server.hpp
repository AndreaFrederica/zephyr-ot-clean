/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_HTTP_SERVER_HPP_
#define FAN_CONTROLLER_HTTP_SERVER_HPP_

#include <zephyr/kernel.h>

#include "fan_controller.hpp"
#include "host_control_manager.hpp"
#include "wifi_manager.hpp"

namespace fanctl {

class HttpServer {
public:
	HttpServer(FanController &fan_controller, WifiManager &wifi_manager,
		   HostControlManager &host_control);

	void Start();

private:
	static void ThreadEntry(void *ctx, void *unused1, void *unused2);
	void Run();
	void HandleClient(int client);

	FanController &fan_controller_;
	WifiManager &wifi_manager_;
	HostControlManager &host_control_;
	struct k_thread thread_;
};

} // namespace fanctl

#endif
