/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_SERVICE_CONTEXT_HPP_
#define FAN_CONTROLLER_SERVICE_CONTEXT_HPP_

namespace fanctl {

class FanController;
class WifiManager;
class HostControlManager;

struct ServiceContext {
	FanController *fan_controller;
	WifiManager *wifi_manager;
	HostControlManager *host_control;
};

} // namespace fanctl

#endif
