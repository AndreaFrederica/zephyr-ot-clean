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
#include "memory_domains.hpp"
#include "http_common.hpp"
#include "curve_profiles.hpp"
#include "fan_controller.hpp"
#include "host_control_manager.hpp"
#include "kilo_editor.hpp"
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
	size_t stack_bytes;
	size_t stack_used_bytes;
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

#if defined(CONFIG_THREAD_STACK_INFO) && defined(CONFIG_INIT_STACKS)
	size_t unused = 0U;
	if (k_thread_stack_space_get(thread, &unused) == 0) {
		size_t stack_size = thread->stack_info.size;
		summary->stack_bytes += stack_size;
		summary->stack_used_bytes += stack_size >= unused ? (stack_size - unused) : stack_size;
	}
#endif
}

inline void EmitHeapStats(EmitLineFn emit, void *ctx, const char *label,
			  const memory::HeapSnapshot &heap)
{
	if (!heap.available) {
		EmitLinef(emit, ctx, "  %-18s : unavailable", label);
		return;
	}

	char capacity_text[24];
	char free_text[24];
	char used_text[24];
	char peak_text[24];
	FormatBytes(heap.capacity_bytes, capacity_text, sizeof(capacity_text));
	FormatBytes(heap.free_bytes, free_text, sizeof(free_text));
	FormatBytes(heap.allocated_bytes, used_text, sizeof(used_text));
	FormatBytes(heap.peak_allocated_bytes, peak_text, sizeof(peak_text));

	EmitLinef(emit, ctx, "  %s capacity : %s", label, capacity_text);
	EmitLinef(emit, ctx, "  %s free     : %s", label, free_text);
	EmitLinef(emit, ctx, "  %s alloc    : %s", label, used_text);
	EmitLinef(emit, ctx, "  %s peak     : %s", label, peak_text);
}

struct ThreadEmitContext {
	EmitLineFn emit;
	void *ctx;
	uint64_t total_cycles;
};

inline unsigned int RuntimePercent(uint64_t part, uint64_t total)
{
	if (total == 0U) {
		return 0U;
	}

	return static_cast<unsigned int>((part * 100U) / total);
}

inline void EmitThreadDetailCallback(const struct k_thread *thread, void *user_data)
{
	auto *detail_ctx = static_cast<ThreadEmitContext *>(user_data);
	if (detail_ctx == nullptr || detail_ctx->emit == nullptr) {
		return;
	}

	const char *name = k_thread_name_get(const_cast<k_thread *>(thread));
	if (name == nullptr || name[0] == '\0') {
		name = "<unnamed>";
	}

	char state_text[32];
	const char *state = k_thread_state_str(const_cast<k_thread *>(thread), state_text,
					       sizeof(state_text));
	int priority = k_thread_priority_get(const_cast<k_thread *>(thread));
	unsigned int cpu_percent = 0U;

#if defined(CONFIG_THREAD_RUNTIME_STATS)
	k_thread_runtime_stats_t thread_stats = {};
	if (k_thread_runtime_stats_get(const_cast<k_thread *>(thread), &thread_stats) == 0) {
		cpu_percent = RuntimePercent(thread_stats.execution_cycles, detail_ctx->total_cycles);
	}
#endif

#if defined(CONFIG_THREAD_STACK_INFO) && defined(CONFIG_INIT_STACKS)
	size_t stack_size = thread->stack_info.size;
	size_t unused = 0U;
	if (k_thread_stack_space_get(thread, &unused) != 0) {
		unused = 0U;
	}
	size_t used = stack_size >= unused ? (stack_size - unused) : stack_size;
	unsigned int used_percent = stack_size > 0U
					    ? static_cast<unsigned int>((used * 100U) / stack_size)
					    : 0U;
	EmitLinef(detail_ctx->emit, detail_ctx->ctx,
		  "  %-18s p=%3d cpu=%3u%% %-11s stack=%5u/%5u B (%3u%%)",
		  name, priority, cpu_percent, state != nullptr ? state : "-",
		  static_cast<unsigned int>(used),
		  static_cast<unsigned int>(stack_size), used_percent);
#else
	EmitLinef(detail_ctx->emit, detail_ctx->ctx, "  %-18s p=%3d cpu=%3u%% %s",
		  name, priority, cpu_percent, state != nullptr ? state : "-");
#endif
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
#if defined(CONFIG_THREAD_RUNTIME_STATS)
	k_thread_runtime_stats_t runtime_all = {};
	bool runtime_all_ok = (k_thread_runtime_stats_all_get(&runtime_all) == 0);
#endif
#if defined(CONFIG_THREAD_STACK_INFO) && defined(CONFIG_INIT_STACKS)
	char stack_total_text[24];
	char stack_used_text[24];
	detail::FormatBytes(threads.stack_bytes, stack_total_text, sizeof(stack_total_text));
	detail::FormatBytes(threads.stack_used_bytes, stack_used_text, sizeof(stack_used_text));
	detail::EmitLinef(emit, ctx, "  stack usage        : %s used / %s reserved",
			  stack_used_text, stack_total_text);
#endif

#if defined(CONFIG_THREAD_RUNTIME_STATS)
	if (runtime_all_ok) {
		unsigned int busy_percent =
			detail::RuntimePercent(runtime_all.total_cycles, runtime_all.execution_cycles);
		unsigned int idle_percent =
			detail::RuntimePercent(runtime_all.idle_cycles, runtime_all.execution_cycles);
		detail::EmitLinef(emit, ctx, "  cpu usage          : busy %u%%  idle %u%%",
				  busy_percent, idle_percent);
	}
#endif

	detail::EmitLinef(emit, ctx, "Memory");
#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
	struct sys_memory_stats heap_stats = {};
	if (malloc_runtime_stats_get(&heap_stats) == 0) {
		memory::HeapSnapshot libc_heap = {};
		libc_heap.available = true;
		libc_heap.capacity_bytes = heap_stats.free_bytes + heap_stats.allocated_bytes;
		libc_heap.free_bytes = heap_stats.free_bytes;
		libc_heap.allocated_bytes = heap_stats.allocated_bytes;
		libc_heap.peak_allocated_bytes = heap_stats.max_allocated_bytes;
		detail::EmitHeapStats(emit, ctx, "libc heap", libc_heap);
	} else {
		detail::EmitLinef(emit, ctx, "  libc heap          : unavailable");
	}
#else
	detail::EmitLinef(emit, ctx, "  libc heap          : disabled");
#endif

	memory::HeapSnapshot http_heap = {};
	if (memory::GetHttpHeapSnapshot(&http_heap)) {
		detail::EmitHeapStats(emit, ctx, "http psram", http_heap);
	} else {
		detail::EmitLinef(emit, ctx, "  http psram         : unavailable");
	}

	memory::HeapSnapshot kilo_heap = {};
	if (kilo::GetHeapSnapshot(&kilo_heap)) {
		detail::EmitHeapStats(emit, ctx, "kilo psram", kilo_heap);
	} else {
		detail::EmitLinef(emit, ctx, "  kilo psram         : unavailable");
	}

	char recv_text[24];
	char scratch_text[24];
	char slot_total_text[24];
	char pool_total_text[24];
	detail::FormatBytes(http::kRecvBufferSize, recv_text, sizeof(recv_text));
	detail::FormatBytes(http::kLargeBufferSize, scratch_text, sizeof(scratch_text));
	detail::FormatBytes(http::kRecvBufferSize + http::kLargeBufferSize,
			    slot_total_text, sizeof(slot_total_text));
	detail::FormatBytes((http::kRecvBufferSize + http::kLargeBufferSize) * http::kWorkerCount,
			    pool_total_text, sizeof(pool_total_text));
	detail::EmitLinef(emit, ctx, "  http workers       : %u", static_cast<unsigned int>(http::kWorkerCount));
	detail::EmitLinef(emit, ctx, "  http recv buffer   : %s per worker", recv_text);
	detail::EmitLinef(emit, ctx, "  http scratch       : %s per worker", scratch_text);
	detail::EmitLinef(emit, ctx, "  http worker total  : %s", slot_total_text);
	detail::EmitLinef(emit, ctx, "  http pool total    : %s", pool_total_text);

	detail::EmitLinef(emit, ctx, "Threads");
	detail::ThreadEmitContext thread_ctx = { emit, ctx, 0U };
#if defined(CONFIG_THREAD_RUNTIME_STATS)
	if (runtime_all_ok) {
		thread_ctx.total_cycles = runtime_all.execution_cycles;
	}
#endif
	k_thread_foreach(detail::EmitThreadDetailCallback, &thread_ctx);
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
	detail::EmitLinef(emit, ctx, "  STA IP             : %s", wifi.sta_ip[0] != '\0' ? wifi.sta_ip : "-");
	detail::EmitLinef(emit, ctx, "  Saved SSID         : %s", wifi.saved_ssid[0] != '\0' ? wifi.saved_ssid : "-");
	detail::EmitLinef(emit, ctx, "  RSSI               : %d dBm", wifi.sta_rssi);
	detail::EmitLinef(emit, ctx, "  HTTP               : http://%s/  %s",
			  kApIpAddr,
			  wifi.sta_ip[0] != '\0' ? wifi.sta_ip : "STA pending");

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

inline void WriteMonitor(EmitLineFn emit, void *ctx, FanController &fan_controller,
			 WifiManager &wifi_manager, HostControlManager &host_control)
{
	ARG_UNUSED(host_control);

	WifiSnapshot wifi = {};
	wifi_manager.GetSnapshot(&wifi);

	FanState fans[kFanCount];
	fan_controller.GetAllStates(fans);

	char uptime_text[32];
	detail::FormatDuration(k_uptime_get(), uptime_text, sizeof(uptime_text));

	detail::EmitLinef(emit, ctx, "fanctl top  uptime=%s", uptime_text);
	detail::EmitLinef(emit, ctx, "wifi  ap=%s sta=%s ip=%s rssi=%d",
			  wifi.ap_enabled ? "on" : "off",
			  wifi.sta_connected ? "connected" : wifi.sta_state,
			  wifi.sta_ip[0] != '\0' ? wifi.sta_ip : "-",
			  wifi.sta_rssi);

#if defined(CONFIG_THREAD_RUNTIME_STATS)
	k_thread_runtime_stats_t runtime_all = {};
	if (k_thread_runtime_stats_all_get(&runtime_all) == 0) {
		unsigned int busy_percent =
			detail::RuntimePercent(runtime_all.total_cycles, runtime_all.execution_cycles);
		unsigned int idle_percent =
			detail::RuntimePercent(runtime_all.idle_cycles, runtime_all.execution_cycles);
		detail::EmitLinef(emit, ctx, "cpu   busy=%u%% idle=%u%%", busy_percent, idle_percent);
	}
#endif

#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
	struct sys_memory_stats heap_stats = {};
	if (malloc_runtime_stats_get(&heap_stats) == 0) {
		char free_text[24];
		char used_text[24];
		detail::FormatBytes(heap_stats.free_bytes, free_text, sizeof(free_text));
		detail::FormatBytes(heap_stats.allocated_bytes, used_text, sizeof(used_text));
		detail::EmitLinef(emit, ctx, "heap  libc used=%s free=%s", used_text, free_text);
	}
#endif

	memory::HeapSnapshot http_heap = {};
	if (memory::GetHttpHeapSnapshot(&http_heap) && http_heap.available) {
		char free_text[24];
		char used_text[24];
		detail::FormatBytes(http_heap.free_bytes, free_text, sizeof(free_text));
		detail::FormatBytes(http_heap.allocated_bytes, used_text, sizeof(used_text));
		detail::EmitLinef(emit, ctx, "heap  http used=%s free=%s", used_text, free_text);
	}

	memory::HeapSnapshot kilo_heap = {};
	if (kilo::GetHeapSnapshot(&kilo_heap) && kilo_heap.available) {
		char free_text[24];
		char used_text[24];
		detail::FormatBytes(kilo_heap.free_bytes, free_text, sizeof(free_text));
		detail::FormatBytes(kilo_heap.allocated_bytes, used_text, sizeof(used_text));
		detail::EmitLinef(emit, ctx, "heap  kilo used=%s free=%s", used_text, free_text);
	}

	for (size_t i = 0U; i < kFanCount; ++i) {
		detail::EmitLinef(emit, ctx,
				  "fan%u  enabled=%s effective=%u%% pwm=%u%% rpm=%d adc=%d mV",
				  static_cast<unsigned int>(i + 1U),
				  fans[i].enabled ? "true" : "false",
				  fans[i].effective_percent,
				  fans[i].pwm_percent,
				  fans[i].actual_rpm,
				  fans[i].mapped_voltage_mv);
	}

	detail::EmitLinef(emit, ctx, "threads");
	detail::ThreadEmitContext thread_ctx = { emit, ctx, 0U };
#if defined(CONFIG_THREAD_RUNTIME_STATS)
	if (k_thread_runtime_stats_all_get(&runtime_all) == 0) {
		thread_ctx.total_cycles = runtime_all.execution_cycles;
	}
#endif
	k_thread_foreach(detail::EmitThreadDetailCallback, &thread_ctx);
}

} // namespace fanctl::show_status

#endif
