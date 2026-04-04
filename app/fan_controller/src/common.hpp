/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_COMMON_HPP_
#define FAN_CONTROLLER_COMMON_HPP_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/net/wifi.h>

namespace fanctl {

constexpr size_t kFanCount = 2;
constexpr int kHttpPort = 80;
constexpr int kHttpStackSize = 16384;
constexpr int kSshStackSize = 24576;
constexpr int kTelemetryStackSize = 4096;
constexpr const char *kApIpAddr = "192.168.4.1";
constexpr const char *kApNetmask = "255.255.255.0";
constexpr const char *kApPsk = "fancontrol123";
constexpr const char *kDeviceHostname = "fanctl";

struct FanState {
	bool enabled;
	bool use_adc_target;
	uint8_t percent;
	uint8_t effective_percent;
	uint8_t pwm_percent;
	uint8_t adc_target_percent;
	uint8_t actual_percent;
	int32_t adc_raw;
	int32_t adc_mv;
	int32_t mapped_voltage_mv;
	int32_t actual_rpm;
	int32_t target_rpm;
	uint32_t pwm_pulse_ns;
	bool tach_valid;
};

struct WifiSnapshot {
	bool ap_enabled;
	int ap_clients;
	bool sta_connected;
	char ap_ssid[WIFI_SSID_MAX_LEN + 1];
	char saved_ssid[WIFI_SSID_MAX_LEN + 1];
	char sta_state[24];
	int sta_rssi;
};

struct HostControlSnapshot {
	bool alive_check_enabled;
	bool timed_out;
	uint32_t timeout_ms;
	uint32_t last_alive_ago_ms;
};

} // namespace fanctl

#endif
