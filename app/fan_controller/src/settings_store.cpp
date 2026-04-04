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
  "wifi.sta_ssid": {
    "type": "string",
    "default": "",
    "max_length": 32,
    "description": "Saved STA SSID."
  },
  "wifi.sta_psk": {
    "type": "string",
    "default": "",
    "max_length": 64,
    "description": "Saved STA password."
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
  "host_key_path": "/etc/ssh/ssh_host_ecdsa_key.der",
  "username": "root",
  "password": "123456",
  "allow_password_auth": true,
  "allow_public_key_auth": true,
  "authorized_keys_path": "/root/.ssh/authorized_keys"
})json";

constexpr const char *kAuthorizedKeysTemplate =
	"# Add one OpenSSH public key per line.\n"
	"# Example:\n"
	"# ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAA... laptop\n";

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

	config->wifi_ssid[0] = '\0';
	config->wifi_psk[0] = '\0';
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
		       "/etc/ssh/ssh_host_ecdsa_key.der");
	(void)snprintf(config->username, sizeof(config->username), "root");
	(void)snprintf(config->password, sizeof(config->password), "123456");
	config->allow_password_auth = true;
	config->allow_public_key_auth = true;
	(void)snprintf(config->authorized_keys_path, sizeof(config->authorized_keys_path), "%s",
		       kAuthorizedKeysRelativePath);
}

int BuildJson(const AppConfig *config, char *json, size_t json_len)
{
	if (config == nullptr || json == nullptr || json_len == 0U) {
		return -EINVAL;
	}

	char ssid[(WIFI_SSID_MAX_LEN * 2U) + 8U];
	char psk[(WIFI_PSK_MAX_LEN * 2U) + 8U];

	EscapeJsonString(config->wifi_ssid, ssid, sizeof(ssid));
	EscapeJsonString(config->wifi_psk, psk, sizeof(psk));

	int written = snprintf(
		json, json_len,
		"{\n"
		"  \"version\": 1,\n"
		"  \"wifi\": {\n"
		"    \"sta_ssid\": \"%s\",\n"
		"    \"sta_psk\": \"%s\"\n"
		"  },\n"
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
		ssid, psk, config->host_alive_check_enabled ? "true" : "false",
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

int ParseJson(const char *json, AppConfig *config)
{
	if (json == nullptr || config == nullptr) {
		return -EINVAL;
	}

	AppConfig parsed = {};
	FillDefaults(&parsed);

	const char *wifi_section = nullptr;
	const char *fan1_section = nullptr;
	const char *fan2_section = nullptr;
	const char *value = nullptr;

	if (!FindJsonKey(json, "wifi", &wifi_section) || !FindJsonKey(wifi_section, "sta_ssid", &value) ||
	    !ParseJsonStringAt(value, parsed.wifi_ssid, sizeof(parsed.wifi_ssid))) {
		return -EINVAL;
	}

	if (!FindJsonKey(wifi_section, "sta_psk", &value) ||
	    !ParseJsonStringAt(value, parsed.wifi_psk, sizeof(parsed.wifi_psk))) {
		return -EINVAL;
	}

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
		AppConfig defaults = {};
		char json[512];

		FillDefaults(&defaults);
		int rc = BuildJson(&defaults, json, sizeof(json));
		if (rc != 0) {
			return rc;
		}

		rc = storage::WriteTextFile(kConfigRelativePath, json, strlen(json));
		if (rc != 0) {
			return rc;
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

} // namespace

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
	AppConfig config = {};

	if (LoadConfig(&config) != 0) {
		FillDefaults(&config);
	}

	(void)snprintf(config.wifi_ssid, sizeof(config.wifi_ssid), "%s", ssid != nullptr ? ssid : "");
	(void)snprintf(config.wifi_psk, sizeof(config.wifi_psk), "%s", psk != nullptr ? psk : "");
	(void)SaveConfig(&config);
}

void ClearWifiCredentials()
{
	SaveWifiCredentials("", "");
}

int LoadWifiCredentials(char *ssid, size_t ssid_len, char *psk, size_t psk_len)
{
	AppConfig config = {};
	int rc = LoadConfig(&config);

	if (rc != 0) {
		return rc;
	}

	if (ssid != nullptr && ssid_len > 0U) {
		(void)snprintf(ssid, ssid_len, "%s", config.wifi_ssid);
	}

	if (psk != nullptr && psk_len > 0U) {
		(void)snprintf(psk, psk_len, "%s", config.wifi_psk);
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

} // namespace fanctl::settings
