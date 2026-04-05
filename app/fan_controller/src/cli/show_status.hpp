/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_SHOW_STATUS_HPP_
#define FAN_CONTROLLER_SHOW_STATUS_HPP_

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

#include <sys_malloc.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/mem_stats.h>

#include "core/common.hpp"
#include "curve_profiles.hpp"
#include "fan_controller.hpp"
#include "host_control_manager.hpp"
#include "settings_store.hpp"
#include "storage.hpp"
#include "wifi_manager.hpp"

namespace fanctl::show_status {

using EmitLineFn = void (*)(void *ctx, const char *line);

namespace detail {

inline void EmitLinef(EmitLineFn emit, void *ctx, const char *fmt, ...)
{
	if (emit == nullptr || fmt == nullptr) {
		return;
	}

	char buffer[256];
	va_list args;
	va_start(args, fmt);
	(void)vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	emit(ctx, buffer);
}

inline void FormatBytes(uint64_t bytes, char *buffer, size_t buffer_len)
{
	static const char *kUnits[] = { "B", "KB", "MB", "GB" };
	double value = static_cast<double>(bytes);
	size_t unit_index = 0U;

	while (value >= 1024.0 && unit_index + 1U < (sizeof(kUnits) / sizeof(kUnits[0]))) {
		value /= 1024.0;
		++unit_index;
	}

	if (unit_index == 0U) {
		(void)snprintf(buffer, buffer_len, "%llu %s",
			       static_cast<unsigned long long>(bytes), kUnits[unit_index]);
		return;
	}

	(void)snprintf(buffer, buffer_len, "%.2f %s", value, kUnits[unit_index]);
}

inline void FormatDuration(int64_t uptime_ms, char *buffer, size_t buffer_len)
{
	uint64_t total_seconds = uptime_ms > 0 ? static_cast<uint64_t>(uptime_ms / 1000) : 0U;
	uint64_t days = total_seconds / 86400U;
	uint64_t hours = (total_seconds % 86400U) / 3600U;
	uint64_t minutes = (total_seconds % 3600U) / 60U;
	uint64_t seconds = total_seconds % 60U;

	(void)snprintf(buffer, buffer_len, "%llu d %02llu:%02llu:%02llu",
		       static_cast<unsigned long long>(days),
		       static_cast<unsigned long long>(hours),
		       static_cast<unsigned long long>(minutes),
		       static_cast<unsigned long long>(seconds));
}

struct ThreadSummary {
	size_t total;
	size_t named;
};

inline void CountThreadCallback(const struct k_thread *thread, void *user_data)
{
	auto *summary = static_cast<ThreadSummary *>(user_data);
	if (summary == nullptr) {
		return;
	}

	++summary->total;
	const char *name = k_thread_name_get(const_cast<k_thread *>(thread));
	if (name != nullptr && name[0] != '\0') {
		++summary->named;
	}
}

inline void EmitFan(EmitLineFn emit, void *ctx, size_t index, const FanState &fan)
{
	EmitLinef(emit, ctx, "Fan %u", static_cast<unsigned int>(index + 1U));
	EmitLinef(emit, ctx, "  enabled            : %s", fan.enabled ? "true" : "false");
	EmitLinef(emit, ctx, "  adc mode           : %s", fan.use_adc_target ? "true" : "false");
	EmitLinef(emit, ctx, "  manual percent     : %u%%", fan.percent);
	EmitLinef(emit, ctx, "  effective percent  : %u%%", fan.effective_percent);
	EmitLinef(emit, ctx, "  pwm output percent : %u%%", fan.pwm_percent);
	EmitLinef(emit, ctx, "  pwm pulse          : %u ns", static_cast<unsigned int>(fan.pwm_pulse_ns));
	EmitLinef(emit, ctx, "  adc raw            : %d", fan.adc_raw);
	EmitLinef(emit, ctx, "  adc sample         : %d mV", fan.adc_mv);
	EmitLinef(emit, ctx, "  mapped voltage     : %d mV", fan.mapped_voltage_mv);
	EmitLinef(emit, ctx, "  adc target percent : %u%%", fan.adc_target_percent);
	EmitLinef(emit, ctx, "  target rpm curve   : %d rpm", fan.target_rpm);
	EmitLinef(emit, ctx, "  actual rpm         : %d rpm", fan.actual_rpm);
	EmitLinef(emit, ctx, "  actual percent est : %u%%", fan.actual_percent);
	EmitLinef(emit, ctx, "  tach valid         : %s", fan.tach_valid ? "true" : "false");
}

} // namespace detail

inline void WriteSystem(EmitLineFn emit, void *ctx)
{
	detail::EmitLinef(emit, ctx, "System");
	detail::EmitLinef(emit, ctx, "  hostname           : %s", kDeviceHostname);
	detail::EmitLinef(emit, ctx, "  build time         : %s %s", __DATE__, __TIME__);

	char uptime_text[32];
	detail::FormatDuration(k_uptime_get(), uptime_text, sizeof(uptime_text));
	detail::EmitLinef(emit, ctx, "  uptime             : %s", uptime_text);

	struct timespec ts = {};
	if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
		time_t now = static_cast<time_t>(ts.tv_sec);
		struct tm utc = {};
		if (gmtime_r(&now, &utc) != nullptr) {
			char time_text[48];
			(void)strftime(time_text, sizeof(time_text), "%Y-%m-%d %H:%M:%S UTC", &utc);
			detail::EmitLinef(emit, ctx, "  realtime           : %s", time_text);
		} else {
			detail::EmitLinef(emit, ctx, "  realtime           : <unavailable>");
		}
		detail::EmitLinef(emit, ctx, "  unix time          : %lld", static_cast<long long>(ts.tv_sec));
	} else {
		detail::EmitLinef(emit, ctx, "  realtime           : <not set>");
	}

	detail::ThreadSummary threads = {};
	k_thread_foreach(detail::CountThreadCallback, &threads);
	detail::EmitLinef(emit, ctx, "  threads            : %u total, %u named",
			  static_cast<unsigned int>(threads.total),
			  static_cast<unsigned int>(threads.named));

#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
	struct sys_memory_stats heap_stats = {};
	if (malloc_runtime_stats_get(&heap_stats) == 0) {
		char free_text[24];
		char used_text[24];
		char peak_text[24];
		detail::FormatBytes(heap_stats.free_bytes, free_text, sizeof(free_text));
		detail::FormatBytes(heap_stats.allocated_bytes, used_text, sizeof(used_text));
		detail::FormatBytes(heap_stats.max_allocated_bytes, peak_text, sizeof(peak_text));
			detail::EmitLinef(emit, ctx, "  heap free          : %s", free_text);
			detail::EmitLinef(emit, ctx, "  heap allocated     : %s", used_text);
			detail::EmitLinef(emit, ctx, "  heap peak          : %s", peak_text);
	} else {
		detail::EmitLinef(emit, ctx, "  heap stats         : unavailable");
	}
#else
	detail::EmitLinef(emit, ctx, "  heap stats         : disabled");
#endif
}

inline void WriteWifi(EmitLineFn emit, void *ctx, WifiManager &wifi_manager)
{
	WifiSnapshot wifi = {};
	wifi_manager.GetSnapshot(&wifi);

	settings::WifiConfig config = {};
	int rc = settings::LoadWifiConfig(&config);

	detail::EmitLinef(emit, ctx, "Wi-Fi Runtime");
	detail::EmitLinef(emit, ctx, "  AP enabled         : %s", wifi.ap_enabled ? "true" : "false");
	detail::EmitLinef(emit, ctx, "  AP SSID            : %s", wifi.ap_ssid[0] != '\0' ? wifi.ap_ssid : "-");
	detail::EmitLinef(emit, ctx, "  AP IP              : %s", kApIpAddr);
	detail::EmitLinef(emit, ctx, "  AP clients         : %d", wifi.ap_clients);
	detail::EmitLinef(emit, ctx, "  STA connected      : %s", wifi.sta_connected ? "true" : "false");
	detail::EmitLinef(emit, ctx, "  STA state          : %s", wifi.sta_state[0] != '\0' ? wifi.sta_state : "-");
	detail::EmitLinef(emit, ctx, "  Saved SSID         : %s", wifi.saved_ssid[0] != '\0' ? wifi.saved_ssid : "-");
	detail::EmitLinef(emit, ctx, "  RSSI               : %d dBm", wifi.sta_rssi);

	detail::EmitLinef(emit, ctx, "Wi-Fi Config");
	if (rc != 0) {
		detail::EmitLinef(emit, ctx, "  load status        : failed (%d)", rc);
		return;
	}

	detail::EmitLinef(emit, ctx, "  AP default enabled : %s", config.ap_enabled ? "true" : "false");
	detail::EmitLinef(emit, ctx, "  AP channel         : %d", config.ap_channel);
	detail::EmitLinef(emit, ctx, "  AP hidden          : %s", config.ap_hidden ? "true" : "false");
	detail::EmitLinef(emit, ctx, "  AP max clients     : %d", config.ap_max_clients);
	detail::EmitLinef(emit, ctx, "  DHCP hostname      : %s",
			  config.dhcp_hostname[0] != '\0' ? config.dhcp_hostname : "-");
	detail::EmitLinef(emit, ctx, "  STA auto reconnect : %s",
			  config.sta_auto_reconnect ? "true" : "false");
	detail::EmitLinef(emit, ctx, "  STA scan threshold : %d", config.sta_scan_threshold);
}

inline void WriteStorage(EmitLineFn emit, void *ctx)
{
	detail::EmitLinef(emit, ctx, "Storage");
	detail::EmitLinef(emit, ctx, "  mount point        : %s", storage::GetMountPoint());
	detail::EmitLinef(emit, ctx, "  config path        : %s", settings::GetConfigRelativePath());
	detail::EmitLinef(emit, ctx, "  wifi path          : %s", settings::GetWifiConfigRelativePath());
	detail::EmitLinef(emit, ctx, "  ntp path           : %s", settings::GetNtpConfigRelativePath());
	detail::EmitLinef(emit, ctx, "  ssh path           : %s", settings::GetSshConfigRelativePath());

	struct fs_statvfs stat = {};
	int rc = fs_statvfs(storage::GetMountPoint(), &stat);
	if (rc != 0) {
		detail::EmitLinef(emit, ctx, "  fs stat            : failed (%d)", rc);
		return;
	}

	uint64_t block_size = stat.f_frsize != 0U ? stat.f_frsize : stat.f_bsize;
	uint64_t total_bytes = block_size * stat.f_blocks;
	uint64_t free_bytes = block_size * stat.f_bfree;
	uint64_t used_bytes = total_bytes >= free_bytes ? total_bytes - free_bytes : 0U;

	char total_text[24];
	char used_text[24];
	char free_text[24];
	detail::FormatBytes(total_bytes, total_text, sizeof(total_text));
	detail::FormatBytes(used_bytes, used_text, sizeof(used_text));
	detail::FormatBytes(free_bytes, free_text, sizeof(free_text));

	detail::EmitLinef(emit, ctx, "  block size         : %lu", static_cast<unsigned long>(block_size));
	detail::EmitLinef(emit, ctx, "  blocks total       : %lu", stat.f_blocks);
	detail::EmitLinef(emit, ctx, "  blocks free        : %lu", stat.f_bfree);
	detail::EmitLinef(emit, ctx, "  total size         : %s", total_text);
	detail::EmitLinef(emit, ctx, "  used size          : %s", used_text);
	detail::EmitLinef(emit, ctx, "  free size          : %s", free_text);
}

inline void WriteHost(EmitLineFn emit, void *ctx, HostControlManager &host_control)
{
	HostControlSnapshot snapshot = {};
	host_control.GetSnapshot(&snapshot);

	detail::EmitLinef(emit, ctx, "Host Control");
	detail::EmitLinef(emit, ctx, "  alive check        : %s",
			  snapshot.alive_check_enabled ? "true" : "false");
	detail::EmitLinef(emit, ctx, "  timed out          : %s", snapshot.timed_out ? "true" : "false");
	detail::EmitLinef(emit, ctx, "  timeout            : %u ms",
			  static_cast<unsigned int>(snapshot.timeout_ms));
	detail::EmitLinef(emit, ctx, "  last alive ago     : %u ms",
			  static_cast<unsigned int>(snapshot.last_alive_ago_ms));
}

inline void WriteNtp(EmitLineFn emit, void *ctx)
{
	settings::NtpConfig config = {};
	int rc = settings::LoadNtpConfig(&config);

	detail::EmitLinef(emit, ctx, "NTP");
	if (rc != 0) {
		detail::EmitLinef(emit, ctx, "  load status        : failed (%d)", rc);
		return;
	}

	detail::EmitLinef(emit, ctx, "  enabled            : %s", config.enabled ? "true" : "false");
	detail::EmitLinef(emit, ctx, "  use DHCP server    : %s",
			  config.use_dhcp_server ? "true" : "false");
	detail::EmitLinef(emit, ctx, "  port               : %d", config.port);
	detail::EmitLinef(emit, ctx, "  sync interval      : %d hours", config.sync_interval_hours);
	detail::EmitLinef(emit, ctx, "  server count       : %u",
			  static_cast<unsigned int>(config.server_count));
	for (size_t i = 0U; i < config.server_count && i < settings::kMaxNtpServers; ++i) {
		detail::EmitLinef(emit, ctx, "  server[%u]         : %s",
				  static_cast<unsigned int>(i),
				  config.servers[i][0] != '\0' ? config.servers[i] : "-");
	}
}

inline void WriteSsh(EmitLineFn emit, void *ctx)
{
	settings::SshConfig config = {};
	int rc = settings::LoadSshConfig(&config);

	detail::EmitLinef(emit, ctx, "SSH");
	if (rc != 0) {
		detail::EmitLinef(emit, ctx, "  load status        : failed (%d)", rc);
		return;
	}

	detail::EmitLinef(emit, ctx, "  enabled            : %s", config.enabled ? "true" : "false");
	detail::EmitLinef(emit, ctx, "  listen port        : %d", config.listen_port);
	detail::EmitLinef(emit, ctx, "  username           : %s",
			  config.username[0] != '\0' ? config.username : "-");
	detail::EmitLinef(emit, ctx, "  password auth      : %s",
			  config.allow_password_auth ? "true" : "false");
	detail::EmitLinef(emit, ctx, "  public key auth    : %s",
			  config.allow_public_key_auth ? "true" : "false");
	detail::EmitLinef(emit, ctx, "  host key path      : %s",
			  config.host_key_path[0] != '\0' ? config.host_key_path : "-");
	detail::EmitLinef(emit, ctx, "  authorized keys    : %s",
			  config.authorized_keys_path[0] != '\0' ? config.authorized_keys_path : "-");
}

inline int WriteFans(EmitLineFn emit, void *ctx, FanController &fan_controller, int fan_id = 0)
{
	FanState fans[kFanCount];
	fan_controller.GetAllStates(fans);

	if (fan_id > 0) {
		if (fan_id < 1 || fan_id > static_cast<int>(kFanCount)) {
			return -EINVAL;
		}
		detail::EmitFan(emit, ctx, static_cast<size_t>(fan_id - 1), fans[fan_id - 1]);
		return 0;
	}

	for (size_t i = 0U; i < kFanCount; ++i) {
		detail::EmitFan(emit, ctx, i, fans[i]);
	}
	return 0;
}

inline void WriteCurves(EmitLineFn emit, void *ctx)
{
	detail::EmitLinef(emit, ctx, "Curves");
	detail::EmitLinef(emit, ctx, "  adc raw -> voltage   : %s",
			  curves::CurveProfiles::GetAdcToVoltagePath());
	detail::EmitLinef(emit, ctx, "  voltage -> percent   : %s",
			  curves::CurveProfiles::GetVoltageToPercentPath());
	detail::EmitLinef(emit, ctx, "  speed percent -> pwm : %s",
			  curves::CurveProfiles::GetPercentToPwmPath());
	detail::EmitLinef(emit, ctx, "  speed percent -> rpm : %s",
			  curves::CurveProfiles::GetPercentToRpmPath());
}

inline void WriteAll(EmitLineFn emit, void *ctx, FanController &fan_controller,
		     WifiManager &wifi_manager, HostControlManager &host_control)
{
	WriteSystem(emit, ctx);
	emit(ctx, "");
	WriteWifi(emit, ctx, wifi_manager);
	emit(ctx, "");
	WriteHost(emit, ctx, host_control);
	emit(ctx, "");
	WriteStorage(emit, ctx);
	emit(ctx, "");
	WriteNtp(emit, ctx);
	emit(ctx, "");
	WriteSsh(emit, ctx);
	emit(ctx, "");
	(void)WriteFans(emit, ctx, fan_controller, 0);
	emit(ctx, "");
	WriteCurves(emit, ctx);
}

} // namespace fanctl::show_status

#endif
