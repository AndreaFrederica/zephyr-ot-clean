/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_HTTP_SERVER_HPP_
#define FAN_CONTROLLER_HTTP_SERVER_HPP_

#include <zephyr/kernel.h>

#include "core/service_context.hpp"
#include "http_common.hpp"

namespace fanctl {

class FanController;
class HostControlManager;
class WifiManager;

class HttpServer {
public:
	explicit HttpServer(const ServiceContext &services);

	void Start();

private:
	static void ListenerThreadEntry(void *ctx, void *unused1, void *unused2);
	static void WorkerThreadEntry(void *ctx, void *worker_index_ptr, void *unused2);
	void Run();
	void WorkerLoop(size_t worker_index);
	void HandleClient(int client);

	FanController &fan_controller_;
	WifiManager &wifi_manager_;
	HostControlManager &host_control_;
	struct k_thread listener_thread_;
	struct k_thread worker_threads_[http::kWorkerCount];
};

} // namespace fanctl

#endif
