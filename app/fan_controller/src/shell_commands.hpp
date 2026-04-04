/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_SHELL_COMMANDS_HPP_
#define FAN_CONTROLLER_SHELL_COMMANDS_HPP_

#include "fan_controller.hpp"
#include "host_control_manager.hpp"
#include "wifi_manager.hpp"

namespace fanctl::shell_commands {

void Init(FanController &fan_controller, WifiManager &wifi_manager, HostControlManager &host_control);

} // namespace fanctl::shell_commands

#endif
