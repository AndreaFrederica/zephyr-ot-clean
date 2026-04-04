/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "http_server.hpp"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/reboot.h>

#include "common.hpp"
#include "curve_profiles.hpp"
#include "settings_store.hpp"
#include "storage.hpp"
#include "wifi_manager.hpp"

LOG_MODULE_REGISTER(http_server, LOG_LEVEL_INF);

namespace fanctl {

namespace {

constexpr size_t kRecvBufferSize = 4096;
constexpr size_t kLargeBufferSize = 4096;
constexpr size_t kStatusBufferSize = 2048;
constexpr int kHttpBacklog = 4;
K_THREAD_STACK_DEFINE(g_http_server_stack, kHttpStackSize);
char g_large_buffer[kLargeBufferSize];

int SendAll(int client, const char *data, size_t len)
{
	size_t offset = 0U;

	while (offset < len) {
		int rc = zsock_send(client, data + offset, len - offset, 0);
		if (rc <= 0) {
			return (rc < 0) ? rc : -EIO;
		}
		offset += static_cast<size_t>(rc);
	}

	return 0;
}

int SendResponseSized(int client, const char *status, const char *content_type, const char *body,
		      size_t body_len)
{
	char header[256];
	int header_len = snprintf(header, sizeof(header),
				  "HTTP/1.1 %s\r\n"
				  "Content-Type: %s\r\n"
				  "Content-Length: %u\r\n"
				  "Connection: close\r\n\r\n",
				  status, content_type, static_cast<unsigned int>(body_len));

	if (header_len <= 0 || header_len >= static_cast<int>(sizeof(header))) {
		return -ENOMEM;
	}

	int rc = SendAll(client, header, static_cast<size_t>(header_len));
	if (rc != 0) {
		return rc;
	}

	return SendAll(client, body, body_len);
}

int SendResponse(int client, const char *status, const char *content_type, const char *body)
{
	return SendResponseSized(client, status, content_type, body, strlen(body));
}

int SendJsonResult(int client, bool ok, const char *message)
{
	char response[160];
	int written = snprintf(response, sizeof(response), "{\"ok\":%s,\"message\":\"%s\"}",
			       ok ? "true" : "false", message);

	if (written <= 0 || written >= static_cast<int>(sizeof(response))) {
		return -ENOMEM;
	}

	return SendResponse(client, ok ? "200 OK" : "400 Bad Request", "application/json", response);
}

int SendFileResponse(int client, const char *status, const char *content_type, const char *path)
{
	struct fs_dirent entry = {};
	struct fs_file_t file;
	char header[256];
	char chunk[512];
	int rc = fs_stat(path, &entry);

	if (rc != 0 || entry.type != FS_DIR_ENTRY_FILE) {
		return -ENOENT;
	}

	int header_len = snprintf(header, sizeof(header),
				  "HTTP/1.1 %s\r\n"
				  "Content-Type: %s\r\n"
				  "Content-Length: %u\r\n"
				  "Connection: close\r\n\r\n",
				  status, content_type, static_cast<unsigned int>(entry.size));
	if (header_len <= 0 || header_len >= static_cast<int>(sizeof(header))) {
		return -ENOMEM;
	}

	rc = SendAll(client, header, static_cast<size_t>(header_len));
	if (rc != 0) {
		return rc;
	}

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_READ);
	if (rc != 0) {
		return rc;
	}

	while (true) {
		ssize_t read_len = fs_read(&file, chunk, sizeof(chunk));
		if (read_len < 0) {
			(void)fs_close(&file);
			return static_cast<int>(read_len);
		}
		if (read_len == 0) {
			break;
		}

		rc = SendAll(client, chunk, static_cast<size_t>(read_len));
		if (rc != 0) {
			(void)fs_close(&file);
			return rc;
		}
	}

	(void)fs_close(&file);
	return 0;
}

void JsonEscape(char *dst, size_t dst_len, const char *src)
{
	size_t out = 0U;

	for (size_t i = 0U; src[i] != '\0' && out + 2U < dst_len; ++i) {
		char c = src[i];

		if (c == '"' || c == '\\') {
			dst[out++] = '\\';
			dst[out++] = c;
		} else if (static_cast<unsigned char>(c) < 0x20U) {
			dst[out++] = '_';
		} else {
			dst[out++] = c;
		}
	}

	dst[out] = '\0';
}

int DecodeHexNibble(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'A' && c <= 'F') {
		return 10 + c - 'A';
	}
	if (c >= 'a' && c <= 'f') {
		return 10 + c - 'a';
	}
	return -1;
}

bool DecodeUrlComponent(const char *src, size_t src_len, char *dst, size_t dst_len)
{
	size_t pos = 0U;

	for (size_t i = 0U; i < src_len && pos + 1U < dst_len; ++i) {
		if (src[i] == '+') {
			dst[pos++] = ' ';
			continue;
		}

		if (src[i] == '%' && i + 2U < src_len) {
			int hi = DecodeHexNibble(src[i + 1]);
			int lo = DecodeHexNibble(src[i + 2]);

			if (hi < 0 || lo < 0) {
				return false;
			}

			dst[pos++] = static_cast<char>((hi << 4) | lo);
			i += 2U;
			continue;
		}

		dst[pos++] = src[i];
	}

	dst[pos] = '\0';
	return true;
}

bool CopyKvValue(const char *source, const char *key, char *out, size_t out_len)
{
	if (source == nullptr || key == nullptr || out == nullptr || out_len == 0U) {
		return false;
	}

	for (const char *cursor = source; *cursor != '\0';) {
		const char *pair_end = strchr(cursor, '&');
		size_t pair_len = (pair_end != nullptr) ? static_cast<size_t>(pair_end - cursor)
							: strlen(cursor);
		const char *eq = static_cast<const char *>(memchr(cursor, '=', pair_len));

		if (eq != nullptr) {
			size_t key_len = static_cast<size_t>(eq - cursor);
			if (strlen(key) == key_len && strncmp(cursor, key, key_len) == 0) {
				return DecodeUrlComponent(eq + 1, pair_len - key_len - 1U, out, out_len);
			}
		}

		cursor = (pair_end != nullptr) ? (pair_end + 1) : (cursor + pair_len);
		if (pair_end == nullptr) {
			break;
		}
	}

	return false;
}

void SplitTarget(char *target, char **path, char **query)
{
	*path = target;
	*query = nullptr;

	char *separator = strchr(target, '?');
	if (separator != nullptr) {
		*separator = '\0';
		*query = separator + 1;
	}
}

int ApplyConfig(const settings::AppConfig &config, WifiManager &wifi_manager,
		HostControlManager &host_control)
{
	host_control.Configure(config);

	// Load WiFi config separately
	settings::WifiConfig wifi_config = {};
	if (settings::LoadWifiConfig(&wifi_config) != 0) {
		settings::FillWifiDefaults(&wifi_config);
	}

	return (wifi_config.sta_ssid[0] == '\0')
		       ? wifi_manager.ClearCredentials()
		       : wifi_manager.SaveAndConnect(wifi_config.sta_ssid, wifi_config.sta_psk);
}

int ReloadAndApplyConfig(WifiManager &wifi_manager, HostControlManager &host_control)
{
	settings::AppConfig config = {};
	int rc = settings::LoadConfig(&config);

	if (rc != 0) {
		return rc;
	}

	return ApplyConfig(config, wifi_manager, host_control);
}

int SaveFanDefaultsFromRequest(FanController &fan_controller, const char *body)
{
	char id_buf[8];
	char enabled_buf[8];
	char use_adc_target_buf[8];
	char percent_buf[8];
	char target_rpm_buf[12];
	bool has_percent = false;
	bool has_target_rpm = false;
	bool has_use_adc_target = false;

	if (!CopyKvValue(body, "id", id_buf, sizeof(id_buf)) ||
	    !CopyKvValue(body, "enabled", enabled_buf, sizeof(enabled_buf))) {
		return -EINVAL;
	}

	has_percent = CopyKvValue(body, "percent", percent_buf, sizeof(percent_buf));
	has_target_rpm = CopyKvValue(body, "target_rpm", target_rpm_buf, sizeof(target_rpm_buf));
	has_use_adc_target =
		CopyKvValue(body, "use_adc_target", use_adc_target_buf, sizeof(use_adc_target_buf));

	long id = strtol(id_buf, nullptr, 10);
	if (id < 1 || id > static_cast<long>(kFanCount)) {
		return -EINVAL;
	}

	settings::AppConfig config = {};
	int rc = settings::LoadConfig(&config);
	if (rc != 0) {
		return rc;
	}

	const size_t index = static_cast<size_t>(id - 1);
	bool enabled = (strcmp(enabled_buf, "1") == 0 || strcmp(enabled_buf, "true") == 0 ||
			strcmp(enabled_buf, "on") == 0);
	bool use_adc_target = config.fan_use_adc_target[index];
	long percent = config.fan_percent[index];
	long target_rpm = -1;

	if (has_use_adc_target) {
		use_adc_target = (strcmp(use_adc_target_buf, "1") == 0 ||
				  strcmp(use_adc_target_buf, "true") == 0 ||
				  strcmp(use_adc_target_buf, "on") == 0);
	}
	if (has_percent) {
		percent = strtol(percent_buf, nullptr, 10);
	}
	if (has_target_rpm) {
		target_rpm = strtol(target_rpm_buf, nullptr, 10);
	}

	if (has_target_rpm && !use_adc_target) {
		if (target_rpm < 0) {
			return -EINVAL;
		}
		percent = fan_controller.GetCurves().EvaluateRpmToPercent(static_cast<int32_t>(target_rpm));
	}

	if (percent < 0 || percent > 100) {
		return -EINVAL;
	}

	return settings::SaveFanDefaults(index, enabled, static_cast<uint8_t>(percent), use_adc_target);
}

} // namespace

HttpServer::HttpServer(FanController &fan_controller, WifiManager &wifi_manager,
			       HostControlManager &host_control)
	: fan_controller_(fan_controller), wifi_manager_(wifi_manager), host_control_(host_control)
{
}

void HttpServer::Start()
{
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
	char buffer[kRecvBufferSize];
	char method[8] = {0};
	char target[160] = {0};
	int received = zsock_recv(client, buffer, sizeof(buffer) - 1, 0);

	if (received <= 0) {
		return;
	}

	buffer[received] = '\0';
	char *headers_end = strstr(buffer, "\r\n\r\n");
	if (headers_end == nullptr) {
		(void)SendResponse(client, "400 Bad Request", "text/plain", "Malformed request");
		return;
	}

	char *body = headers_end + 4;
	(void)sscanf(buffer, "%7s %159s", method, target);

	char *path = nullptr;
	char *query = nullptr;
	SplitTarget(target, &path, &query);

	char *content_length_hdr = strstr(buffer, "Content-Length:");
	int content_length = (content_length_hdr != nullptr)
				     ? atoi(content_length_hdr + strlen("Content-Length:"))
				     : 0;

	while ((buffer + received) - body < content_length &&
	       received < static_cast<int>(sizeof(buffer)) - 1) {
		int rc = zsock_recv(client, buffer + received, sizeof(buffer) - 1 - received, 0);
		if (rc <= 0) {
			break;
		}
		received += rc;
		buffer[received] = '\0';
	}

	if (strcmp(method, "GET") == 0 && strncmp(path, "/api/", 5) != 0) {
		char fs_path[96];
		const char *content_type = nullptr;

		if (storage::ResolveWebPath(path, fs_path, sizeof(fs_path), &content_type) &&
		    SendFileResponse(client, "200 OK", content_type, fs_path) == 0) {
			return;
		}

		(void)SendResponse(client, "404 Not Found", "text/plain", "Static file not found");
		return;
	}

	if (strcmp(method, "GET") == 0 && strcmp(path, "/api/status") == 0) {
		FanState fans[kFanCount];
		WifiSnapshot wifi = {};
		char ap_ssid[WIFI_SSID_MAX_LEN * 2U + 1U];
		char sta_ssid[WIFI_SSID_MAX_LEN * 2U + 1U];
		char response[kStatusBufferSize];

		fan_controller_.GetAllStates(fans);
		wifi_manager_.GetSnapshot(&wifi);
		JsonEscape(ap_ssid, sizeof(ap_ssid), wifi.ap_ssid);
		JsonEscape(sta_ssid, sizeof(sta_ssid), wifi.saved_ssid);

		(void)snprintf(response, sizeof(response),
			       "{\"ap\":{\"enabled\":%s,\"ssid\":\"%s\",\"psk\":\"%s\",\"ip\":\"%s\",\"clients\":%d},"
			       "\"sta\":{\"connected\":%s,\"ssid\":\"%s\",\"state\":\"%s\",\"rssi\":%d},"
			       "\"fans\":[{\"id\":1,\"enabled\":%s,\"use_adc_target\":%s,"
			       "\"percent\":%u,\"effective_percent\":%u,\"pwm_percent\":%u,"
			       "\"adc_target_percent\":%u,\"actual_percent\":%u,\"adc_raw\":%d,"
			       "\"adc_mv\":%d,\"mapped_voltage_mv\":%d,\"actual_rpm\":%d,"
			       "\"target_rpm\":%d,\"pwm_pulse_ns\":%u},"
			       "{\"id\":2,\"enabled\":%s,\"use_adc_target\":%s,"
			       "\"percent\":%u,\"effective_percent\":%u,\"pwm_percent\":%u,"
			       "\"adc_target_percent\":%u,\"actual_percent\":%u,\"adc_raw\":%d,"
			       "\"adc_mv\":%d,\"mapped_voltage_mv\":%d,\"actual_rpm\":%d,"
			       "\"target_rpm\":%d,\"pwm_pulse_ns\":%u}],"
			       "\"curves\":{\"adc_to_voltage\":\"%s\",\"voltage_to_percent\":\"%s\","
			       "\"percent_to_pwm\":\"%s\",\"percent_to_rpm\":\"%s\"}}",
			       wifi.ap_enabled ? "true" : "false", ap_ssid, kApPsk, kApIpAddr,
			       wifi.ap_clients, wifi.sta_connected ? "true" : "false", sta_ssid,
			       wifi.sta_state, wifi.sta_rssi,
			       fans[0].enabled ? "true" : "false",
			       fans[0].use_adc_target ? "true" : "false", fans[0].percent,
			       fans[0].effective_percent, fans[0].pwm_percent,
			       fans[0].adc_target_percent, fans[0].actual_percent, fans[0].adc_raw,
			       fans[0].adc_mv, fans[0].mapped_voltage_mv, fans[0].actual_rpm,
			       fans[0].target_rpm, static_cast<unsigned int>(fans[0].pwm_pulse_ns),
			       fans[1].enabled ? "true" : "false",
			       fans[1].use_adc_target ? "true" : "false", fans[1].percent,
			       fans[1].effective_percent, fans[1].pwm_percent,
			       fans[1].adc_target_percent, fans[1].actual_percent, fans[1].adc_raw,
			       fans[1].adc_mv, fans[1].mapped_voltage_mv, fans[1].actual_rpm,
			       fans[1].target_rpm, static_cast<unsigned int>(fans[1].pwm_pulse_ns),
			       curves::CurveProfiles::GetAdcToVoltagePath(),
			       curves::CurveProfiles::GetVoltageToPercentPath(),
			       curves::CurveProfiles::GetPercentToPwmPath(),
			       curves::CurveProfiles::GetPercentToRpmPath());

		(void)SendResponse(client, "200 OK", "application/json", response);
		return;
	}

	if (strcmp(method, "GET") == 0 && strcmp(path, "/api/config") == 0) {
		size_t out_len = 0U;
		int rc = settings::ReadConfigJson(g_large_buffer, sizeof(g_large_buffer), &out_len);

		if (rc != 0) {
			(void)SendJsonResult(client, false, "config read failed");
			return;
		}

		(void)SendResponseSized(client, "200 OK", "application/json; charset=utf-8",
					g_large_buffer, out_len);
		return;
	}

	if (strcmp(method, "POST") == 0 && strcmp(path, "/api/config") == 0) {
		int rc = settings::WriteConfigJson(body, static_cast<size_t>(content_length));
		if (rc == 0) {
			rc = ReloadAndApplyConfig(wifi_manager_, host_control_);
		}
		if (rc != 0) {
			(void)SendJsonResult(client, false, "config save failed");
			return;
		}

		size_t out_len = 0U;
		rc = settings::ReadConfigJson(g_large_buffer, sizeof(g_large_buffer), &out_len);
		if (rc != 0) {
			(void)SendJsonResult(client, false, "config reload failed");
			return;
		}

		(void)SendResponseSized(client, "200 OK", "application/json; charset=utf-8",
					g_large_buffer, out_len);
		return;
	}

	if (strcmp(method, "GET") == 0 && strcmp(path, "/api/config/fields") == 0) {
		size_t out_len = 0U;
		int rc = storage::ReadTextFile(settings::GetFieldDefinitionsRelativePath(), g_large_buffer,
					      sizeof(g_large_buffer), &out_len);
		if (rc != 0) {
			(void)SendJsonResult(client, false, "field definition read failed");
			return;
		}

		(void)SendResponseSized(client, "200 OK", "application/json; charset=utf-8",
					g_large_buffer, out_len);
		return;
	}

	if (strcmp(method, "GET") == 0 && strcmp(path, "/api/fs/list") == 0) {
		char requested[96];
		requested[0] = '/';
		requested[1] = '\0';
		(void)CopyKvValue(query, "path", requested, sizeof(requested));

		int rc = storage::ListDirectoryJson(requested, g_large_buffer, sizeof(g_large_buffer));
		if (rc != 0) {
			(void)SendJsonResult(client, false, "directory list failed");
			return;
		}

		(void)SendResponse(client, "200 OK", "application/json", g_large_buffer);
		return;
	}

	if (strcmp(method, "GET") == 0 && strcmp(path, "/api/fs/file") == 0) {
		char requested[96];

		if (!CopyKvValue(query, "path", requested, sizeof(requested))) {
			(void)SendJsonResult(client, false, "missing path");
			return;
		}

		size_t out_len = 0U;
		int rc = storage::ReadTextFile(requested, g_large_buffer, sizeof(g_large_buffer), &out_len);
		if (rc != 0) {
			(void)SendJsonResult(client, false, "file read failed");
			return;
		}

		(void)SendResponseSized(client, "200 OK", "text/plain; charset=utf-8", g_large_buffer,
					out_len);
		return;
	}

	if (strcmp(method, "POST") == 0 && strcmp(path, "/api/fs/file") == 0) {
		char requested[96];

		if (!CopyKvValue(query, "path", requested, sizeof(requested))) {
			(void)SendJsonResult(client, false, "missing path");
			return;
		}

		int rc = 0;
		if (strcmp(requested, settings::GetConfigRelativePath()) == 0) {
			rc = settings::WriteConfigJson(body, static_cast<size_t>(content_length));
			if (rc == 0) {
				rc = ReloadAndApplyConfig(wifi_manager_, host_control_);
			}
		} else if (strcmp(requested, settings::GetSshConfigRelativePath()) == 0) {
			rc = settings::WriteSshConfigJson(body, static_cast<size_t>(content_length));
		} else if (strcmp(requested, settings::GetNtpConfigRelativePath()) == 0) {
			rc = settings::WriteNtpConfigJson(body, static_cast<size_t>(content_length));
		} else {
			rc = storage::WriteTextFile(requested, body, static_cast<size_t>(content_length));
		}
		if (rc != 0) {
			(void)SendJsonResult(client, false, "file save failed");
			return;
		}

		(void)SendJsonResult(client, true, "file saved");
		return;
	}

	if (strcmp(method, "POST") == 0 && strcmp(path, "/api/fs/mkdir") == 0) {
		char requested[96];

		if (!CopyKvValue(query, "path", requested, sizeof(requested))) {
			(void)SendJsonResult(client, false, "missing path");
			return;
		}

		int rc = storage::MakeDirectory(requested);
		if (rc != 0) {
			(void)SendJsonResult(client, false, "mkdir failed");
			return;
		}

		(void)SendJsonResult(client, true, "directory created");
		return;
	}

	if (strcmp(method, "POST") == 0 && strcmp(path, "/api/fs/delete") == 0) {
		char requested[96];

		if (!CopyKvValue(query, "path", requested, sizeof(requested))) {
			(void)SendJsonResult(client, false, "missing path");
			return;
		}

		int rc = storage::DeletePath(requested);
		if (rc != 0) {
			(void)SendJsonResult(client, false, "delete failed");
			return;
		}

		(void)SendJsonResult(client, true, "deleted");
		return;
	}

	if (strcmp(method, "POST") == 0 && strcmp(path, "/api/fan/defaults") == 0) {
		int rc = SaveFanDefaultsFromRequest(fan_controller_, body);
		if (rc != 0) {
			(void)SendResponse(client, "400 Bad Request", "text/plain",
					   "Invalid fan default payload");
			return;
		}

		(void)SendResponse(client, "200 OK", "application/json", "{\"ok\":true}");
		return;
	}

	if (strcmp(method, "POST") == 0 && strcmp(path, "/api/fan") == 0) {
		char id_buf[8];
		char enabled_buf[8];
		char use_adc_target_buf[8];
		char percent_buf[8];
		char target_rpm_buf[12];
		bool has_percent = false;
		bool has_target_rpm = false;
		bool has_use_adc_target = false;

		if (!CopyKvValue(body, "id", id_buf, sizeof(id_buf)) ||
		    !CopyKvValue(body, "enabled", enabled_buf, sizeof(enabled_buf))) {
			(void)SendResponse(client, "400 Bad Request", "text/plain", "Invalid fan payload");
			return;
		}
		has_percent = CopyKvValue(body, "percent", percent_buf, sizeof(percent_buf));
		has_target_rpm = CopyKvValue(body, "target_rpm", target_rpm_buf, sizeof(target_rpm_buf));
		has_use_adc_target =
			CopyKvValue(body, "use_adc_target", use_adc_target_buf, sizeof(use_adc_target_buf));

		long id = strtol(id_buf, nullptr, 10);
		bool enabled = (strcmp(enabled_buf, "1") == 0 || strcmp(enabled_buf, "true") == 0 ||
				strcmp(enabled_buf, "on") == 0);
		FanState state = {};
		bool use_adc_target = false;
		long percent = -1;
		long target_rpm = -1;
		if (id >= 1 && id <= static_cast<long>(kFanCount)) {
			fan_controller_.GetState(static_cast<size_t>(id - 1), &state);
			use_adc_target = state.use_adc_target;
			percent = state.percent;
		}
		if (has_use_adc_target) {
			use_adc_target = (strcmp(use_adc_target_buf, "1") == 0 ||
					  strcmp(use_adc_target_buf, "true") == 0 ||
					  strcmp(use_adc_target_buf, "on") == 0);
		}
		if (has_percent) {
			percent = strtol(percent_buf, nullptr, 10);
		}
		if (has_target_rpm) {
			target_rpm = strtol(target_rpm_buf, nullptr, 10);
		}

		int rc = 0;
		if (id < 1 || id > static_cast<long>(kFanCount)) {
			(void)SendResponse(client, "400 Bad Request", "text/plain", "Invalid fan payload");
			return;
		}
		if (has_target_rpm && !use_adc_target) {
			if (target_rpm < 0) {
				(void)SendResponse(client, "400 Bad Request", "text/plain",
						   "Invalid fan payload");
				return;
			}
			rc = fan_controller_.ConfigureFanTargetRpm(static_cast<size_t>(id - 1),
								    static_cast<int32_t>(target_rpm),
								    enabled, false);
		} else {
			if (percent < 0 || percent > 100) {
				(void)SendResponse(client, "400 Bad Request", "text/plain",
						   "Invalid fan payload");
				return;
			}
			rc = fan_controller_.ConfigureFan(static_cast<size_t>(id - 1),
							  static_cast<uint8_t>(percent), enabled,
							  use_adc_target, false);
		}
		if (rc != 0) {
			(void)SendResponse(client, "400 Bad Request", "text/plain", "Invalid fan payload");
			return;
		}

		(void)SendResponse(client, "200 OK", "application/json", "{\"ok\":true}");
		return;
	}

	if (strcmp(method, "POST") == 0 && strcmp(path, "/api/wifi") == 0) {
		char ssid[WIFI_SSID_MAX_LEN + 1];
		char psk[WIFI_PSK_MAX_LEN + 1];

		psk[0] = '\0';
		if (!CopyKvValue(body, "ssid", ssid, sizeof(ssid)) ||
		    wifi_manager_.SaveAndConnect(ssid,
						 CopyKvValue(body, "psk", psk, sizeof(psk)) ? psk : "") != 0) {
			(void)SendResponse(client, "400 Bad Request", "text/plain",
					   "Invalid Wi-Fi payload");
			return;
		}

		(void)SendResponse(client, "200 OK", "application/json", "{\"ok\":true}");
		return;
	}

	if (strcmp(method, "GET") == 0 && strcmp(path, "/api/ntp") == 0) {
		size_t out_len = 0U;
		int rc = settings::ReadNtpConfigJson(g_large_buffer, sizeof(g_large_buffer), &out_len);
		if (rc != 0) {
			(void)SendJsonResult(client, false, "ntp config read failed");
			return;
		}

		(void)SendResponseSized(client, "200 OK", "application/json; charset=utf-8",
					g_large_buffer, out_len);
		return;
	}

	if (strcmp(method, "POST") == 0 && strcmp(path, "/api/ntp") == 0) {
		int rc = settings::WriteNtpConfigJson(body, static_cast<size_t>(content_length));
		if (rc != 0) {
			(void)SendJsonResult(client, false, "ntp config save failed");
			return;
		}

		size_t out_len = 0U;
		rc = settings::ReadNtpConfigJson(g_large_buffer, sizeof(g_large_buffer), &out_len);
		if (rc != 0) {
			(void)SendJsonResult(client, false, "ntp config reload failed");
			return;
		}

		(void)SendResponseSized(client, "200 OK", "application/json; charset=utf-8",
					g_large_buffer, out_len);
		return;
	}

	if (strcmp(method, "POST") == 0 && strcmp(path, "/api/wifi/scan") == 0) {
		int rc = wifi_manager_.StartScan();
		if (rc != 0) {
			(void)SendJsonResult(client, false, "scan failed");
			return;
		}

		// Wait for scan to complete (max 10 seconds)
		int timeout = 100;
		while (!wifi_manager_.IsScanComplete() && timeout-- > 0) {
			k_sleep(K_MSEC(100));
		}

		WifiScanResult results[16];
		size_t count = 0;
		wifi_manager_.GetScanResults(results, ARRAY_SIZE(results), &count);

		// Build JSON response
		char json[2048];
		int pos = snprintf(json, sizeof(json), "{\"ok\":true,\"networks\":[");
		
		for (size_t i = 0; i < count && pos < static_cast<int>(sizeof(json)) - 256; ++i) {
			const char *security = "unknown";
			switch (results[i].security) {
			case WIFI_SECURITY_TYPE_NONE: security = "open"; break;
			case WIFI_SECURITY_TYPE_PSK: security = "wpa2"; break;
			case WIFI_SECURITY_TYPE_PSK_SHA256: security = "wpa2"; break;
			case WIFI_SECURITY_TYPE_SAE: security = "wpa3"; break;
			default: break;
			}
			
			int written = snprintf(json + pos, sizeof(json) - pos,
				"%s{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%u,\"security\":\"%s\"}",
				(i > 0) ? "," : "",
				results[i].ssid,
				results[i].rssi,
				results[i].channel,
				security);
			pos += written;
		}
		
		snprintf(json + pos, sizeof(json) - pos, "]}");
		(void)SendResponse(client, "200 OK", "application/json", json);
		return;
	}

	if (strcmp(method, "POST") == 0 && strcmp(path, "/api/reboot") == 0) {
		(void)SendResponse(client, "200 OK", "application/json", "{\"ok\":true}");
		k_sleep(K_MSEC(100));
		sys_reboot(SYS_REBOOT_COLD);
		return;
	}

	(void)SendResponse(client, "404 Not Found", "text/plain", "Not found");
}

void HttpServer::Run()
{
	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(kHttpPort);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	int server = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server < 0) {
		LOG_ERR("socket create failed");
		return;
	}

	if (zsock_bind(server, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
		LOG_ERR("bind failed");
		(void)zsock_close(server);
		return;
	}

	if (zsock_listen(server, kHttpBacklog) != 0) {
		LOG_ERR("listen failed");
		(void)zsock_close(server);
		return;
	}

	LOG_INF("HTTP server listening on port %d", kHttpPort);

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
