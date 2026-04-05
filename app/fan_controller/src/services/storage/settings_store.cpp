/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "settings_store.hpp"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include "generated/config_assets.hpp"
#include "generated/default_curve_assets.hpp"
#include "storage.hpp"

namespace fanctl::settings {

namespace {

constexpr const char *kConfigRelativePath = "/etc/fanctl/config.json";
constexpr const char *kLegacyConfigRelativePath = "/settings/fanctl.json";
constexpr const char *kFieldDefinitionsRelativePath = "/etc/fanctl/config.fields.json";
constexpr const char *kLegacyFieldDefinitionsRelativePath = "/settings/config_fields.json";
constexpr const char *kSshConfigRelativePath = "/etc/ssh/sshd_config.json";
constexpr const char *kLegacySshConfigRelativePath = "/settings/ssh_config.json";
constexpr const char *kAuthorizedKeysRelativePath = "/root/.ssh/authorized_keys";

constexpr const char *kFieldDefinitionsJson = R"json({
  "version": {
    "type": "integer",
    "default": 1,
    "readonly": true,
    "description": "Configuration schema version."
  },
  "fans.fan1.enabled": {
    "type": "boolean",
    "default": true,
    "description": "Fan 1 boot-time power state."
  },
  "fans.fan1.percent": {
    "type": "integer",
    "default": 40,
    "minimum": 0,
    "maximum": 100,
    "description": "Fan 1 boot-time manual target percent."
  },
  "fans.fan1.use_adc_target": {
    "type": "boolean",
    "default": false,
    "description": "Use calibrated ADC feedback voltage to derive fan 1 target percent."
  },
  "fans.fan2.enabled": {
    "type": "boolean",
    "default": true,
    "description": "Fan 2 boot-time power state."
  },
  "fans.fan2.percent": {
    "type": "integer",
    "default": 40,
    "minimum": 0,
    "maximum": 100,
    "description": "Fan 2 boot-time manual target percent."
  },
  "fans.fan2.use_adc_target": {
    "type": "boolean",
    "default": false,
    "description": "Use calibrated ADC feedback voltage to derive fan 2 target percent."
  },
  "host.alive_check_enabled": {
    "type": "boolean",
    "default": false,
    "description": "When host-controlled mode is active, require periodic keepalive from the upper computer."
  },
  "host.alive_timeout_ms": {
    "type": "integer",
    "default": 5000,
    "minimum": 500,
    "maximum": 600000,
    "description": "Upper computer keepalive timeout in milliseconds."
  }
})json";

constexpr const char *kSshConfigJson = R"json({
  "enabled": true,
  "listen_port": 22,
  "host_key_path": "/etc/ssh/ssh_host_ecdsa_key.pem",
  "username": "root",
  "password": "123456",
  "allow_password_auth": true,
  "allow_public_key_auth": true,
  "authorized_keys_path": "/root/.ssh/authorized_keys"
})json";

constexpr const char *kNtpConfigRelativePath = "/etc/fanctl/ntp.json";
constexpr const char *kWifiConfigRelativePath = "/etc/fanctl/wifi.json";

const char *GetDefaultConfigContent(const char *path, size_t *out_size)
{
	for (size_t i = 0; i < kConfigAssetCount; ++i) {
		if (strcmp(path, kConfigAssets[i].path) == 0) {
			*out_size = kConfigAssets[i].size;
			return reinterpret_cast<const char *>(kConfigAssets[i].data);
		}
	}
	*out_size = 0;
	return nullptr;
}

constexpr const char *kAuthorizedKeysTemplate =
	"# Add one OpenSSH public key per line.\n"
	"# Example:\n"
	"# ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAA... laptop\n";
constexpr const char *kSettingsFileRelativePath = "/var/lib/fanctl/settings.bin";

void SkipWhitespace(const char **cursor)
{
	while (**cursor != '\0' && isspace(static_cast<unsigned char>(**cursor)) != 0) {
		++(*cursor);
	}
}

bool FindJsonKey(const char *json, const char *key, const char **value_pos)
{
	char pattern[64];

	(void)snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	const char *found = strstr(json, pattern);
	if (found == nullptr) {
		return false;
	}

	found = strchr(found + strlen(pattern), ':');
	if (found == nullptr) {
		return false;
	}

	++found;
	SkipWhitespace(&found);
	*value_pos = found;
	return true;
}

bool ParseJsonStringAt(const char *value_pos, char *out, size_t out_len)
{
	if (value_pos == nullptr || out == nullptr || out_len == 0U || *value_pos != '"') {
		return false;
	}

	size_t pos = 0U;
	++value_pos;

	while (*value_pos != '\0' && *value_pos != '"' && pos + 1U < out_len) {
		if (*value_pos == '\\') {
			++value_pos;
			if (*value_pos == '\0') {
				return false;
			}

			switch (*value_pos) {
			case '"':
			case '\\':
			case '/':
				out[pos++] = *value_pos;
				break;
			case 'n':
				out[pos++] = '\n';
				break;
			case 'r':
				out[pos++] = '\r';
				break;
			case 't':
				out[pos++] = '\t';
				break;
			default:
				return false;
			}
			++value_pos;
			continue;
		}

		out[pos++] = *value_pos++;
	}

	if (*value_pos != '"') {
		return false;
	}

	out[pos] = '\0';
	return true;
}

bool ParseJsonBoolAt(const char *value_pos, bool *out)
{
	if (value_pos == nullptr || out == nullptr) {
		return false;
	}

	if (strncmp(value_pos, "true", 4) == 0) {
		*out = true;
		return true;
	}

	if (strncmp(value_pos, "false", 5) == 0) {
		*out = false;
		return true;
	}

	return false;
}

bool ParseJsonIntAt(const char *value_pos, int *out)
{
	if (value_pos == nullptr || out == nullptr) {
		return false;
	}

	char *end = nullptr;
	long value = strtol(value_pos, &end, 10);
	if (end == value_pos) {
		return false;
	}

	*out = static_cast<int>(value);
	return true;
}

bool ParseJsonStringArrayAt(const char *value_pos, char out[][64], size_t max_items, size_t *out_count)
{
	if (value_pos == nullptr || out == nullptr || out_count == nullptr) {
		return false;
	}

	const char *cursor = value_pos;
	SkipWhitespace(&cursor);
	if (*cursor != '[') {
		return false;
	}

	++cursor;
	*out_count = 0U;

	while (true) {
		SkipWhitespace(&cursor);
		if (*cursor == ']') {
			++cursor;
			return true;
		}

		if (*out_count >= max_items) {
			return false;
		}

		if (!ParseJsonStringAt(cursor, out[*out_count], 64)) {
			return false;
		}
		++(*out_count);

		++cursor;
		while (*cursor != '\0') {
			if (*cursor == '"' && cursor[-1] != '\\') {
				++cursor;
				break;
			}
			++cursor;
		}

		SkipWhitespace(&cursor);
		if (*cursor == ',') {
			++cursor;
			continue;
		}
		if (*cursor == ']') {
			++cursor;
			return true;
		}

		return false;
	}
}

void EscapeJsonString(const char *src, char *dst, size_t dst_len)
{
	size_t out = 0U;

	for (size_t i = 0U; src[i] != '\0' && out + 2U < dst_len; ++i) {
		switch (src[i]) {
		case '"':
		case '\\':
			dst[out++] = '\\';
			dst[out++] = src[i];
			break;
		case '\n':
			dst[out++] = '\\';
			dst[out++] = 'n';
			break;
		case '\r':
			dst[out++] = '\\';
			dst[out++] = 'r';
			break;
		case '\t':
			dst[out++] = '\\';
			dst[out++] = 't';
			break;
		default:
			if (static_cast<unsigned char>(src[i]) < 0x20U) {
				dst[out++] = '_';
			} else {
				dst[out++] = src[i];
			}
			break;
		}
	}

	dst[out] = '\0';
}

void FillDefaults(AppConfig *config)
{
	if (config == nullptr) {
		return;
	}

	for (size_t i = 0; i < kFanCount; ++i) {
		config->fan_enabled[i] = true;
		config->fan_percent[i] = 40;
		config->fan_use_adc_target[i] = false;
	}
	config->host_alive_check_enabled = false;
	config->host_alive_timeout_ms = 5000U;
}

void FillSshDefaults(SshConfig *config)
{
	if (config == nullptr) {
		return;
	}

	config->enabled = true;
	config->listen_port = 22;
	(void)snprintf(config->host_key_path, sizeof(config->host_key_path),
		       "/etc/ssh/ssh_host_ecdsa_key.pem");
	(void)snprintf(config->username, sizeof(config->username), "root");
	(void)snprintf(config->password, sizeof(config->password), "123456");
	config->allow_password_auth = true;
	config->allow_public_key_auth = true;
	(void)snprintf(config->authorized_keys_path, sizeof(config->authorized_keys_path), "%s",
		       kAuthorizedKeysRelativePath);
}

void FillNtpDefaults(NtpConfig *config)
{
	if (config == nullptr) {
		return;
	}

	config->enabled = true;
	config->use_dhcp_server = true;
	config->server_count = 4U;
	(void)snprintf(config->servers[0], sizeof(config->servers[0]), "223.5.5.5");
	(void)snprintf(config->servers[1], sizeof(config->servers[1]), "114.114.114.114");
	(void)snprintf(config->servers[2], sizeof(config->servers[2]), "120.25.115.20");
	(void)snprintf(config->servers[3], sizeof(config->servers[3]), "pool.ntp.org");
	config->port = 123;
	config->sync_interval_hours = 24;
}


int BuildJson(const AppConfig *config, char *json, size_t json_len)
{
	if (config == nullptr || json == nullptr || json_len == 0U) {
		return -EINVAL;
	}

	int written = snprintf(
		json, json_len,
		"{\n"
		"  \"version\": 1,\n"
		"  \"host\": {\n"
		"    \"alive_check_enabled\": %s,\n"
		"    \"alive_timeout_ms\": %u\n"
		"  },\n"
		"  \"fans\": {\n"
		"    \"fan1\": {\n"
		"      \"enabled\": %s,\n"
		"      \"percent\": %u,\n"
		"      \"use_adc_target\": %s\n"
		"    },\n"
		"    \"fan2\": {\n"
		"      \"enabled\": %s,\n"
		"      \"percent\": %u,\n"
		"      \"use_adc_target\": %s\n"
		"    }\n"
		"  }\n"
		"}\n",
		config->host_alive_check_enabled ? "true" : "false",
		static_cast<unsigned int>(config->host_alive_timeout_ms),
		config->fan_enabled[0] ? "true" : "false", config->fan_percent[0],
		config->fan_use_adc_target[0] ? "true" : "false",
		config->fan_enabled[1] ? "true" : "false", config->fan_percent[1],
		config->fan_use_adc_target[1] ? "true" : "false");

	return (written > 0 && static_cast<size_t>(written) < json_len) ? 0 : -ENOSPC;
}

int BuildSshJson(const SshConfig *config, char *json, size_t json_len)
{
	if (config == nullptr || json == nullptr || json_len == 0U) {
		return -EINVAL;
	}

	char host_key_path[192];
	char username[64];
	char password[128];
	char authorized_keys_path[192];

	EscapeJsonString(config->host_key_path, host_key_path, sizeof(host_key_path));
	EscapeJsonString(config->username, username, sizeof(username));
	EscapeJsonString(config->password, password, sizeof(password));
	EscapeJsonString(config->authorized_keys_path, authorized_keys_path,
			 sizeof(authorized_keys_path));

	int written = snprintf(
		json, json_len,
		"{\n"
		"  \"enabled\": %s,\n"
		"  \"listen_port\": %d,\n"
		"  \"host_key_path\": \"%s\",\n"
		"  \"username\": \"%s\",\n"
		"  \"password\": \"%s\",\n"
		"  \"allow_password_auth\": %s,\n"
		"  \"allow_public_key_auth\": %s,\n"
		"  \"authorized_keys_path\": \"%s\"\n"
		"}\n",
		config->enabled ? "true" : "false", config->listen_port, host_key_path, username,
		password, config->allow_password_auth ? "true" : "false",
		config->allow_public_key_auth ? "true" : "false", authorized_keys_path);

	return (written > 0 && static_cast<size_t>(written) < json_len) ? 0 : -ENOSPC;
}

int BuildNtpJson(const NtpConfig *config, char *json, size_t json_len)
{
	if (config == nullptr || json == nullptr || json_len == 0U) {
		return -EINVAL;
	}

	size_t offset = 0U;
	int written = snprintf(
		json, json_len,
		"{\n"
		"  \"enabled\": %s,\n"
		"  \"use_dhcp_server\": %s,\n"
		"  \"servers\": [",
		config->enabled ? "true" : "false", config->use_dhcp_server ? "true" : "false");
	if (written <= 0 || static_cast<size_t>(written) >= json_len) {
		return -ENOSPC;
	}
	offset = static_cast<size_t>(written);

	for (size_t i = 0; i < config->server_count; ++i) {
		char server[128];
		EscapeJsonString(config->servers[i], server, sizeof(server));
		written = snprintf(json + offset, json_len - offset, "%s\"%s\"",
				   i == 0U ? "" : ", ", server);
		if (written <= 0 || static_cast<size_t>(written) >= json_len - offset) {
			return -ENOSPC;
		}
		offset += static_cast<size_t>(written);
	}

	written = snprintf(
		json + offset, json_len - offset,
		"],\n"
		"  \"port\": %d,\n"
		"  \"sync_interval_hours\": %d\n"
		"}\n",
		config->port, config->sync_interval_hours);

	return (written > 0 && static_cast<size_t>(written) < json_len - offset) ? 0 : -ENOSPC;
}

int BuildWifiJson(const WifiConfig *config, char *json, size_t json_len)
{
	if (config == nullptr || json == nullptr || json_len == 0U) {
		return -EINVAL;
	}

	char ssid[(WIFI_SSID_MAX_LEN * 2U) + 8U];
	char psk[(WIFI_PSK_MAX_LEN * 2U) + 8U];
	char ap_ssid_custom[(WIFI_SSID_MAX_LEN * 2U) + 8U];
	char ap_ssid_prefix[32];
	char dhcp_hostname[64];

	EscapeJsonString(config->sta_ssid, ssid, sizeof(ssid));
	EscapeJsonString(config->sta_psk, psk, sizeof(psk));
	EscapeJsonString(config->ap_ssid_custom, ap_ssid_custom, sizeof(ap_ssid_custom));
	EscapeJsonString(config->ap_ssid_prefix, ap_ssid_prefix, sizeof(ap_ssid_prefix));
	EscapeJsonString(config->dhcp_hostname, dhcp_hostname, sizeof(dhcp_hostname));

	int written = snprintf(
		json, json_len,
		"{\n"
		"  \"sta_ssid\": \"%s\",\n"
		"  \"sta_psk\": \"%s\",\n"
		"  \"ap_enabled\": %s,\n"
		"  \"ap_ssid_prefix\": \"%s\",\n"
		"  \"ap_ssid_use_mac_suffix\": %s,\n"
		"  \"ap_ssid_custom\": \"%s\",\n"
		"  \"ap_channel\": %d,\n"
		"  \"ap_hidden\": %s,\n"
		"  \"ap_max_clients\": %d,\n"
		"  \"dhcp_hostname\": \"%s\",\n"
		"  \"sta_auto_reconnect\": %s,\n"
		"  \"sta_scan_threshold\": %d\n"
		"}\n",
		ssid, psk,
		config->ap_enabled ? "true" : "false",
		ap_ssid_prefix,
		config->ap_ssid_use_mac_suffix ? "true" : "false",
		ap_ssid_custom,
		config->ap_channel,
		config->ap_hidden ? "true" : "false",
		config->ap_max_clients,
		dhcp_hostname,
		config->sta_auto_reconnect ? "true" : "false",
		config->sta_scan_threshold);

	return (written > 0 && static_cast<size_t>(written) < json_len) ? 0 : -ENOSPC;
}

int ParseJson(const char *json, AppConfig *config)
{
	if (json == nullptr || config == nullptr) {
		return -EINVAL;
	}

	AppConfig parsed = {};
	FillDefaults(&parsed);

	const char *fan1_section = nullptr;
	const char *fan2_section = nullptr;
	const char *value = nullptr;

	const char *host_section = nullptr;
	int timeout_ms = 0;
	if (FindJsonKey(json, "host", &host_section)) {
		if (!FindJsonKey(host_section, "alive_check_enabled", &value) ||
		    !ParseJsonBoolAt(value, &parsed.host_alive_check_enabled) ||
		    !FindJsonKey(host_section, "alive_timeout_ms", &value) ||
		    !ParseJsonIntAt(value, &timeout_ms) || timeout_ms < 500 || timeout_ms > 600000) {
			return -EINVAL;
		}
		parsed.host_alive_timeout_ms = static_cast<uint32_t>(timeout_ms);
	}

	if (!FindJsonKey(json, "fan1", &fan1_section) || !FindJsonKey(fan1_section, "enabled", &value) ||
	    !ParseJsonBoolAt(value, &parsed.fan_enabled[0])) {
		return -EINVAL;
	}

	int percent = 0;
	if (!FindJsonKey(fan1_section, "percent", &value) || !ParseJsonIntAt(value, &percent) ||
	    percent < 0 || percent > 100) {
		return -EINVAL;
	}
	parsed.fan_percent[0] = static_cast<uint8_t>(percent);
	if (FindJsonKey(fan1_section, "use_adc_target", &value)) {
		if (!ParseJsonBoolAt(value, &parsed.fan_use_adc_target[0])) {
			return -EINVAL;
		}
	}

	if (!FindJsonKey(json, "fan2", &fan2_section) || !FindJsonKey(fan2_section, "enabled", &value) ||
	    !ParseJsonBoolAt(value, &parsed.fan_enabled[1])) {
		return -EINVAL;
	}

	if (!FindJsonKey(fan2_section, "percent", &value) || !ParseJsonIntAt(value, &percent) ||
	    percent < 0 || percent > 100) {
		return -EINVAL;
	}
	parsed.fan_percent[1] = static_cast<uint8_t>(percent);
	if (FindJsonKey(fan2_section, "use_adc_target", &value)) {
		if (!ParseJsonBoolAt(value, &parsed.fan_use_adc_target[1])) {
			return -EINVAL;
		}
	}

	*config = parsed;
	return 0;
}

int ParseSshJson(const char *json, SshConfig *config)
{
	if (json == nullptr || config == nullptr) {
		return -EINVAL;
	}

	SshConfig parsed = {};
	FillSshDefaults(&parsed);
	const char *value = nullptr;

	if (!FindJsonKey(json, "enabled", &value) || !ParseJsonBoolAt(value, &parsed.enabled)) {
		return -EINVAL;
	}

	if (!FindJsonKey(json, "listen_port", &value) || !ParseJsonIntAt(value, &parsed.listen_port) ||
	    parsed.listen_port < 1 || parsed.listen_port > 65535) {
		return -EINVAL;
	}

	if (!FindJsonKey(json, "host_key_path", &value) ||
	    !ParseJsonStringAt(value, parsed.host_key_path, sizeof(parsed.host_key_path)) ||
	    parsed.host_key_path[0] != '/') {
		return -EINVAL;
	}

	if (!FindJsonKey(json, "username", &value) ||
	    !ParseJsonStringAt(value, parsed.username, sizeof(parsed.username)) ||
	    parsed.username[0] == '\0') {
		return -EINVAL;
	}

	if (!FindJsonKey(json, "password", &value) ||
	    !ParseJsonStringAt(value, parsed.password, sizeof(parsed.password))) {
		return -EINVAL;
	}

	if (!FindJsonKey(json, "allow_password_auth", &value) ||
	    !ParseJsonBoolAt(value, &parsed.allow_password_auth)) {
		return -EINVAL;
	}

	if (!FindJsonKey(json, "allow_public_key_auth", &value) ||
	    !ParseJsonBoolAt(value, &parsed.allow_public_key_auth)) {
		return -EINVAL;
	}

	if (!FindJsonKey(json, "authorized_keys_path", &value) ||
	    !ParseJsonStringAt(value, parsed.authorized_keys_path,
			       sizeof(parsed.authorized_keys_path)) ||
	    parsed.authorized_keys_path[0] != '/') {
		return -EINVAL;
	}

	*config = parsed;
	return 0;
}

int ParseNtpJson(const char *json, NtpConfig *config)
{
	if (json == nullptr || config == nullptr) {
		return -EINVAL;
	}

	NtpConfig parsed = {};
	FillNtpDefaults(&parsed);
	const char *value = nullptr;

	if (!FindJsonKey(json, "enabled", &value) || !ParseJsonBoolAt(value, &parsed.enabled)) {
		return -EINVAL;
	}

	if (FindJsonKey(json, "use_dhcp_server", &value) &&
	    !ParseJsonBoolAt(value, &parsed.use_dhcp_server)) {
		return -EINVAL;
	}

	if (FindJsonKey(json, "servers", &value)) {
		if (!ParseJsonStringArrayAt(value, parsed.servers, ARRAY_SIZE(parsed.servers),
					    &parsed.server_count)) {
			return -EINVAL;
		}
	} else if (FindJsonKey(json, "server", &value)) {
		if (!ParseJsonStringAt(value, parsed.servers[0], sizeof(parsed.servers[0]))) {
			return -EINVAL;
		}
		parsed.server_count = 1U;
	}

	if (!FindJsonKey(json, "port", &value) || !ParseJsonIntAt(value, &parsed.port) ||
	    parsed.port < 1 || parsed.port > 65535) {
		return -EINVAL;
	}

	if (!FindJsonKey(json, "sync_interval_hours", &value) ||
	    !ParseJsonIntAt(value, &parsed.sync_interval_hours) ||
	    parsed.sync_interval_hours < 1 || parsed.sync_interval_hours > 720) {
		return -EINVAL;
	}

	if (!parsed.use_dhcp_server && parsed.server_count == 0U) {
		return -EINVAL;
	}

	*config = parsed;
	return 0;
}

int ParseWifiJson(const char *json, WifiConfig *config)
{
	if (json == nullptr || config == nullptr) {
		return -EINVAL;
	}

	WifiConfig parsed = {};
	FillWifiDefaults(&parsed);
	const char *value = nullptr;

	// sta_ssid is optional
	if (FindJsonKey(json, "sta_ssid", &value)) {
		if (!ParseJsonStringAt(value, parsed.sta_ssid, sizeof(parsed.sta_ssid))) {
			return -EINVAL;
		}
	}

	// sta_psk is optional
	if (FindJsonKey(json, "sta_psk", &value)) {
		if (!ParseJsonStringAt(value, parsed.sta_psk, sizeof(parsed.sta_psk))) {
			return -EINVAL;
		}
	}

	// ap_enabled is optional (defaults to true)
	if (FindJsonKey(json, "ap_enabled", &value)) {
		if (!ParseJsonBoolAt(value, &parsed.ap_enabled)) {
			return -EINVAL;
		}
	}

	// ap_ssid_prefix is optional
	if (FindJsonKey(json, "ap_ssid_prefix", &value)) {
		if (!ParseJsonStringAt(value, parsed.ap_ssid_prefix, sizeof(parsed.ap_ssid_prefix))) {
			return -EINVAL;
		}
	}

	// ap_ssid_use_mac_suffix is optional
	if (FindJsonKey(json, "ap_ssid_use_mac_suffix", &value)) {
		if (!ParseJsonBoolAt(value, &parsed.ap_ssid_use_mac_suffix)) {
			return -EINVAL;
		}
	}

	// ap_ssid_custom is optional
	if (FindJsonKey(json, "ap_ssid_custom", &value)) {
		if (!ParseJsonStringAt(value, parsed.ap_ssid_custom, sizeof(parsed.ap_ssid_custom))) {
			return -EINVAL;
		}
	}

	// ap_channel is optional
	if (FindJsonKey(json, "ap_channel", &value)) {
		if (!ParseJsonIntAt(value, &parsed.ap_channel) ||
		    parsed.ap_channel < 0 || parsed.ap_channel > 14) {
			return -EINVAL;
		}
	}

	// ap_hidden is optional
	if (FindJsonKey(json, "ap_hidden", &value)) {
		if (!ParseJsonBoolAt(value, &parsed.ap_hidden)) {
			return -EINVAL;
		}
	}

	// ap_max_clients is optional
	if (FindJsonKey(json, "ap_max_clients", &value)) {
		if (!ParseJsonIntAt(value, &parsed.ap_max_clients) ||
		    parsed.ap_max_clients < 1 || parsed.ap_max_clients > 10) {
			return -EINVAL;
		}
	}

	// dhcp_hostname is optional
	if (FindJsonKey(json, "dhcp_hostname", &value)) {
		if (!ParseJsonStringAt(value, parsed.dhcp_hostname, sizeof(parsed.dhcp_hostname)) ||
		    parsed.dhcp_hostname[0] == '\0') {
			return -EINVAL;
		}
	}

	// sta_auto_reconnect is optional
	if (FindJsonKey(json, "sta_auto_reconnect", &value)) {
		if (!ParseJsonBoolAt(value, &parsed.sta_auto_reconnect)) {
			return -EINVAL;
		}
	}

	// sta_scan_threshold is optional
	if (FindJsonKey(json, "sta_scan_threshold", &value)) {
		if (!ParseJsonIntAt(value, &parsed.sta_scan_threshold) ||
		    parsed.sta_scan_threshold < -100 || parsed.sta_scan_threshold > 0) {
			return -EINVAL;
		}
	}

	*config = parsed;
	return 0;
}

int MigrateLegacyFile(const char *legacy_path, const char *target_path)
{
	char current[1024];
	size_t current_len = 0U;

	if (storage::ReadTextFile(target_path, current, sizeof(current), &current_len) == 0) {
		return 0;
	}

	int rc = storage::ReadTextFile(legacy_path, current, sizeof(current), &current_len);
	if (rc != 0) {
		return rc;
	}

	return storage::WriteTextFile(target_path, current, current_len);
}

int EnsureTextFile(const char *path, const char *content)
{
	char existing[64];

	if (storage::ReadTextFile(path, existing, sizeof(existing), nullptr) == 0) {
		return 0;
	}

	return storage::WriteTextFile(path, content, strlen(content));
}

int EnsureConfigFiles()
{
	char current[1024];
	size_t current_len = 0U;

	(void)MigrateLegacyFile(kLegacyConfigRelativePath, kConfigRelativePath);
	(void)MigrateLegacyFile(kLegacySshConfigRelativePath, kSshConfigRelativePath);
	(void)MigrateLegacyFile(kLegacyFieldDefinitionsRelativePath, kFieldDefinitionsRelativePath);

	(void)storage::WriteTextFile(kFieldDefinitionsRelativePath, kFieldDefinitionsJson,
				     strlen(kFieldDefinitionsJson));

	if (storage::ReadTextFile(kSshConfigRelativePath, current, sizeof(current), &current_len) != 0) {
		int rc = storage::WriteTextFile(kSshConfigRelativePath, kSshConfigJson,
						strlen(kSshConfigJson));
		if (rc != 0) {
			return rc;
		}
	}

	if (storage::ReadTextFile(kConfigRelativePath, current, sizeof(current), &current_len) != 0) {
		size_t default_size = 0;
		const char *default_content = GetDefaultConfigContent("config.json", &default_size);
		if (default_content != nullptr) {
			int rc = storage::WriteTextFile(kConfigRelativePath, default_content, default_size);
			if (rc != 0) {
				return rc;
			}
		}
	}

	if (storage::ReadTextFile(kNtpConfigRelativePath, current, sizeof(current), &current_len) != 0) {
		size_t default_size = 0;
		const char *default_content = GetDefaultConfigContent("ntp.json", &default_size);
		if (default_content != nullptr) {
			int rc = storage::WriteTextFile(kNtpConfigRelativePath, default_content, default_size);
			if (rc != 0) {
				return rc;
			}
		}
	}

	if (storage::ReadTextFile(kWifiConfigRelativePath, current, sizeof(current), &current_len) != 0) {
		size_t default_size = 0;
		const char *default_content = GetDefaultConfigContent("wifi.json", &default_size);
		if (default_content != nullptr) {
			int rc = storage::WriteTextFile(kWifiConfigRelativePath, default_content, default_size);
			if (rc != 0) {
				return rc;
			}
		}
	}

	return EnsureTextFile(kAuthorizedKeysRelativePath, kAuthorizedKeysTemplate);
}

int SaveSshConfig(const SshConfig *config)
{
	char json[768];
	int rc = BuildSshJson(config, json, sizeof(json));

	if (rc != 0) {
		return rc;
	}

	return storage::WriteTextFile(kSshConfigRelativePath, json, strlen(json));
}

int SaveNtpConfig(const NtpConfig *config)
{
	char json[768];
	int rc = BuildNtpJson(config, json, sizeof(json));

	if (rc != 0) {
		return rc;
	}

	return storage::WriteTextFile(kNtpConfigRelativePath, json, strlen(json));
}

int SaveWifiConfig(const WifiConfig *config)
{
	char json[768];
	int rc = BuildWifiJson(config, json, sizeof(json));

	if (rc != 0) {
		return rc;
	}

	return storage::WriteTextFile(kWifiConfigRelativePath, json, strlen(json));
}

} // namespace

void FillWifiDefaults(WifiConfig *config)
{
	if (config == nullptr) {
		return;
	}

	config->sta_ssid[0] = '\0';
	config->sta_psk[0] = '\0';
	config->ap_enabled = true;
	(void)snprintf(config->ap_ssid_prefix, sizeof(config->ap_ssid_prefix), "fanctl");
	config->ap_ssid_use_mac_suffix = true;
	config->ap_ssid_custom[0] = '\0';
	config->ap_channel = 0;
	config->ap_hidden = false;
	config->ap_max_clients = 4;
	(void)snprintf(config->dhcp_hostname, sizeof(config->dhcp_hostname), "fanctl");
	config->sta_auto_reconnect = true;
	config->sta_scan_threshold = -80;
}

int Init()
{
	int rc = settings_subsys_init();
	if (rc != 0 && rc != -EALREADY) {
		return rc;
	}

	return EnsureConfigFiles();
}

void GetDefaultConfig(AppConfig *config)
{
	FillDefaults(config);
}

int LoadConfig(AppConfig *config)
{
	if (config == nullptr) {
		return -EINVAL;
	}

	char json[1024];
	size_t json_len = 0U;
	int rc = storage::ReadTextFile(kConfigRelativePath, json, sizeof(json), &json_len);
	if (rc != 0) {
		return rc;
	}

	return ParseJson(json, config);
}

int SaveConfig(const AppConfig *config)
{
	char json[1024];
	int rc = BuildJson(config, json, sizeof(json));

	if (rc != 0) {
		return rc;
	}

	return storage::WriteTextFile(kConfigRelativePath, json, strlen(json));
}

int ReadConfigJson(char *json, size_t json_len, size_t *out_len)
{
	return storage::ReadTextFile(kConfigRelativePath, json, json_len, out_len);
}

int WriteConfigJson(const char *json, size_t json_len)
{
	if (json == nullptr || json_len == 0U) {
		return -EINVAL;
	}

	char buffer[1024];
	if (json_len >= sizeof(buffer)) {
		return -ENOSPC;
	}

	memcpy(buffer, json, json_len);
	buffer[json_len] = '\0';

	AppConfig config = {};
	int rc = ParseJson(buffer, &config);
	if (rc != 0) {
		return rc;
	}

	return SaveConfig(&config);
}

int LoadSshConfig(SshConfig *config)
{
	if (config == nullptr) {
		return -EINVAL;
	}

	char json[768];
	size_t json_len = 0U;
	int rc = storage::ReadTextFile(kSshConfigRelativePath, json, sizeof(json), &json_len);
	if (rc != 0) {
		return rc;
	}

	return ParseSshJson(json, config);
}

int ReadSshConfigJson(char *json, size_t json_len, size_t *out_len)
{
	return storage::ReadTextFile(kSshConfigRelativePath, json, json_len, out_len);
}

int WriteSshConfigJson(const char *json, size_t json_len)
{
	if (json == nullptr || json_len == 0U) {
		return -EINVAL;
	}

	char buffer[768];
	if (json_len >= sizeof(buffer)) {
		return -ENOSPC;
	}

	memcpy(buffer, json, json_len);
	buffer[json_len] = '\0';

	SshConfig config = {};
	int rc = ParseSshJson(buffer, &config);
	if (rc != 0) {
		return rc;
	}

	return SaveSshConfig(&config);
}

void SaveWifiCredentials(const char *ssid, const char *psk)
{
	WifiConfig config = {};

	if (LoadWifiConfig(&config) != 0) {
		FillWifiDefaults(&config);
	}

	(void)snprintf(config.sta_ssid, sizeof(config.sta_ssid), "%s", ssid != nullptr ? ssid : "");
	(void)snprintf(config.sta_psk, sizeof(config.sta_psk), "%s", psk != nullptr ? psk : "");
	(void)SaveWifiConfig(&config);
}

void ClearWifiCredentials()
{
	SaveWifiCredentials("", "");
}

int LoadWifiCredentials(char *ssid, size_t ssid_len, char *psk, size_t psk_len)
{
	WifiConfig config = {};
	int rc = LoadWifiConfig(&config);

	if (rc != 0) {
		return rc;
	}

	if (ssid != nullptr && ssid_len > 0U) {
		(void)snprintf(ssid, ssid_len, "%s", config.sta_ssid);
	}

	if (psk != nullptr && psk_len > 0U) {
		(void)snprintf(psk, psk_len, "%s", config.sta_psk);
	}

	return 0;
}

const char *GetConfigRelativePath()
{
	return kConfigRelativePath;
}

const char *GetFieldDefinitionsRelativePath()
{
	return kFieldDefinitionsRelativePath;
}

const char *GetSshConfigRelativePath()
{
	return kSshConfigRelativePath;
}

const char *GetAuthorizedKeysRelativePath()
{
	return kAuthorizedKeysRelativePath;
}

const char *GetNtpConfigRelativePath()
{
	return kNtpConfigRelativePath;
}

int LoadNtpConfig(NtpConfig *config)
{
	if (config == nullptr) {
		return -EINVAL;
	}

	char json[768];
	size_t json_len = 0U;
	int rc = storage::ReadTextFile(kNtpConfigRelativePath, json, sizeof(json), &json_len);
	if (rc != 0) {
		return rc;
	}

	return ParseNtpJson(json, config);
}

int ReadNtpConfigJson(char *json, size_t json_len, size_t *out_len)
{
	return storage::ReadTextFile(kNtpConfigRelativePath, json, json_len, out_len);
}

int WriteNtpConfigJson(const char *json, size_t json_len)
{
	if (json == nullptr || json_len == 0U) {
		return -EINVAL;
	}

	char buffer[768];
	if (json_len >= sizeof(buffer)) {
		return -ENOSPC;
	}

	memcpy(buffer, json, json_len);
	buffer[json_len] = '\0';

	NtpConfig config = {};
	int rc = ParseNtpJson(buffer, &config);
	if (rc != 0) {
		return rc;
	}

	return SaveNtpConfig(&config);
}

const char *GetWifiConfigRelativePath()
{
	return kWifiConfigRelativePath;
}

int LoadWifiConfig(WifiConfig *config)
{
	if (config == nullptr) {
		return -EINVAL;
	}

	char json[768];
	size_t json_len = 0U;
	int rc = storage::ReadTextFile(kWifiConfigRelativePath, json, sizeof(json), &json_len);
	if (rc != 0) {
		return rc;
	}

	return ParseWifiJson(json, config);
}

int ReadWifiConfigJson(char *json, size_t json_len, size_t *out_len)
{
	return storage::ReadTextFile(kWifiConfigRelativePath, json, json_len, out_len);
}

int WriteWifiConfigJson(const char *json, size_t json_len)
{
	if (json == nullptr || json_len == 0U) {
		return -EINVAL;
	}

	char buffer[768];
	if (json_len >= sizeof(buffer)) {
		return -ENOSPC;
	}

	memcpy(buffer, json, json_len);
	buffer[json_len] = '\0';

	WifiConfig config = {};
	int rc = ParseWifiJson(buffer, &config);
	if (rc != 0) {
		return rc;
	}

	return SaveWifiConfig(&config);
}

int LoadFanState(size_t index, bool *enabled, uint8_t *percent)
{
	if (index >= kFanCount) {
		return -EINVAL;
	}

	AppConfig config = {};
	int rc = LoadConfig(&config);

	if (rc != 0) {
		return rc;
	}

	if (enabled != nullptr) {
		*enabled = config.fan_enabled[index];
	}

	if (percent != nullptr) {
		*percent = config.fan_percent[index];
	}

	return 0;
}

void SaveFanState(size_t index, bool enabled, uint8_t percent)
{
	ARG_UNUSED(enabled);

	if (index >= kFanCount) {
		return;
	}

	AppConfig config = {};

	if (LoadConfig(&config) != 0) {
		FillDefaults(&config);
	}

	config.fan_percent[index] = MIN(percent, 100U);
	(void)SaveConfig(&config);
}

int LoadFanAdcTargetMode(size_t index, bool *use_adc_target)
{
	if (index >= kFanCount || use_adc_target == nullptr) {
		return -EINVAL;
	}

	AppConfig config = {};
	int rc = LoadConfig(&config);
	if (rc != 0) {
		return rc;
	}

	*use_adc_target = config.fan_use_adc_target[index];
	return 0;
}

void SaveFanAdcTargetMode(size_t index, bool use_adc_target)
{
	if (index >= kFanCount) {
		return;
	}

	AppConfig config = {};
	if (LoadConfig(&config) != 0) {
		FillDefaults(&config);
	}

	config.fan_use_adc_target[index] = use_adc_target;
	(void)SaveConfig(&config);
}

int SaveFanDefaults(size_t index, bool enabled, uint8_t percent, bool use_adc_target)
{
	if (index >= kFanCount) {
		return -EINVAL;
	}

	AppConfig config = {};
	if (LoadConfig(&config) != 0) {
		FillDefaults(&config);
	}

	config.fan_enabled[index] = enabled;
	config.fan_percent[index] = MIN(percent, 100U);
	config.fan_use_adc_target[index] = use_adc_target;
	return SaveConfig(&config);
}

int FactoryReset()
{
	SshConfig previous_ssh = {};
	(void)LoadSshConfig(&previous_ssh);

	AppConfig app_config = {};
	FillDefaults(&app_config);
	int rc = SaveConfig(&app_config);
	if (rc != 0) {
		return rc;
	}

	SshConfig ssh_config = {};
	FillSshDefaults(&ssh_config);
	rc = SaveSshConfig(&ssh_config);
	if (rc != 0) {
		return rc;
	}

	NtpConfig ntp_config = {};
	FillNtpDefaults(&ntp_config);
	rc = SaveNtpConfig(&ntp_config);
	if (rc != 0) {
		return rc;
	}

	WifiConfig wifi_config = {};
	FillWifiDefaults(&wifi_config);
	rc = SaveWifiConfig(&wifi_config);
	if (rc != 0) {
		return rc;
	}

	rc = storage::WriteTextFile(kFieldDefinitionsRelativePath, kFieldDefinitionsJson,
				    strlen(kFieldDefinitionsJson));
	if (rc != 0) {
		return rc;
	}

	rc = storage::WriteTextFile(kAuthorizedKeysRelativePath, kAuthorizedKeysTemplate,
				    strlen(kAuthorizedKeysTemplate));
	if (rc != 0) {
		return rc;
	}

	for (size_t i = 0; i < curves::kDefaultCurveAssetCount; ++i) {
		rc = storage::WriteTextFile(curves::kDefaultCurveAssets[i].path,
					    reinterpret_cast<const char *>(curves::kDefaultCurveAssets[i].data),
					    curves::kDefaultCurveAssets[i].size);
		if (rc != 0) {
			return rc;
		}
	}

	if (previous_ssh.host_key_path[0] != '\0') {
		rc = storage::DeletePath(previous_ssh.host_key_path);
		if (rc != 0 && rc != -ENOENT) {
			return rc;
		}
	}

	rc = storage::DeletePath(ssh_config.host_key_path);
	if (rc != 0 && rc != -ENOENT) {
		return rc;
	}

	rc = storage::DeletePath(kSettingsFileRelativePath);
	if (rc != 0 && rc != -ENOENT) {
		return rc;
	}

	return 0;
}

} // namespace fanctl::settings
