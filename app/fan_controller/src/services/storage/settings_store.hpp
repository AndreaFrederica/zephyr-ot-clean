/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_SETTINGS_STORE_HPP_
#define FAN_CONTROLLER_SETTINGS_STORE_HPP_

#include <stddef.h>
#include <stdint.h>

#include "core/common.hpp"

namespace fanctl::settings {

constexpr size_t kMaxNtpServers = 6;

struct AppConfig {
	bool fan_enabled[kFanCount];
	uint8_t fan_percent[kFanCount];
	bool fan_use_adc_target[kFanCount];
	bool fan_pwm_inverted[kFanCount];
	uint8_t fan_pwm_min_percent[kFanCount];
	uint8_t fan_pwm_max_percent[kFanCount];
	bool host_alive_check_enabled;
	uint32_t host_alive_timeout_ms;
};

struct SshConfig {
	bool enabled;
	int listen_port;
	char host_key_path[128];
	char username[32];
	char password[64];
	bool allow_password_auth;
	bool allow_public_key_auth;
	char authorized_keys_path[128];
};

struct NtpConfig {
	bool enabled;
	bool use_dhcp_server;
	char servers[kMaxNtpServers][64];
	size_t server_count;
	int port;
	int sync_interval_hours;
};

struct WifiConfig {
	char sta_ssid[WIFI_SSID_MAX_LEN + 1];
	char sta_psk[WIFI_PSK_MAX_LEN + 1];
	bool ap_enabled;
	char ap_ssid_prefix[16];
	bool ap_ssid_use_mac_suffix;
	char ap_ssid_custom[33];
	int ap_channel;
	bool ap_hidden;
	int ap_max_clients;
	char dhcp_hostname[32];
	bool sta_auto_reconnect;
	int sta_scan_threshold;
};

int Init();
void GetDefaultConfig(AppConfig *config);
int LoadConfig(AppConfig *config);
int SaveConfig(const AppConfig *config);
int ReadConfigJson(char *json, size_t json_len, size_t *out_len);
int WriteConfigJson(const char *json, size_t json_len);
int LoadSshConfig(SshConfig *config);
int ReadSshConfigJson(char *json, size_t json_len, size_t *out_len);
int WriteSshConfigJson(const char *json, size_t json_len);
int LoadNtpConfig(NtpConfig *config);
int ReadNtpConfigJson(char *json, size_t json_len, size_t *out_len);
int WriteNtpConfigJson(const char *json, size_t json_len);
const char *GetNtpConfigRelativePath();

void FillWifiDefaults(WifiConfig *config);
int LoadWifiConfig(WifiConfig *config);
int ReadWifiConfigJson(char *json, size_t json_len, size_t *out_len);
int WriteWifiConfigJson(const char *json, size_t json_len);
const char *GetWifiConfigRelativePath();
void SaveWifiCredentials(const char *ssid, const char *psk);
void ClearWifiCredentials();
int LoadWifiCredentials(char *ssid, size_t ssid_len, char *psk, size_t psk_len);
const char *GetConfigRelativePath();
const char *GetFieldDefinitionsRelativePath();
const char *GetSshConfigRelativePath();
const char *GetAuthorizedKeysRelativePath();
int LoadFanState(size_t index, bool *enabled, uint8_t *percent);
void SaveFanState(size_t index, bool enabled, uint8_t percent);
int LoadFanAdcTargetMode(size_t index, bool *use_adc_target);
void SaveFanAdcTargetMode(size_t index, bool use_adc_target);
int SaveFanDefaults(size_t index, bool enabled, uint8_t percent, bool use_adc_target);
int SaveFanPwmConfig(size_t index, bool inverted, uint8_t min_percent, uint8_t max_percent);
int FactoryReset();

} // namespace fanctl::settings

#endif
