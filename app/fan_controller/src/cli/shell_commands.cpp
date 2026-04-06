/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "shell_commands.hpp"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include "cli_runtime.hpp"
#include "core/common.hpp"
#include "htop_monitor.hpp"
#include "kilo_editor.hpp"
#include "line_editor.hpp"
#include "storage.hpp"
#include "wifi_manager.hpp"

namespace fanctl::shell_commands {

namespace {

cli::Runtime g_runtime = {};
LineEditor g_editor;
char g_cwd[128] = "/root";
char g_prompt[80] = "root@fanctl:/root$ ";
cli::State g_state = { &g_editor, g_cwd, sizeof(g_cwd) };

struct ShellKiloSession {
	const struct shell *sh;
	k_mutex lock;
	k_sem data_ready;
	k_sem finished;
	k_thread thread;
	char path[128];
	uint8_t buffer[512];
	size_t head;
	size_t tail;
	size_t count;
	bool active;
};

K_THREAD_STACK_DEFINE(g_kilo_stack, 12288);
ShellKiloSession g_kilo_session = {};

// htop monitor session
struct ShellHtopSession {
	const struct shell *sh;
	k_mutex lock;
	k_sem finished;
	k_thread thread;
	uint8_t buffer[256];
	size_t head;
	size_t tail;
	size_t count;
	bool active;
};

K_THREAD_STACK_DEFINE(g_htop_stack, 8192);
ShellHtopSession g_htop_session = {};

void UpdateShellPrompt(const struct shell *sh);

void ShellTransportWrite(const struct shell *sh, const char *text, size_t len)
{
	if (sh == nullptr || text == nullptr || len == 0U || sh->iface == nullptr ||
	    sh->iface->api == nullptr || sh->iface->api->write == nullptr) {
		return;
	}

	constexpr size_t kShellWriteChunk = 64U;
	constexpr int kShellWriteStallLimit = 200;
	size_t offset = 0U;
	int stall_count = 0;
	while (offset < len) {
		size_t request_len = MIN(len - offset, kShellWriteChunk);
		size_t written = 0U;
		int rc = sh->iface->api->write(sh->iface, text + offset, request_len, &written);
		if (rc != 0) {
			break;
		}

		if (written == 0U) {
			if (++stall_count >= kShellWriteStallLimit) {
				break;
			}
			k_sleep(K_MSEC(1));
			continue;
		}

		stall_count = 0;
		offset += written;
	}
}

void ShellWrite(void *ctx, const char *text, size_t len)
{
	const struct shell *sh = static_cast<const struct shell *>(ctx);
	shell_fprintf(sh, SHELL_NORMAL, "%.*s", static_cast<int>(len), text);
}

void ShellLine(void *ctx, const char *line)
{
	const struct shell *sh = static_cast<const struct shell *>(ctx);
	shell_print(sh, "%s", line);
}

cli::Io MakeIo(const struct shell *sh)
{
	cli::Io io = {};
	io.write = ShellWrite;
	io.line = ShellLine;
	io.ctx = const_cast<shell *>(sh);
	return io;
}

void KiloShellWrite(void *ctx, const char *text, size_t len)
{
	ShellKiloSession *session = static_cast<ShellKiloSession *>(ctx);
	if (session == nullptr) {
		return;
	}

	ShellTransportWrite(session->sh, text, len);
}

void KiloBypassCallback(const struct shell *sh, uint8_t *data, size_t len, void *user_data)
{
	ARG_UNUSED(sh);

	ShellKiloSession *session = static_cast<ShellKiloSession *>(user_data);
	if (session == nullptr || data == nullptr || len == 0U) {
		return;
	}

	k_mutex_lock(&session->lock, K_FOREVER);
	for (size_t i = 0; i < len; ++i) {
		if (session->count >= ARRAY_SIZE(session->buffer)) {
			break;
		}
		session->buffer[session->head] = data[i];
		session->head = (session->head + 1U) % ARRAY_SIZE(session->buffer);
		session->count++;
		k_sem_give(&session->data_ready);
	}
	k_mutex_unlock(&session->lock);
}

int KiloReadChar(void *ctx)
{
	ShellKiloSession *session = static_cast<ShellKiloSession *>(ctx);
	if (session == nullptr) {
		return -EINVAL;
	}

	while (true) {
		k_mutex_lock(&session->lock, K_FOREVER);
		if (session->count > 0U) {
			uint8_t ch = session->buffer[session->tail];
			session->tail = (session->tail + 1U) % ARRAY_SIZE(session->buffer);
			session->count--;
			k_mutex_unlock(&session->lock);
			return static_cast<int>(ch);
		}
		bool active = session->active;
		k_mutex_unlock(&session->lock);

		if (!active) {
			return -EIO;
		}

		(void)k_sem_take(&session->data_ready, K_FOREVER);
	}
}

int KiloPeekChar(void *ctx, char *ch)
{
	ShellKiloSession *session = static_cast<ShellKiloSession *>(ctx);
	if (session == nullptr || ch == nullptr) {
		return -EINVAL;
	}

	k_mutex_lock(&session->lock, K_FOREVER);
	if (session->count == 0U) {
		k_mutex_unlock(&session->lock);
		return 0;
	}

	*ch = static_cast<char>(session->buffer[session->tail]);
	k_mutex_unlock(&session->lock);
	return 1;
}

void KiloThreadEntry(void *ctx, void *, void *)
{
	ShellKiloSession *session = static_cast<ShellKiloSession *>(ctx);
	if (session == nullptr || session->sh == nullptr) {
		return;
	}

	kilo::Io io = {};
	io.read_char = KiloReadChar;
	io.peek_char = KiloPeekChar;
	io.write = KiloShellWrite;
	io.ctx = session;

	int rc = kilo::Run(io, session->path, 24, 80);
	shell_set_bypass(session->sh, nullptr, nullptr);
	if (rc != 0) {
		if (rc == -EFBIG) {
			shell_error(session->sh, "kilo file exceeds 200KB limit");
		} else {
			shell_error(session->sh, "kilo exited with error: %d", rc);
		}
	}

	k_mutex_lock(&session->lock, K_FOREVER);
	session->active = false;
	session->count = 0U;
	session->head = 0U;
	session->tail = 0U;
	k_mutex_unlock(&session->lock);
	UpdateShellPrompt(session->sh);
	k_sem_give(&session->finished);
}

// ========== htop monitor implementation ==========

void HtopShellWrite(void *ctx, const char *data, size_t len)
{
	ShellHtopSession *session = static_cast<ShellHtopSession *>(ctx);
	if (session == nullptr || session->sh == nullptr) {
		return;
	}
	shell_fprintf(session->sh, SHELL_NORMAL, "%.*s", static_cast<int>(len), data);
}

void HtopInputCallback(const struct shell *sh, uint8_t *data, size_t len, void *user_data)
{
	ARG_UNUSED(sh);

	ShellHtopSession *session = static_cast<ShellHtopSession *>(user_data);
	if (session == nullptr || data == nullptr || len == 0U) {
		return;
	}

	k_mutex_lock(&session->lock, K_FOREVER);
	for (size_t i = 0; i < len; ++i) {
		if (session->count >= ARRAY_SIZE(session->buffer)) {
			break;
		}
		session->buffer[session->head] = data[i];
		session->head = (session->head + 1U) % ARRAY_SIZE(session->buffer);
		session->count++;
	}
	k_mutex_unlock(&session->lock);
}

int HtopReadChar(void *ctx)
{
	ShellHtopSession *session = static_cast<ShellHtopSession *>(ctx);
	if (session == nullptr) {
		return -EINVAL;
	}

	while (true) {
		k_mutex_lock(&session->lock, K_FOREVER);
		if (session->count > 0U) {
			uint8_t ch = session->buffer[session->tail];
			session->tail = (session->tail + 1U) % ARRAY_SIZE(session->buffer);
			session->count--;
			k_mutex_unlock(&session->lock);
			return static_cast<int>(ch);
		}
		bool active = session->active;
		k_mutex_unlock(&session->lock);

		if (!active) {
			return -EIO;
		}

		k_sleep(K_MSEC(10));
	}
}

int HtopPeekChar(void *ctx, char *ch)
{
	ShellHtopSession *session = static_cast<ShellHtopSession *>(ctx);
	if (session == nullptr || ch == nullptr) {
		return -EINVAL;
	}

	k_mutex_lock(&session->lock, K_FOREVER);
	if (session->count == 0U) {
		k_mutex_unlock(&session->lock);
		return 0;
	}

	*ch = static_cast<char>(session->buffer[session->tail]);
	k_mutex_unlock(&session->lock);
	return 1;
}

void HtopThreadEntry(void *ctx, void *, void *)
{
	ShellHtopSession *session = static_cast<ShellHtopSession *>(ctx);
	if (session == nullptr || session->sh == nullptr) {
		return;
	}

	htop::Io io = {};
	io.read_char = HtopReadChar;
	io.peek_char = HtopPeekChar;
	io.write = HtopShellWrite;
	io.ctx = session;

	// 获取终端大小，使用默认值
	int rows = 24;
	int cols = 80;

	(void)htop::Run(io, rows, cols);
	shell_set_bypass(session->sh, nullptr, nullptr);

	k_mutex_lock(&session->lock, K_FOREVER);
	session->active = false;
	session->count = 0U;
	session->head = 0U;
	session->tail = 0U;
	k_mutex_unlock(&session->lock);
	UpdateShellPrompt(session->sh);
	k_sem_give(&session->finished);
}

// ========== htop command ==========

int CmdHtop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (sh == nullptr) {
		return -EINVAL;
	}

	k_mutex_lock(&g_htop_session.lock, K_FOREVER);
	if (g_htop_session.active) {
		k_mutex_unlock(&g_htop_session.lock);
		shell_error(sh, "htop is already running");
		return -EBUSY;
	}

	g_htop_session.sh = sh;
	g_htop_session.head = 0U;
	g_htop_session.tail = 0U;
	g_htop_session.count = 0U;
	g_htop_session.active = true;
	k_mutex_unlock(&g_htop_session.lock);

	shell_set_bypass(sh, HtopInputCallback, &g_htop_session);

	// 清屏并显示启动信息
	shell_fprintf(sh, SHELL_NORMAL, "\x1b[2J\x1b[HStarting htop... Press 'q' to quit\r\n");

	k_thread_create(&g_htop_session.thread, g_htop_stack,
			K_THREAD_STACK_SIZEOF(g_htop_stack),
			HtopThreadEntry, &g_htop_session, nullptr, nullptr,
			5, 0, K_NO_WAIT);
	k_thread_name_set(&g_htop_session.thread, "htop_ui");

	// 等待 htop 结束
	k_sem_take(&g_htop_session.finished, K_FOREVER);

	return 0;
}

void FillDynamicEntry(struct shell_static_entry *entry, const char *syntax, const char *help)
{
	entry->syntax = syntax;
	entry->handler = nullptr;
	entry->subcmd = nullptr;
	entry->help = help;
}

void UpdateShellPrompt(const struct shell *sh)
{
	const struct shell *target = (sh != nullptr) ? sh : shell_backend_uart_get_ptr();
	if (target == nullptr) {
		return;
	}

	cli::BuildPrompt(g_state, g_prompt, sizeof(g_prompt));
	(void)shell_prompt_change(target, g_prompt);
}

bool GetDirectoryEntryName(const char *user_dir, size_t entry_idx, char *out, size_t out_len)
{
	char fs_path[160];
	if (storage::ResolveManagedPath(user_dir, fs_path, sizeof(fs_path)) != 0) {
		return false;
	}

	struct fs_dir_t dir;
	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, fs_path) != 0) {
		return false;
	}

	size_t current = 0U;
	bool found = false;
	while (true) {
		struct fs_dirent entry = {};
		int rc = fs_readdir(&dir, &entry);
		if (rc != 0 || entry.name[0] == '\0') {
			break;
		}

		if (current++ == entry_idx) {
			(void)snprintf(out, out_len, "%s", entry.name);
			found = true;
			break;
		}
	}

	(void)fs_closedir(&dir);
	return found;
}

void PathDynamicGet(size_t idx, struct shell_static_entry *entry)
{
	static char syntax[160];
	char name[128];

	if (idx == 0U) {
		FillDynamicEntry(entry, ".", "current directory");
		return;
	}
	if (idx == 1U) {
		FillDynamicEntry(entry, "..", "parent directory");
		return;
	}
	if (idx == 2U) {
		FillDynamicEntry(entry, "~", "home directory");
		return;
	}

	idx -= 3U;
	if (GetDirectoryEntryName(g_state.cwd, idx, name, sizeof(name))) {
		(void)snprintf(syntax, sizeof(syntax), "%s", name);
		FillDynamicEntry(entry, syntax, "path");
		return;
	}

	entry->syntax = nullptr;
}

SHELL_DYNAMIC_CMD_CREATE(dsub_paths, PathDynamicGet);

int RunFanctl(const struct shell *sh, size_t argc, char **argv)
{
	char *cmd[8];
	cli::CommandResult result = {};
	cli::Io io = MakeIo(sh);

	cmd[0] = const_cast<char *>("fanctl");
	for (size_t i = 0; i < argc; ++i) {
		cmd[i + 1U] = argv[i];
	}

	int rc = cli::HandleFanctl(g_runtime, g_state, cmd, static_cast<int>(argc + 1U), io, &result);
	if (result.reboot_requested) {
		k_sleep(K_MSEC(50));
		sys_reboot(SYS_REBOOT_COLD);
	}
	return rc;
}

int RunShow(const struct shell *sh, size_t argc, char **argv)
{
	char *cmd[8];
	cli::Io io = MakeIo(sh);

	cmd[0] = const_cast<char *>("show");
	for (size_t i = 0; i < argc; ++i) {
		cmd[i + 1U] = argv[i];
	}

	return cli::HandleShow(g_runtime, g_state, cmd, static_cast<int>(argc + 1U), io);
}

int RunEdit(const struct shell *sh, size_t argc, char **argv)
{
	char *cmd[20];
	cli::Io io = MakeIo(sh);

	cmd[0] = const_cast<char *>("edit");
	for (size_t i = 0; i < argc; ++i) {
		cmd[i + 1U] = argv[i];
	}

	return cli::HandleEdit(g_runtime, g_state, cmd, static_cast<int>(argc + 1U), io);
}

int CmdStatus(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	cli::PrintStatus(g_runtime, MakeIo(sh));
	return 0;
}

int CmdSet(const struct shell *sh, size_t argc, char **argv)
{
	return RunFanctl(sh, argc, argv);
}

int CmdWifi(const struct shell *sh, size_t argc, char **argv)
{
	return RunFanctl(sh, argc, argv);
}

int CmdAp(const struct shell *sh, size_t argc, char **argv)
{
	return RunFanctl(sh, argc, argv);
}

int CmdClearWifi(const struct shell *sh, size_t argc, char **argv)
{
	return RunFanctl(sh, argc, argv);
}

int CmdConfig(const struct shell *sh, size_t argc, char **argv)
{
	return RunFanctl(sh, argc, argv);
}

int CmdFactoryReset(const struct shell *sh, size_t argc, char **argv)
{
	return RunFanctl(sh, argc, argv);
}

int CmdReboot(const struct shell *sh, size_t argc, char **argv)
{
	return RunFanctl(sh, argc, argv);
}

int CmdShowFans(const struct shell *sh, size_t argc, char **argv)
{
	return RunShow(sh, argc, argv);
}

int CmdShowCurves(const struct shell *sh, size_t argc, char **argv)
{
	return RunShow(sh, argc, argv);
}

int CmdShowSystem(const struct shell *sh, size_t argc, char **argv)
{
	return RunShow(sh, argc, argv);
}

int CmdShowWifi(const struct shell *sh, size_t argc, char **argv)
{
	return RunShow(sh, argc, argv);
}

int CmdShowStorage(const struct shell *sh, size_t argc, char **argv)
{
	return RunShow(sh, argc, argv);
}

int CmdShowHost(const struct shell *sh, size_t argc, char **argv)
{
	return RunShow(sh, argc, argv);
}

int CmdShowNtp(const struct shell *sh, size_t argc, char **argv)
{
	return RunShow(sh, argc, argv);
}

int CmdShowSsh(const struct shell *sh, size_t argc, char **argv)
{
	return RunShow(sh, argc, argv);
}

int CmdShowAll(const struct shell *sh, size_t argc, char **argv)
{
	return RunShow(sh, argc, argv);
}

int CmdLs(const struct shell *sh, size_t argc, char **argv)
{
	return cli::HandleLs(g_runtime, g_state, argc > 1 ? argv[1] : g_state.cwd, MakeIo(sh));
}

int CmdCat(const struct shell *sh, size_t argc, char **argv)
{
	return cli::HandleCat(g_runtime, g_state, argv[1], MakeIo(sh));
}

int CmdTouch(const struct shell *sh, size_t argc, char **argv)
{
	return cli::HandleTouch(g_runtime, g_state, argv[1], MakeIo(sh));
}

int CmdMkdir(const struct shell *sh, size_t argc, char **argv)
{
	return cli::HandleMkdir(g_runtime, g_state, argv[1], MakeIo(sh));
}

int CmdRm(const struct shell *sh, size_t argc, char **argv)
{
	return cli::HandleRm(g_runtime, g_state, argv[1], MakeIo(sh));
}

int CmdCp(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	return cli::HandleCp(g_runtime, g_state, argv[1], argv[2], MakeIo(sh));
}

int CmdMv(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	return cli::HandleMv(g_runtime, g_state, argv[1], argv[2], MakeIo(sh));
}

int CmdWriteFile(const struct shell *sh, size_t argc, char **argv)
{
	return cli::HandleWriteFile(g_runtime, g_state, argv, static_cast<int>(argc), MakeIo(sh));
}

int CmdCd(const struct shell *sh, size_t argc, char **argv)
{
	int rc = cli::HandleCd(g_runtime, &g_state, argc > 1 ? argv[1] : "/root", MakeIo(sh));
	if (rc == 0) {
		UpdateShellPrompt(sh);
	}
	return rc;
}

int CmdPwd(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return cli::HandlePwd(g_state, MakeIo(sh));
}

int CmdEditOpen(const struct shell *sh, size_t argc, char **argv)
{
	return RunEdit(sh, argc, argv);
}

int CmdEditStatus(const struct shell *sh, size_t argc, char **argv)
{
	return RunEdit(sh, argc, argv);
}

int CmdEditShow(const struct shell *sh, size_t argc, char **argv)
{
	return RunEdit(sh, argc, argv);
}

int CmdEditSet(const struct shell *sh, size_t argc, char **argv)
{
	return RunEdit(sh, argc, argv);
}

int CmdEditInsert(const struct shell *sh, size_t argc, char **argv)
{
	return RunEdit(sh, argc, argv);
}

int CmdEditAppend(const struct shell *sh, size_t argc, char **argv)
{
	return RunEdit(sh, argc, argv);
}

int CmdEditDelete(const struct shell *sh, size_t argc, char **argv)
{
	return RunEdit(sh, argc, argv);
}

int CmdEditWrite(const struct shell *sh, size_t argc, char **argv)
{
	return RunEdit(sh, argc, argv);
}

int CmdEditSaveAs(const struct shell *sh, size_t argc, char **argv)
{
	return RunEdit(sh, argc, argv);
}

int CmdEditQuit(const struct shell *sh, size_t argc, char **argv)
{
	return RunEdit(sh, argc, argv);
}

int CmdStorageSummary(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return cli::HandleStorageSummary(MakeIo(sh));
}

int CmdSimpleTop(const struct shell *sh, size_t argc, char **argv)
{
	return cli::HandleTop(g_runtime, argv, static_cast<int>(argc), MakeIo(sh));
}

int CmdKilo(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (sh == nullptr) {
		return -EINVAL;
	}

	k_mutex_lock(&g_kilo_session.lock, K_FOREVER);
	if (g_kilo_session.active) {
		k_mutex_unlock(&g_kilo_session.lock);
		shell_error(sh, "kilo is already running");
		return -EBUSY;
	}
	k_mutex_unlock(&g_kilo_session.lock);

	char resolved[128];
	int rc = cli::ResolvePath(g_state, argv[1], resolved, sizeof(resolved));
	if (rc != 0) {
		shell_error(sh, "kilo: invalid path: %d", rc);
		return rc;
	}

	k_mutex_lock(&g_kilo_session.lock, K_FOREVER);
	g_kilo_session.sh = sh;
	g_kilo_session.head = 0U;
	g_kilo_session.tail = 0U;
	g_kilo_session.count = 0U;
	g_kilo_session.active = true;
	(void)snprintf(g_kilo_session.path, sizeof(g_kilo_session.path), "%s", resolved);
	k_mutex_unlock(&g_kilo_session.lock);

	while (k_sem_take(&g_kilo_session.data_ready, K_NO_WAIT) == 0) {
	}
	while (k_sem_take(&g_kilo_session.finished, K_NO_WAIT) == 0) {
	}

	shell_set_bypass(sh, KiloBypassCallback, &g_kilo_session);
	(void)k_thread_create(&g_kilo_session.thread, g_kilo_stack,
			      K_THREAD_STACK_SIZEOF(g_kilo_stack), KiloThreadEntry,
			      &g_kilo_session, nullptr, nullptr, K_PRIO_PREEMPT(8), 0,
			      K_NO_WAIT);
	k_thread_name_set(&g_kilo_session.thread, "fanctl_kilo");
	return 0;
}

int CmdScan(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Starting Wi-Fi scan...");

	int rc = g_runtime.wifi_manager->StartScan();
	if (rc != 0) {
		shell_error(sh, "Scan failed: %d", rc);
		return rc;
	}

	int timeout = 100;
	while (!g_runtime.wifi_manager->IsScanComplete() && timeout-- > 0) {
		k_sleep(K_MSEC(100));
	}

	if (!g_runtime.wifi_manager->IsScanComplete()) {
		shell_warn(sh, "Scan timeout");
	} else if (g_runtime.wifi_manager->GetScanStatus() != 0) {
		shell_warn(sh, "Scan finished with status %d", g_runtime.wifi_manager->GetScanStatus());
	}

	static WifiScanResult scan_results[16];
	size_t count = 0;
	g_runtime.wifi_manager->GetScanResults(scan_results, ARRAY_SIZE(scan_results), &count);

	if (count == 0) {
		shell_print(sh, "No networks found");
		return 0;
	}

	shell_print(sh, "Found %u network(s):", static_cast<unsigned int>(count));
	shell_print(sh, "%-4s %-32s %-8s %-6s %s", "No.", "SSID", "RSSI", "CH", "Security");

	for (size_t i = 0; i < count; ++i) {
		const char *security = "Unknown";
		switch (scan_results[i].security) {
		case WIFI_SECURITY_TYPE_NONE: security = "Open"; break;
		case WIFI_SECURITY_TYPE_PSK: security = "WPA/WPA2"; break;
		case WIFI_SECURITY_TYPE_PSK_SHA256: security = "WPA2"; break;
		case WIFI_SECURITY_TYPE_SAE: security = "WPA3"; break;
		default: break;
		}
		shell_print(sh, "%-4u %-32s %-8d %-6u %s",
			    static_cast<unsigned int>(i + 1), scan_results[i].ssid,
			    scan_results[i].rssi, scan_results[i].channel, security);
	}

	return 0;
}

int CmdWifiConnect(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 1) {
		shell_print(sh, "Wi-Fi Connection Utility");
		shell_print(sh, "========================");
		shell_print(sh, "");
		shell_print(sh, "Starting scan...");

		int rc = g_runtime.wifi_manager->StartScan();
		if (rc != 0) {
			shell_error(sh, "Scan failed: %d", rc);
			return rc;
		}

		int timeout = 100;
		while (!g_runtime.wifi_manager->IsScanComplete() && timeout-- > 0) {
			k_sleep(K_MSEC(100));
		}

		if (!g_runtime.wifi_manager->IsScanComplete()) {
			shell_warn(sh, "Scan timeout");
		} else if (g_runtime.wifi_manager->GetScanStatus() != 0) {
			shell_warn(sh, "Scan finished with status %d",
				   g_runtime.wifi_manager->GetScanStatus());
		}

		static WifiScanResult connect_scan_results[16];
		size_t count = 0;
		g_runtime.wifi_manager->GetScanResults(connect_scan_results, ARRAY_SIZE(connect_scan_results),
						       &count);

		if (count == 0) {
			shell_print(sh, "No networks found");
			return 0;
		}

		shell_print(sh, "");
		shell_print(sh, "%-4s %-32s %-8s %-6s %s", "No.", "SSID", "RSSI", "CH", "Security");
		for (size_t i = 0; i < count; ++i) {
			const char *security = "Unknown";
			switch (connect_scan_results[i].security) {
			case WIFI_SECURITY_TYPE_NONE: security = "Open"; break;
			case WIFI_SECURITY_TYPE_PSK: security = "WPA/WPA2"; break;
			case WIFI_SECURITY_TYPE_PSK_SHA256: security = "WPA2"; break;
			case WIFI_SECURITY_TYPE_SAE: security = "WPA3"; break;
			default: break;
			}
			shell_print(sh, "%-4u %-32s %-8d %-6u %s",
				    static_cast<unsigned int>(i + 1), connect_scan_results[i].ssid,
				    connect_scan_results[i].rssi, connect_scan_results[i].channel, security);
		}

		shell_print(sh, "");
		shell_print(sh, "Usage: wificonnect <number> [password]");
		shell_print(sh, "Example: wificonnect 1 mypassword");
		return 0;
	}

	long choice = strtol(argv[1], nullptr, 10);
	if (choice <= 0) {
		shell_error(sh, "Invalid network number");
		return -EINVAL;
	}

	static WifiScanResult connect_scan_results[16];
	size_t count = 0;
	g_runtime.wifi_manager->GetScanResults(connect_scan_results, ARRAY_SIZE(connect_scan_results),
					       &count);

	if (choice > static_cast<long>(count)) {
		shell_error(sh, "Network number out of range, run 'wificonnect' first to scan");
		return -EINVAL;
	}

	const WifiScanResult &selected = connect_scan_results[choice - 1];
	const char *psk = "";
	if (argc > 2) {
		psk = argv[2];
	} else if (selected.security != WIFI_SECURITY_TYPE_NONE) {
		shell_error(sh, "Network requires password: wificonnect %s <password>", argv[1]);
		return -EINVAL;
	}

	shell_print(sh, "Connecting to %s...", selected.ssid);
	int rc = g_runtime.wifi_manager->SaveAndConnect(selected.ssid, psk);
	if (rc != 0) {
		shell_error(sh, "Connection failed: %d", rc);
		return rc;
	}

	shell_print(sh, "Connection request sent.");
	cli::PrintStatus(g_runtime, MakeIo(sh));
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_fanctl,
	SHELL_CMD(status, NULL, "Show Wi-Fi and fan status", CmdStatus),
	SHELL_CMD_ARG(set, NULL, "fanctl set <1|2> <0-100> <on|off>", CmdSet, 4, 0),
	SHELL_CMD_ARG(wifi, NULL, "fanctl wifi <ssid> [psk]", CmdWifi, 2, 1),
	SHELL_CMD_ARG(ap, NULL, "fanctl ap <on|off>", CmdAp, 2, 0),
	SHELL_CMD(scan, NULL, "Scan for Wi-Fi networks", CmdScan),
	SHELL_CMD(config, NULL, "Show JSON config file", CmdConfig),
	SHELL_CMD(clearwifi, NULL, "Clear stored Wi-Fi credentials", CmdClearWifi),
	SHELL_CMD_ARG(factoryreset, NULL, "fanctl factoryreset confirm", CmdFactoryReset, 2, 0),
	SHELL_CMD(reboot, NULL, "Reboot the controller", CmdReboot),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_edit,
	SHELL_CMD_ARG(open, &dsub_paths, "edit open <path>", CmdEditOpen, 2, 0),
	SHELL_CMD(status, NULL, "Show current editor state", CmdEditStatus),
	SHELL_CMD_ARG(show, NULL, "edit show [start] [end]", CmdEditShow, 1, 2),
	SHELL_CMD_ARG(set, NULL, "edit set <line> <text>", CmdEditSet, 3, 16),
	SHELL_CMD_ARG(ins, NULL, "edit ins <line> <text>", CmdEditInsert, 3, 16),
	SHELL_CMD_ARG(app, NULL, "edit app <text>", CmdEditAppend, 2, 16),
	SHELL_CMD_ARG(del, NULL, "edit del <line>", CmdEditDelete, 2, 0),
	SHELL_CMD(write, NULL, "Save current file", CmdEditWrite),
	SHELL_CMD_ARG(saveas, &dsub_paths, "edit saveas <path>", CmdEditSaveAs, 2, 0),
	SHELL_CMD(quit, NULL, "Close editor", CmdEditQuit),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_show,
	SHELL_CMD_ARG(fans, NULL, "show fans [1|2]", CmdShowFans, 1, 1),
	SHELL_CMD(curves, NULL, "show curves", CmdShowCurves),
	SHELL_CMD(system, NULL, "show system", CmdShowSystem),
	SHELL_CMD(wifi, NULL, "show wifi", CmdShowWifi),
	SHELL_CMD(storage, NULL, "show storage", CmdShowStorage),
	SHELL_CMD(host, NULL, "show host", CmdShowHost),
	SHELL_CMD(ntp, NULL, "show ntp", CmdShowNtp),
	SHELL_CMD(ssh, NULL, "show ssh", CmdShowSsh),
	SHELL_CMD(all, NULL, "show all", CmdShowAll),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(fanctl, &sub_fanctl, "Fan controller commands", NULL);
SHELL_CMD_REGISTER(edit, &sub_edit, "Minimal line editor", NULL);
SHELL_CMD_REGISTER(show, &sub_show, "Operational show commands", NULL);
SHELL_CMD_ARG_REGISTER(ls, &dsub_paths, "ls [path]", CmdLs, 1, 1);
SHELL_CMD_ARG_REGISTER(cat, &dsub_paths, "cat <path>", CmdCat, 2, 0);
SHELL_CMD_REGISTER(cf, NULL, "Show storage capacity/free summary", CmdStorageSummary);
SHELL_CMD_REGISTER(duf, NULL, "Show storage capacity/free summary", CmdStorageSummary);
SHELL_CMD_ARG_REGISTER(touch, &dsub_paths, "touch <path>", CmdTouch, 2, 0);
SHELL_CMD_ARG_REGISTER(mkdir, &dsub_paths, "mkdir <path>", CmdMkdir, 2, 0);
SHELL_CMD_ARG_REGISTER(cd, &dsub_paths, "cd <path>", CmdCd, 1, 1);
SHELL_CMD_REGISTER(pwd, NULL, "Print current directory", CmdPwd);
SHELL_CMD_ARG_REGISTER(rm, &dsub_paths, "rm <path>", CmdRm, 2, 0);
SHELL_CMD_ARG_REGISTER(cp, &dsub_paths, "cp <source> <target>", CmdCp, 3, 0);
SHELL_CMD_ARG_REGISTER(mv, &dsub_paths, "mv <source> <target>", CmdMv, 3, 0);
SHELL_CMD_ARG_REGISTER(kilo, &dsub_paths, "kilo <path>", CmdKilo, 2, 0);
SHELL_CMD_ARG_REGISTER(writefile, &dsub_paths, "writefile <path> <text>", CmdWriteFile, 3, 16);
SHELL_CMD_ARG_REGISTER(wificonnect, NULL, "Wi-Fi connection utility: wificonnect [number] [password]",
		       CmdWifiConnect, 1, 2);
SHELL_CMD_ARG_REGISTER(simpletop, NULL, "Simple live monitor: simpletop [samples] [interval_ms]", CmdSimpleTop, 1, 2);
SHELL_CMD_ARG_REGISTER(htop, NULL, "Interactive process viewer (htop)", CmdHtop, 1, 0);

} // namespace

void Init(const ServiceContext &services)
{
	g_runtime = services;
	cli::ResetState(&g_state);
	k_mutex_init(&g_kilo_session.lock);
	k_sem_init(&g_kilo_session.data_ready, 0, UINT_MAX);
	k_sem_init(&g_kilo_session.finished, 0, 1);
	g_kilo_session.active = false;
	
	k_mutex_init(&g_htop_session.lock);
	k_sem_init(&g_htop_session.finished, 0, 1);
	g_htop_session.active = false;
	
	UpdateShellPrompt(nullptr);
}

} // namespace fanctl::shell_commands
