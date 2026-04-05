/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_SSH_SERVER_HPP_
#define FAN_CONTROLLER_SSH_SERVER_HPP_

#include <zephyr/kernel.h>

#include <wolfssh/ssh.h>

#include "settings_store.hpp"
#include "core/service_context.hpp"

namespace fanctl {

class FanController;
class HostControlManager;
class WifiManager;

class SshServer {
public:
	explicit SshServer(const ServiceContext &services);

	int Init();
	void Start();
	bool IsEnabled() const;
	int GetListenPort() const;

private:
	static void ThreadEntry(void *ctx, void *unused1, void *unused2);
	void Run();
	int EnsureHostKey();
	int SetupContext();
	int HandleClient(int client);

	FanController &fan_controller_;
	WifiManager &wifi_manager_;
	HostControlManager &host_control_;
	settings::SshConfig config_;
	WOLFSSH_CTX *ctx_;
	struct k_thread thread_;
	bool enabled_;
};

} // namespace fanctl

#endif
