/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "http_server.hpp"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>

#include "core/common.hpp"
#include "http_api.hpp"
#include "http_common.hpp"
#include "storage.hpp"
#include "wifi_manager.hpp"

LOG_MODULE_REGISTER(http_server, LOG_LEVEL_INF);

namespace fanctl {

namespace {

K_THREAD_STACK_DEFINE(g_http_server_stack, kHttpStackSize);
char g_large_buffer[http::kLargeBufferSize];

} // namespace

HttpServer::HttpServer(const ServiceContext &services)
	: fan_controller_(*services.fan_controller), wifi_manager_(*services.wifi_manager),
	  host_control_(*services.host_control)
{
}

void HttpServer::Start()
{
	LOG_INF("HTTP server thread starting on port %d", kHttpPort);
	k_thread_create(&thread_, g_http_server_stack, K_THREAD_STACK_SIZEOF(g_http_server_stack),
			ThreadEntry, this, nullptr, nullptr, 6, 0, K_NO_WAIT);
	k_thread_name_set(&thread_, "fanctl_http");

#if defined(CONFIG_SMP) && defined(CONFIG_SCHED_CPU_MASK)
	// 绑定到APP CPU (CPU 1)，与风扇控制隔离
	if (arch_num_cpus() > 1) {
		(void)k_thread_cpu_pin(&thread_, 1);
	}
#endif
}

void HttpServer::ThreadEntry(void *ctx, void *unused1, void *unused2)
{
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);

	static_cast<HttpServer *>(ctx)->Run();
}

void HttpServer::HandleClient(int client)
{
	char buffer[http::kRecvBufferSize];
	http::Request request = {};
	int received = zsock_recv(client, buffer, sizeof(buffer) - 1, 0);

	if (received <= 0) {
		return;
	}

	buffer[received] = '\0';
	char *headers_end = strstr(buffer, "\r\n\r\n");
	if (headers_end == nullptr) {
		(void)http::SendResponse(client, "400 Bad Request", "text/plain", "Malformed request");
		return;
	}

	request.body = headers_end + 4;
	request.path = request.target;
	request.query = nullptr;
	(void)sscanf(buffer, "%7s %159s", request.method, request.target);
	http::SplitTarget(request.target, &request.path, &request.query);

	char *content_length_hdr = strstr(buffer, "Content-Length:");
	int content_length = (content_length_hdr != nullptr)
			     ? atoi(content_length_hdr + strlen("Content-Length:"))
			     : 0;

	// 安全检查：Content-Length 不能超过缓冲区剩余容量（预留 256 字节给 headers）
	if (content_length > static_cast<int>(sizeof(buffer)) - 256) {
		(void)http::SendResponse(client, "413 Payload Too Large", "text/plain",
					 "Request body too large");
		return;
	}

	request.body_len = static_cast<size_t>(content_length > 0 ? content_length : 0);

	// 循环读取 body 数据直到 content_length 或缓冲区满
	while ((buffer + received) - request.body < content_length &&
	       received < static_cast<int>(sizeof(buffer)) - 1) {
		int rc = zsock_recv(client, buffer + received, sizeof(buffer) - 1 - received, 0);
		if (rc <= 0) {
			break;
		}
		received += rc;
		buffer[received] = '\0';
	}

	// 验证实际接收的数据量是否匹配 Content-Length
	if (content_length > 0 &&
	    (buffer + received) - request.body < content_length) {
		(void)http::SendResponse(client, "400 Bad Request", "text/plain",
					 "Incomplete request body");
		return;
	}

	if (strcmp(request.method, "GET") == 0 && strncmp(request.path, "/api/", 5) != 0) {
		char fs_path[96];
		const char *content_type = nullptr;

		if (storage::ResolveWebPath(request.path, fs_path, sizeof(fs_path), &content_type) &&
		    http::SendFileResponse(client, "200 OK", content_type, fs_path) == 0) {
			return;
		}

		(void)http::SendResponse(client, "404 Not Found", "text/plain", "Static file not found");
		return;
	}

	ServiceContext services = { &fan_controller_, &wifi_manager_, &host_control_ };
	if (http::HandleApiRequest(client, request, g_large_buffer, sizeof(g_large_buffer), services)) {
		return;
	}

	(void)http::SendResponse(client, "404 Not Found", "text/plain", "Not found");
}

void HttpServer::Run()
{
	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(kHttpPort);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	LOG_INF("HTTP server binding to 0.0.0.0:%d", kHttpPort);

	int server = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server < 0) {
		LOG_ERR("HTTP socket create failed");
		return;
	}

	if (zsock_bind(server, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
		LOG_ERR("HTTP bind failed");
		(void)zsock_close(server);
		return;
	}

	if (zsock_listen(server, http::kHttpBacklog) != 0) {
		LOG_ERR("HTTP listen failed");
		(void)zsock_close(server);
		return;
	}

	WifiSnapshot wifi = {};
	wifi_manager_.GetSnapshot(&wifi);
	LOG_INF("HTTP server listening on 0.0.0.0:%d", kHttpPort);
	LOG_INF("HTTP endpoints: AP=http://%s/ STA=http://%s/", kApIpAddr,
		wifi.sta_ip[0] != '\0' ? wifi.sta_ip : "pending");

	while (true) {
		int client = zsock_accept(server, nullptr, nullptr);
		if (client < 0) {
			k_sleep(K_MSEC(100));
			continue;
		}

		HandleClient(client);
		(void)zsock_close(client);
	}
}

} // namespace fanctl
