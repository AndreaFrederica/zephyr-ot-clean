/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_HTTP_API_HPP_
#define FAN_CONTROLLER_HTTP_API_HPP_

#include <stddef.h>

#include "core/service_context.hpp"
#include "http_common.hpp"

namespace fanctl::http {

bool HandleApiRequest(int client, const Request &request, char *scratch, size_t scratch_len,
		      const ServiceContext &services);

} // namespace fanctl::http

#endif
