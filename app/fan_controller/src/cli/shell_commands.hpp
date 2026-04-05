/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_SHELL_COMMANDS_HPP_
#define FAN_CONTROLLER_SHELL_COMMANDS_HPP_

#include "core/service_context.hpp"

namespace fanctl::shell_commands {

void Init(const ServiceContext &services);

} // namespace fanctl::shell_commands

#endif
