/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "http_api.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

#include "core/common.hpp"
#include "curve_profiles.hpp"
#include "fan_controller.hpp"
#include "host_control_manager.hpp"
#include "settings_store.hpp"
#include "storage.hpp"
#include "wifi_manager.hpp"

namespace fanctl::http {

namespace {

int ApplyConfig(const settings::AppConfig &config, WifiManager &wifi_manager,
		HostControlManager &host_control)
{
	host_control.Configure(config);

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

bool HandleApiRequest(int client, const Request &request, char *scratch, size_t scratch_len,
		      const ServiceContext &services)
{
	FanController &fan_controller = *services.fan_controller;
	WifiManager &wifi_manager = *services.wifi_manager;
	HostControlManager &host_control = *services.host_control;
	const char *path = request.path;
	const char *query = request.query;
	const char *body = request.body;
	const size_t content_length = request.body_len;

	if (strcmp(request.method, "GET") == 0 && strcmp(path, "/api/status") == 0) {
		FanState fans[kFanCount];
		WifiSnapshot wifi = {};
		char ap_ssid[WIFI_SSID_MAX_LEN * 2U + 1U];
		char sta_ssid[WIFI_SSID_MAX_LEN * 2U + 1U];
		char response[kStatusBufferSize];

		fan_controller.GetAllStates(fans);
		wifi_manager.GetSnapshot(&wifi);
		JsonEscape(ap_ssid, sizeof(ap_ssid), wifi.ap_ssid);
		JsonEscape(sta_ssid, sizeof(sta_ssid), wifi.saved_ssid);

		(void)snprintf(response, sizeof(response),
			       "{\"ap\":{\"enabled\":%s,\"ssid\":\"%s\",\"psk\":\"%s\",\"ip\":\"%s\",\"clients\":%d},"
			       "\"sta\":{\"connected\":%s,\"ssid\":\"%s\",\"state\":\"%s\",\"ip\":\"%s\",\"rssi\":%d},"
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
			       wifi.sta_state, wifi.sta_ip, wifi.sta_rssi,
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
		return true;
	}

	if (strcmp(request.method, "GET") == 0 && strcmp(path, "/api/config") == 0) {
		size_t out_len = 0U;
		int rc = settings::ReadConfigJson(scratch, scratch_len, &out_len);

		if (rc != 0) {
			(void)SendJsonResult(client, false, "config read failed");
			return true;
		}

		(void)SendResponseSized(client, "200 OK", "application/json; charset=utf-8", scratch, out_len);
		return true;
	}

	if (strcmp(request.method, "POST") == 0 && strcmp(path, "/api/config") == 0) {
		int rc = settings::WriteConfigJson(body, content_length);
		if (rc == 0) {
			rc = ReloadAndApplyConfig(wifi_manager, host_control);
		}
		if (rc != 0) {
			(void)SendJsonResult(client, false, "config save failed");
			return true;
		}

		size_t out_len = 0U;
		rc = settings::ReadConfigJson(scratch, scratch_len, &out_len);
		if (rc != 0) {
			(void)SendJsonResult(client, false, "config reload failed");
			return true;
		}

		(void)SendResponseSized(client, "200 OK", "application/json; charset=utf-8", scratch, out_len);
		return true;
	}

	if (strcmp(request.method, "GET") == 0 && strcmp(path, "/api/config/fields") == 0) {
		size_t out_len = 0U;
		int rc = storage::ReadTextFile(settings::GetFieldDefinitionsRelativePath(), scratch,
					      scratch_len, &out_len);
		if (rc != 0) {
			(void)SendJsonResult(client, false, "field definition read failed");
			return true;
		}

		(void)SendResponseSized(client, "200 OK", "application/json; charset=utf-8", scratch, out_len);
		return true;
	}

	if (strcmp(request.method, "GET") == 0 && strcmp(path, "/api/fs/list") == 0) {
		char requested[96];
		requested[0] = '/';
		requested[1] = '\0';
		(void)CopyKvValue(query, "path", requested, sizeof(requested));

		int rc = storage::ListDirectoryJson(requested, scratch, scratch_len);
		if (rc != 0) {
			(void)SendJsonResult(client, false, "directory list failed");
			return true;
		}

		(void)SendResponse(client, "200 OK", "application/json", scratch);
		return true;
	}

	if (strcmp(request.method, "GET") == 0 && strcmp(path, "/api/fs/file") == 0) {
		char requested[96];

		if (!CopyKvValue(query, "path", requested, sizeof(requested))) {
			(void)SendJsonResult(client, false, "missing path");
			return true;
		}

		size_t out_len = 0U;
		int rc = storage::ReadTextFile(requested, scratch, scratch_len, &out_len);
		if (rc != 0) {
			(void)SendJsonResult(client, false, "file read failed");
			return true;
		}

		(void)SendResponseSized(client, "200 OK", "text/plain; charset=utf-8", scratch, out_len);
		return true;
	}

	if (strcmp(request.method, "POST") == 0 && strcmp(path, "/api/fs/file") == 0) {
		char requested[96];

		if (!CopyKvValue(query, "path", requested, sizeof(requested))) {
			(void)SendJsonResult(client, false, "missing path");
			return true;
		}

		int rc = 0;
		if (strcmp(requested, settings::GetConfigRelativePath()) == 0) {
			rc = settings::WriteConfigJson(body, content_length);
			if (rc == 0) {
				rc = ReloadAndApplyConfig(wifi_manager, host_control);
			}
		} else if (strcmp(requested, settings::GetSshConfigRelativePath()) == 0) {
			rc = settings::WriteSshConfigJson(body, content_length);
		} else if (strcmp(requested, settings::GetNtpConfigRelativePath()) == 0) {
			rc = settings::WriteNtpConfigJson(body, content_length);
		} else {
			rc = storage::WriteTextFile(requested, body, content_length);
		}
		if (rc != 0) {
			(void)SendJsonResult(client, false, "file save failed");
			return true;
		}

		(void)SendJsonResult(client, true, "file saved");
		return true;
	}

	if (strcmp(request.method, "POST") == 0 && strcmp(path, "/api/fs/mkdir") == 0) {
		char requested[96];

		if (!CopyKvValue(query, "path", requested, sizeof(requested))) {
			(void)SendJsonResult(client, false, "missing path");
			return true;
		}

		int rc = storage::MakeDirectory(requested);
		if (rc != 0) {
			(void)SendJsonResult(client, false, "mkdir failed");
			return true;
		}

		(void)SendJsonResult(client, true, "directory created");
		return true;
	}

	if (strcmp(request.method, "POST") == 0 && strcmp(path, "/api/fs/delete") == 0) {
		char requested[96];

		if (!CopyKvValue(query, "path", requested, sizeof(requested))) {
			(void)SendJsonResult(client, false, "missing path");
			return true;
		}

		int rc = storage::DeletePath(requested);
		if (rc != 0) {
			(void)SendJsonResult(client, false, "delete failed");
			return true;
		}

		(void)SendJsonResult(client, true, "deleted");
		return true;
	}

	if (strcmp(request.method, "POST") == 0 && strcmp(path, "/api/fan/defaults") == 0) {
		int rc = SaveFanDefaultsFromRequest(fan_controller, body);
		if (rc != 0) {
			(void)SendResponse(client, "400 Bad Request", "text/plain", "Invalid fan default payload");
			return true;
		}

		(void)SendResponse(client, "200 OK", "application/json", "{\"ok\":true}");
		return true;
	}

	if (strcmp(request.method, "POST") == 0 && strcmp(path, "/api/fan") == 0) {
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
			return true;
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
			fan_controller.GetState(static_cast<size_t>(id - 1), &state);
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
			return true;
		}
		if (has_target_rpm && !use_adc_target) {
			if (target_rpm < 0) {
				(void)SendResponse(client, "400 Bad Request", "text/plain", "Invalid fan payload");
				return true;
			}
			rc = fan_controller.ConfigureFanTargetRpm(static_cast<size_t>(id - 1),
							 static_cast<int32_t>(target_rpm), enabled, false);
		} else {
			if (percent < 0 || percent > 100) {
				(void)SendResponse(client, "400 Bad Request", "text/plain", "Invalid fan payload");
				return true;
			}
			rc = fan_controller.ConfigureFan(static_cast<size_t>(id - 1),
							 static_cast<uint8_t>(percent), enabled,
							 use_adc_target, false);
		}
		if (rc != 0) {
			(void)SendResponse(client, "400 Bad Request", "text/plain", "Invalid fan payload");
			return true;
		}

		(void)SendResponse(client, "200 OK", "application/json", "{\"ok\":true}");
		return true;
	}

	if (strcmp(request.method, "POST") == 0 && strcmp(path, "/api/wifi") == 0) {
		char ssid[WIFI_SSID_MAX_LEN + 1];
		char psk[WIFI_PSK_MAX_LEN + 1];

		psk[0] = '\0';
		if (!CopyKvValue(body, "ssid", ssid, sizeof(ssid)) ||
		    wifi_manager.SaveAndConnect(ssid,
						CopyKvValue(body, "psk", psk, sizeof(psk)) ? psk : "") != 0) {
			(void)SendResponse(client, "400 Bad Request", "text/plain", "Invalid Wi-Fi payload");
			return true;
		}

		(void)SendResponse(client, "200 OK", "application/json", "{\"ok\":true}");
		return true;
	}

	if (strcmp(request.method, "GET") == 0 && strcmp(path, "/api/ntp") == 0) {
		size_t out_len = 0U;
		int rc = settings::ReadNtpConfigJson(scratch, scratch_len, &out_len);
		if (rc != 0) {
			(void)SendJsonResult(client, false, "ntp config read failed");
			return true;
		}

		(void)SendResponseSized(client, "200 OK", "application/json; charset=utf-8", scratch, out_len);
		return true;
	}

	if (strcmp(request.method, "POST") == 0 && strcmp(path, "/api/ntp") == 0) {
		int rc = settings::WriteNtpConfigJson(body, content_length);
		if (rc != 0) {
			(void)SendJsonResult(client, false, "ntp config save failed");
			return true;
		}

		size_t out_len = 0U;
		rc = settings::ReadNtpConfigJson(scratch, scratch_len, &out_len);
		if (rc != 0) {
			(void)SendJsonResult(client, false, "ntp config reload failed");
			return true;
		}

		(void)SendResponseSized(client, "200 OK", "application/json; charset=utf-8", scratch, out_len);
		return true;
	}

	if (strcmp(request.method, "POST") == 0 && strcmp(path, "/api/wifi/scan") == 0) {
		int rc = wifi_manager.StartScan();
		if (rc != 0) {
			(void)SendJsonResult(client, false, "scan failed");
			return true;
		}

		int timeout = 100;
		while (!wifi_manager.IsScanComplete() && timeout-- > 0) {
			k_sleep(K_MSEC(100));
		}

		WifiScanResult results[16];
		size_t count = 0;
		wifi_manager.GetScanResults(results, ARRAY_SIZE(results), &count);

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
					       (i > 0) ? "," : "", results[i].ssid, results[i].rssi,
					       results[i].channel, security);
			pos += written;
		}

		(void)snprintf(json + pos, sizeof(json) - pos, "]}");
		(void)SendResponse(client, "200 OK", "application/json", json);
		return true;
	}

	if (strcmp(request.method, "POST") == 0 && strcmp(path, "/api/reboot") == 0) {
		(void)SendResponse(client, "200 OK", "application/json", "{\"ok\":true}");
		k_sleep(K_MSEC(100));
		sys_reboot(SYS_REBOOT_COLD);
		return true;
	}

	return false;
}

} // namespace fanctl::http
