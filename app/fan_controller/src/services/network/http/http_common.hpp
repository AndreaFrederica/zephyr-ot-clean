/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_HTTP_COMMON_HPP_
#define FAN_CONTROLLER_HTTP_COMMON_HPP_

#include <stddef.h>

namespace fanctl::http {

constexpr size_t kMaxRequestBodySize = 129U * 1024U;
constexpr size_t kRecvBufferHeadroom = 3U * 1024U;
constexpr size_t kRecvBufferSize = kMaxRequestBodySize + kRecvBufferHeadroom;
constexpr size_t kLargeBufferSize = 65536;
constexpr size_t kStatusBufferSize = 2048;
constexpr size_t kWorkerCount = 4;
constexpr size_t kClientQueueDepth = 8;
constexpr int kHttpBacklog = 8;

struct Request {
	char method[8];
	char target[160];
	char *path;
	char *query;
	const char *body;
	size_t body_len;
};

int SendAll(int client, const char *data, size_t len);
int SendResponseSized(int client, const char *status, const char *content_type, const char *body,
		      size_t body_len);
int SendResponse(int client, const char *status, const char *content_type, const char *body);
int SendJsonResult(int client, bool ok, const char *message);
int SendFileResponse(int client, const char *status, const char *content_type, const char *path);

void JsonEscape(char *dst, size_t dst_len, const char *src);
bool CopyKvValue(const char *source, const char *key, char *out, size_t out_len);
void SplitTarget(char *target, char **path, char **query);

} // namespace fanctl::http

#endif
