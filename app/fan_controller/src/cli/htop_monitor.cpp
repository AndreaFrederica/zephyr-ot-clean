/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "htop_monitor.hpp"

#include <string.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

// 外部声明 malloc stats API
extern "C" int malloc_runtime_stats_get(struct sys_memory_stats *stats);

#include "core/common.hpp"
#include "core/memory_domains.hpp"
#include "services/fan/fan_controller.hpp"
#include "services/network/wifi_manager.hpp"
#include "services/host_control/host_control_manager.hpp"

// ANSI 终端控制序列
#define ESC "\x1b"
#define CSI ESC "["

#define CLEAR_SCREEN        CSI "2J"
#define CLEAR_LINE          CSI "K"
#define CURSOR_HIDE         CSI "?25l"
#define CURSOR_SHOW         CSI "?25h"
#define CURSOR_HOME         CSI "H"
#define CURSOR_MOVE(r,c)    CSI r ";" c "H"

#define COLOR_RESET         CSI "0m"
#define COLOR_BOLD          CSI "1m"
#define COLOR_RED           CSI "31m"
#define COLOR_GREEN         CSI "32m"
#define COLOR_YELLOW        CSI "33m"
#define COLOR_BLUE          CSI "34m"
#define COLOR_MAGENTA       CSI "35m"
#define COLOR_CYAN          CSI "36m"
#define COLOR_WHITE         CSI "37m"
#define COLOR_BG_BLUE       CSI "44m"
#define COLOR_BG_GRAY       CSI "100m"

// 外部全局对象（由 main.cpp 定义）
namespace fanctl {
	extern FanController g_fan_controller;
	extern WifiManager g_wifi_manager;
	extern HostControlManager g_host_control;
}

namespace fanctl::htop {

namespace {

struct HtopContext {
	const Io *io;
	int rows;
	int cols;
	bool running;
	uint32_t refresh_interval_ms;
	uint32_t frame_count;
};

void WriteRaw(HtopContext *ctx, const char *data)
{
	if (ctx->io && ctx->io->write) {
		ctx->io->write(ctx->io->ctx, data, strlen(data));
	}
}

void WriteFmt(HtopContext *ctx, const char *fmt, ...)
{
	char buf[256];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	WriteRaw(ctx, buf);
}

void ClearScreen(HtopContext *ctx)
{
	WriteRaw(ctx, CLEAR_SCREEN CURSOR_HOME);
}

void MoveCursor(HtopContext *ctx, int row, int col)
{
	WriteFmt(ctx, CSI "%d;%dH", row, col);
}

void DrawHeader(HtopContext *ctx)
{
	// 顶部标题栏
	WriteRaw(ctx, COLOR_BG_BLUE COLOR_BOLD COLOR_WHITE);
	MoveCursor(ctx, 1, 1);
	
	char title[128];
	snprintf(title, sizeof(title), " fanctl htop - %dx%d | 'q' quit | 'r' refresh | +/- speed ", 
		 ctx->cols, ctx->rows);
	
	// 填充整行
	int title_len = strlen(title);
	for (int i = 0; i < ctx->cols; i++) {
		if (i < title_len) {
			WriteFmt(ctx, "%c", title[i]);
		} else {
			WriteRaw(ctx, " ");
		}
	}
	WriteRaw(ctx, COLOR_RESET);
}

void DrawBar(HtopContext *ctx, int row, int col, int width, 
	     const char *label, uint32_t value, uint32_t max, 
	     const char *color_used, const char *color_free)
{
	MoveCursor(ctx, row, col);
	
	int bar_width = width - 20;  // 留给标签和数值的空间
	if (bar_width < 5) bar_width = 5;
	
	uint32_t percent = (max > 0) ? (value * 100 / max) : 0;
	int filled = (percent * bar_width) / 100;
	if (filled > bar_width) filled = bar_width;
	
	WriteFmt(ctx, "%s", label);
	WriteFmt(ctx, "[%s", color_used);
	for (int i = 0; i < filled; i++) WriteRaw(ctx, "|");
	WriteFmt(ctx, "%s", color_free);
	for (int i = filled; i < bar_width; i++) WriteRaw(ctx, " ");
	WriteFmt(ctx, "%s] %3u%%", COLOR_RESET, percent);
}

void FormatBytes(char *buf, size_t len, size_t bytes)
{
	if (bytes >= 1024 * 1024) {
		snprintf(buf, len, "%.2fM", bytes / (1024.0 * 1024.0));
	} else if (bytes >= 1024) {
		snprintf(buf, len, "%.1fK", bytes / 1024.0);
	} else {
		snprintf(buf, len, "%zuB", bytes);
	}
}

void DrawMemorySection(HtopContext *ctx, int start_row)
{
	MoveCursor(ctx, start_row, 1);
	WriteFmt(ctx, COLOR_BOLD "Memory Usage" COLOR_RESET);
	
	// DRAM - 使用 libc heap stats
	struct sys_memory_stats heap_stats = {};
	if (malloc_runtime_stats_get(&heap_stats) == 0) {
		size_t total = heap_stats.free_bytes + heap_stats.allocated_bytes;
		DrawBar(ctx, start_row + 1, 1, ctx->cols / 2 - 2,
			"DRAM ", heap_stats.allocated_bytes, total,
			COLOR_GREEN, COLOR_YELLOW);
		MoveCursor(ctx, start_row + 1, ctx->cols / 2 + 2);
		WriteFmt(ctx, "free:%zu used:%zu", heap_stats.free_bytes, heap_stats.allocated_bytes);
	}
	
	// PSRAM
	memory::HeapSnapshot psram = {};
	if (memory::GetPsramHeapSnapshot(&psram) && psram.available) {
		DrawBar(ctx, start_row + 2, 1, ctx->cols / 2 - 2,
			"PSRAM", psram.allocated_bytes, psram.capacity_bytes,
			COLOR_CYAN, COLOR_YELLOW);
		MoveCursor(ctx, start_row + 2, ctx->cols / 2 + 2);
		WriteFmt(ctx, "total:%.1fM free:%.1fM", 
			psram.capacity_bytes / (1024.0*1024.0),
			psram.free_bytes / (1024.0*1024.0));
	}
	
	// HTTP pool
	memory::HeapSnapshot http = {};
	if (memory::GetHttpHeapSnapshot(&http) && http.available) {
		DrawBar(ctx, start_row + 3, 1, ctx->cols / 2 - 2,
			"HTTP ", http.allocated_bytes, http.capacity_bytes,
			COLOR_MAGENTA, COLOR_YELLOW);
	}
}

void DrawCpuSection(HtopContext *ctx, int start_row)
{
#if defined(CONFIG_THREAD_RUNTIME_STATS)
	k_thread_runtime_stats_t runtime_all = {};
	if (k_thread_runtime_stats_all_get(&runtime_all) != 0) {
		return;
	}
	
	MoveCursor(ctx, start_row, ctx->cols / 2 + 1);
	WriteFmt(ctx, COLOR_BOLD "CPU Usage" COLOR_RESET);
	
	uint32_t busy_percent = (runtime_all.execution_cycles > 0) 
		? ((runtime_all.execution_cycles - runtime_all.idle_cycles) * 100 
		   / runtime_all.execution_cycles) : 0;
	
	DrawBar(ctx, start_row + 1, ctx->cols / 2 + 1, ctx->cols / 2 - 1,
		"Busy ", busy_percent, 100, COLOR_RED, COLOR_YELLOW);
	
	MoveCursor(ctx, start_row + 2, ctx->cols / 2 + 1);
	WriteFmt(ctx, "Cycles: exec=%llu idle=%llu", 
		runtime_all.execution_cycles, runtime_all.idle_cycles);
#endif
}

void DrawFanSection(HtopContext *ctx, int start_row)
{
	MoveCursor(ctx, start_row, 1);
	WriteFmt(ctx, COLOR_BOLD "Fan Status" COLOR_RESET);
	
	FanState fans[kFanCount];
	fanctl::g_fan_controller.GetAllStates(fans);
	
	for (size_t i = 0; i < kFanCount; i++) {
		MoveCursor(ctx, start_row + 1 + i, 1);
		const char *color = fans[i].enabled ? COLOR_GREEN : COLOR_RED;
		WriteFmt(ctx, "Fan%u: %s%s%s  eff=%3u%% pwm=%3u%% rpm=%5d adc=%4dmV",
			(unsigned)(i + 1),
			color, fans[i].enabled ? "ON " : "OFF", COLOR_RESET,
			fans[i].effective_percent,
			fans[i].pwm_percent,
			fans[i].actual_rpm,
			fans[i].mapped_voltage_mv);
	}
}

void DrawWifiSection(HtopContext *ctx, int start_row)
{
	MoveCursor(ctx, start_row, 1);
	WriteFmt(ctx, COLOR_BOLD "WiFi Status" COLOR_RESET);
	
	WifiSnapshot wifi = {};
	fanctl::g_wifi_manager.GetSnapshot(&wifi);
	
	MoveCursor(ctx, start_row + 1, 1);
	const char *sta_color = wifi.sta_connected ? COLOR_GREEN : COLOR_RED;
	WriteFmt(ctx, "STA: %s%s%s (%s) IP=%s RSSI=%d",
		sta_color,
		wifi.sta_connected ? "CONNECTED" : wifi.sta_state,
		COLOR_RESET,
		wifi.saved_ssid[0] ? wifi.saved_ssid : "--",
		wifi.sta_ip[0] ? wifi.sta_ip : "--",
		wifi.sta_rssi);
	
	MoveCursor(ctx, start_row + 2, 1);
	const char *ap_color = wifi.ap_enabled ? COLOR_GREEN : COLOR_RED;
	WriteFmt(ctx, "AP : %s%s%s (%s) clients=%d",
		ap_color,
		wifi.ap_enabled ? "ON " : "OFF",
		COLOR_RESET,
		wifi.ap_ssid[0] ? wifi.ap_ssid : "--",
		wifi.ap_clients);
}

void DrawThreadsSection(HtopContext *ctx, int start_row, int max_rows)
{
	MoveCursor(ctx, start_row, 1);
	WriteFmt(ctx, COLOR_BOLD "%-16s %4s %6s %12s %s" COLOR_RESET,
		 "Thread", "Prio", "CPU%", "Stack", "State");
	
#if defined(CONFIG_THREAD_RUNTIME_STATS)
	k_thread_runtime_stats_t runtime_all = {};
	bool has_runtime_stats = (k_thread_runtime_stats_all_get(&runtime_all) == 0);
#else
	bool has_runtime_stats = false;
#endif
	
	// 使用 Zephyr 的线程遍历 API
	struct ThreadInfo {
		HtopContext *ctx;
		int row;
		int max_row;
		int count;
		bool has_runtime;
#if defined(CONFIG_THREAD_RUNTIME_STATS)
		k_thread_runtime_stats_t *total_stats;
#endif
	} info = { ctx, start_row + 1, start_row + max_rows, 0, has_runtime_stats
#if defined(CONFIG_THREAD_RUNTIME_STATS)
		, &runtime_all
#endif
	};
	
	k_thread_foreach([](const struct k_thread *thread, void *user_data) {
		ThreadInfo *info = static_cast<ThreadInfo*>(user_data);
		if (info->row >= info->max_row) return;
		
		HtopContext *ctx = info->ctx;
		MoveCursor(ctx, info->row, 1);
		
		const char *name = k_thread_name_get((struct k_thread*)thread);
		if (!name) name = "--";
		
		int prio = k_thread_priority_get((struct k_thread*)thread);
		
		// 线程状态
		const char *state_str = "unknown";
		uint8_t state = thread->base.thread_state;
		if (state == 0) state_str = (thread == k_current_get()) ? "running" : "queued";
		else if (state & 0x08) state_str = "sleeping";
		else if (state & 0x02) state_str = "pending";
		else if (state & 0x04) state_str = "suspended";
		else if (state & 0x01) state_str = "queued";
		
		// 栈使用
		size_t stack_size = thread->stack_info.size;
		size_t stack_used = thread->stack_info.size - thread->stack_info.delta;
		uint32_t stack_pct = (stack_size > 0) ? (uint32_t)(stack_used * 100 / stack_size) : 0;
		
		// CPU 使用率
		uint32_t cpu_pct = 0;
#if defined(CONFIG_THREAD_RUNTIME_STATS)
		if (info->has_runtime && info->total_stats && info->total_stats->execution_cycles > 0) {
			k_thread_runtime_stats_t thread_stats = {};
			if (k_thread_runtime_stats_get((struct k_thread*)thread, &thread_stats) == 0) {
				cpu_pct = (uint32_t)((thread_stats.execution_cycles * 100) / 
						   info->total_stats->execution_cycles);
			}
		}
#endif
		
		WriteFmt(ctx, "%-16s %4d %5u%% %4zu/%4zu(%2u%%) %s",
			name, prio, cpu_pct,
			stack_used / 1024, stack_size / 1024, stack_pct,
			state_str);
		
		info->row++;
		info->count++;
	}, &info);
}

void DrawFooter(HtopContext *ctx)
{
	MoveCursor(ctx, ctx->rows, 1);
	WriteRaw(ctx, COLOR_BG_GRAY COLOR_WHITE);
	
	char footer[128];
	snprintf(footer, sizeof(footer), 
		" Frame: %lu | Interval: %lums | Press 'q' to quit ",
		(unsigned long)ctx->frame_count,
		(unsigned long)ctx->refresh_interval_ms);
	
	int len = strlen(footer);
	for (int i = 0; i < ctx->cols; i++) {
		if (i < len) {
			WriteFmt(ctx, "%c", footer[i]);
		} else {
			WriteRaw(ctx, " ");
		}
	}
	WriteRaw(ctx, COLOR_RESET);
}

void DrawFrame(HtopContext *ctx)
{
	ClearScreen(ctx);
	DrawHeader(ctx);
	
	// 布局分区 (24行标准终端)
	int row = 3;
	
	// 上半部分：内存 (4行) 和 CPU (3行)
	DrawMemorySection(ctx, row);
	row += 5;
	DrawCpuSection(ctx, row);
	row += 4;
	
	// 中间：风扇 (3行) 和 WiFi (3行)
	DrawFanSection(ctx, row);
	row += 4;
	DrawWifiSection(ctx, row);
	row += 4;
	
	// 下半部分：线程列表 (剩余行数)
	int remaining_rows = ctx->rows - row - 2;  // -2 for footer
	if (remaining_rows > 3) {
		DrawThreadsSection(ctx, row, remaining_rows);
	}
	
	DrawFooter(ctx);
	ctx->frame_count++;
}

// 非阻塞读取按键
int ReadKeyNonBlocking(HtopContext *ctx)
{
	if (!ctx->io || !ctx->io->peek_char || !ctx->io->read_char) {
		return -1;
	}
	
	char ch;
	// 先 peek 看是否有数据
	if (ctx->io->peek_char(ctx->io->ctx, &ch) <= 0) {
		return -1;  // 没有数据
	}
	
	// 有数据，读取它
	return ctx->io->read_char(ctx->io->ctx);
}

void ProcessInput(HtopContext *ctx)
{
	// 处理所有待输入的按键
	while (true) {
		int key = ReadKeyNonBlocking(ctx);
		if (key < 0) break;
		
		switch (key) {
		case 'q':
		case 'Q':
			ctx->running = false;
			return;
		case 0x1B:  // ESC 键
			// 尝试读取后续字符（箭头键等转义序列）
			key = ReadKeyNonBlocking(ctx);
			if (key < 0) {
				// 单独的 ESC，也作为退出
				ctx->running = false;
				return;
			}
			// 丢弃转义序列其余部分
			while (ReadKeyNonBlocking(ctx) >= 0) {}
			break;
		case 'r':
		case 'R':
			// 立即刷新 - 什么都不做，主循环会重绘
			break;
		case '+':
			if (ctx->refresh_interval_ms > 100) {
				ctx->refresh_interval_ms -= 100;
			}
			break;
		case '-':
			if (ctx->refresh_interval_ms < 5000) {
				ctx->refresh_interval_ms += 100;
			}
			break;
		}
	}
}

} // namespace

int Run(const Io &io, int screen_rows, int screen_cols)
{
	if (!io.write || !io.read_char) {
		return -EINVAL;
	}
	
	HtopContext ctx = {};
	ctx.io = &io;
	ctx.rows = screen_rows;
	ctx.cols = screen_cols;
	ctx.running = true;
	ctx.refresh_interval_ms = 1000;  // 默认 1 秒刷新
	ctx.frame_count = 0;
	
	// 隐藏光标，进入全屏模式
	WriteRaw(&ctx, CURSOR_HIDE CLEAR_SCREEN);
	
	while (ctx.running) {
		DrawFrame(&ctx);
		
		// 等待输入或超时
		uint32_t elapsed = 0;
		while (elapsed < ctx.refresh_interval_ms && ctx.running) {
			ProcessInput(&ctx);
			k_sleep(K_MSEC(50));
			elapsed += 50;
		}
	}
	
	// 恢复终端状态
	WriteRaw(&ctx, CLEAR_SCREEN CURSOR_HOME CURSOR_SHOW);
	
	return 0;
}

} // namespace fanctl::htop
