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
#include "memory_domains.hpp"
#include "http_api.hpp"
#include "http_common.hpp"
#include "storage.hpp"
#include "wifi_manager.hpp"

LOG_MODULE_REGISTER(http_server, LOG_LEVEL_INF);

namespace fanctl {

namespace {

K_THREAD_STACK_DEFINE(g_http_listener_stack, kHttpAcceptStackSize);
K_THREAD_STACK_ARRAY_DEFINE(g_http_worker_stacks, http::kWorkerCount, kHttpWorkerStackSize);
K_MSGQ_DEFINE(g_http_client_queue, sizeof(int), http::kClientQueueDepth, sizeof(int));
uintptr_t g_http_worker_ids[http::kWorkerCount];

} // namespace

HttpServer::HttpServer(const ServiceContext &services)
	: fan_controller_(*services.fan_controller), wifi_manager_(*services.wifi_manager),
	  host_control_(*services.host_control)
{
}

void HttpServer::Start()
{
	LOG_INF("HTTP server thread starting on port %d", kHttpPort);
	for (size_t i = 0U; i < http::kWorkerCount; ++i) {
		g_http_worker_ids[i] = i;
		k_thread_create(&worker_threads_[i], g_http_worker_stacks[i],
				K_THREAD_STACK_SIZEOF(g_http_worker_stacks[i]), WorkerThreadEntry, this,
				reinterpret_cast<void *>(g_http_worker_ids[i]), nullptr, 6, 0,
				K_NO_WAIT);

		char thread_name[16];
		(void)snprintf(thread_name, sizeof(thread_name), "http_w%u",
			       static_cast<unsigned int>(i));
		k_thread_name_set(&worker_threads_[i], thread_name);
	}

	k_thread_create(&listener_thread_, g_http_listener_stack,
			K_THREAD_STACK_SIZEOF(g_http_listener_stack), ListenerThreadEntry, this,
			nullptr, nullptr, 6, 0, K_NO_WAIT);
	k_thread_name_set(&listener_thread_, "fanctl_http");

#if defined(CONFIG_SMP) && defined(CONFIG_SCHED_CPU_MASK)
	// 绑定到APP CPU (CPU 1)，与风扇控制隔离
	if (arch_num_cpus() > 1) {
		(void)k_thread_cpu_pin(&listener_thread_, 1);
		for (size_t i = 0U; i < http::kWorkerCount; ++i) {
			(void)k_thread_cpu_pin(&worker_threads_[i], 1);
		}
	}
#endif
}

void HttpServer::ListenerThreadEntry(void *ctx, void *unused1, void *unused2)
{
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);

	static_cast<HttpServer *>(ctx)->Run();
}

void HttpServer::WorkerThreadEntry(void *ctx, void *worker_index_ptr, void *unused2)
{
	ARG_UNUSED(unused2);

	size_t worker_index = reinterpret_cast<uintptr_t>(worker_index_ptr);
	static_cast<HttpServer *>(ctx)->WorkerLoop(worker_index);
}

void HttpServer::HandleClient(int client)
{
	struct zsock_timeval timeout = {
		.tv_sec = 5,
		.tv_usec = 0,
	};
	int optval = 1;

	(void)zsock_setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	(void)zsock_setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	(void)zsock_setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));

	memory::HttpBufferSet buffers = {};
	if (!memory::AcquireHttpBufferSet(&buffers, K_NO_WAIT)) {
		LOG_WRN("HTTP PSRAM buffer set busy");
		(void)http::SendResponse(client, "503 Service Unavailable", "text/plain",
					 "HTTP memory busy, retry shortly");
		return;
	}
	char *buffer = buffers.recv_buffer;

	http::Request request = {};
	int received = zsock_recv(client, buffer, buffers.recv_capacity - 1, 0);

	if (received <= 0) {
		memory::ReleaseHttpBufferSet(&buffers);
		return;
	}

	buffer[received] = '\0';
	char *headers_end = strstr(buffer, "\r\n\r\n");
	if (headers_end == nullptr) {
		memory::ReleaseHttpBufferSet(&buffers);
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
	if (content_length > static_cast<int>(buffers.recv_capacity - 256U)) {
		memory::ReleaseHttpBufferSet(&buffers);
		(void)http::SendResponse(client, "413 Payload Too Large", "text/plain",
					 "Request body too large");
		return;
	}

	request.body_len = static_cast<size_t>(content_length > 0 ? content_length : 0);

	// 循环读取 body 数据直到 content_length 或缓冲区满
	while ((buffer + received) - request.body < content_length &&
	       received < static_cast<int>(buffers.recv_capacity - 1U)) {
		int rc = zsock_recv(client, buffer + received,
				    buffers.recv_capacity - 1U - static_cast<size_t>(received), 0);
		if (rc <= 0) {
			break;
		}
		received += rc;
		buffer[received] = '\0';
	}

	// 验证实际接收的数据量是否匹配 Content-Length
	if (content_length > 0 &&
	    (buffer + received) - request.body < content_length) {
		memory::ReleaseHttpBufferSet(&buffers);
		(void)http::SendResponse(client, "400 Bad Request", "text/plain",
					 "Incomplete request body");
		return;
	}

	if (strcmp(request.method, "GET") == 0 && strncmp(request.path, "/api/", 5) != 0) {
		char fs_path[96];
		const char *content_type = nullptr;

		if (storage::ResolveWebPath(request.path, fs_path, sizeof(fs_path), &content_type) &&
		    http::SendFileResponse(client, "200 OK", content_type, fs_path) == 0) {
			memory::ReleaseHttpBufferSet(&buffers);
			return;
		}

		memory::ReleaseHttpBufferSet(&buffers);
		(void)http::SendResponse(client, "404 Not Found", "text/plain", "Static file not found");
		return;
	}

	ServiceContext services = { &fan_controller_, &wifi_manager_, &host_control_ };
	char *scratch = buffers.scratch_buffer;

	if (http::HandleApiRequest(client, request, scratch, buffers.scratch_capacity, services)) {
		memory::ReleaseHttpBufferSet(&buffers);
		return;
	}

	memory::ReleaseHttpBufferSet(&buffers);
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

	int reuse_addr = 1;
	(void)zsock_setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse_addr,
			       sizeof(reuse_addr));

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
	LOG_INF("HTTP worker pool: %u workers, queue depth %u",
		static_cast<unsigned int>(http::kWorkerCount),
		static_cast<unsigned int>(http::kClientQueueDepth));

	while (true) {
		int client = zsock_accept(server, nullptr, nullptr);
		if (client < 0) {
			k_sleep(K_MSEC(100));
			continue;
		}

		if (k_msgq_put(&g_http_client_queue, &client, K_NO_WAIT) != 0) {
			LOG_WRN("HTTP client queue full");
			(void)http::SendResponse(client, "503 Service Unavailable", "text/plain",
						 "HTTP workers busy, retry shortly");
			(void)zsock_close(client);
		}
	}
}

void HttpServer::WorkerLoop(size_t worker_index)
{
	LOG_INF("HTTP worker %u online", static_cast<unsigned int>(worker_index));

	while (true) {
		int client = -1;
		if (k_msgq_get(&g_http_client_queue, &client, K_FOREVER) != 0) {
			continue;
		}

		HandleClient(client);
		(void)zsock_close(client);
	}
}

} // namespace fanctl
