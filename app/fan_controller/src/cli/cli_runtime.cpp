/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cli_runtime.hpp"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/fs/fs.h>

#include "core/common.hpp"
#include "curve_profiles.hpp"
#include "settings_store.hpp"
#include "show_status.hpp"
#include "storage.hpp"

namespace fanctl::cli {

namespace {

int SaveEditorToTarget(const Runtime &runtime, LineEditor &editor, const char *path, const Io &io)
{
	if (strcmp(path, settings::GetConfigRelativePath()) == 0) {
		int rc = settings::WriteConfigJson(editor.GetContent(), strlen(editor.GetContent()));
		if (rc != 0) {
			EmitLinef(io, "config validation failed: %d", rc);
			return rc;
		}

		rc = ApplyConfigFromStore(runtime);
		if (rc != 0) {
			EmitLinef(io, "config apply failed: %d", rc);
			return rc;
		}

		return 0;
	}

	if (strcmp(path, settings::GetSshConfigRelativePath()) == 0) {
		int rc = settings::WriteSshConfigJson(editor.GetContent(), strlen(editor.GetContent()));
		if (rc != 0) {
			EmitLinef(io, "save failed: %d", rc);
		}
		return rc;
	}

	int rc = storage::WriteTextFile(path, editor.GetContent(), strlen(editor.GetContent()));
	if (rc != 0) {
		EmitLinef(io, "save failed: %d", rc);
	}
	return rc;
}

} // namespace

void ResetState(State *state)
{
	if (state == nullptr || state->cwd == nullptr || state->cwd_len == 0U) {
		return;
	}

	if (state->editor != nullptr) {
		state->editor->Close();
	}
	(void)snprintf(state->cwd, state->cwd_len, "/root");
}

void BuildPrompt(const State &state, char *buffer, size_t buffer_len)
{
	if (buffer == nullptr || buffer_len == 0U) {
		return;
	}

	(void)snprintf(buffer, buffer_len, "root@%s:%s# ", kDeviceHostname, state.cwd);
}

void Emit(const Io &io, const char *text)
{
	if (io.write == nullptr || text == nullptr) {
		return;
	}

	io.write(io.ctx, text, strlen(text));
}

void EmitLine(const Io &io, const char *text)
{
	if (io.line != nullptr) {
		io.line(io.ctx, text != nullptr ? text : "");
		return;
	}

	if (text != nullptr) {
		Emit(io, text);
	}
	Emit(io, "\n");
}

void Emitf(const Io &io, const char *fmt, ...)
{
	if (fmt == nullptr) {
		return;
	}

	char buffer[384];
	va_list args;

	va_start(args, fmt);
	int written = vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	if (written > 0) {
		io.write(io.ctx, buffer, static_cast<size_t>(written < static_cast<int>(sizeof(buffer)) ? written
													 : static_cast<int>(sizeof(buffer) - 1)));
	}
}

void EmitLinef(const Io &io, const char *fmt, ...)
{
	if (fmt == nullptr) {
		return;
	}

	char buffer[256];
	va_list args;

	va_start(args, fmt);
	(void)vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	EmitLine(io, buffer);
}

int ResolvePath(const State &state, const char *input, char *output, size_t output_len)
{
	if (output == nullptr || output_len == 0U) {
		return -EINVAL;
	}

	const char *source = input;
	if (source == nullptr || source[0] == '\0') {
		source = state.cwd;
	}

	char combined[192];
	if (source[0] == '/') {
		(void)snprintf(combined, sizeof(combined), "%s", source);
	} else if (source[0] == '~') {
		if (source[1] == '\0') {
			(void)snprintf(combined, sizeof(combined), "/root");
		} else if (source[1] == '/') {
			(void)snprintf(combined, sizeof(combined), "/root/%s", source + 2);
		} else {
			return -EINVAL;
		}
	} else if (strcmp(state.cwd, "/") == 0) {
		(void)snprintf(combined, sizeof(combined), "/%s", source);
	} else {
		(void)snprintf(combined, sizeof(combined), "%s/%s", state.cwd, source);
	}

	char scratch[192];
	(void)snprintf(scratch, sizeof(scratch), "%s", combined);

	char *segments[16];
	size_t segment_count = 0U;
	char *save = nullptr;
	for (char *segment = strtok_r(scratch, "/", &save); segment != nullptr;
	     segment = strtok_r(nullptr, "/", &save)) {
		if (strcmp(segment, ".") == 0 || segment[0] == '\0') {
			continue;
		}
		if (strcmp(segment, "..") == 0) {
			if (segment_count > 0U) {
				--segment_count;
			}
			continue;
		}
		if (segment_count >= ARRAY_SIZE(segments)) {
			return -ENOSPC;
		}
		segments[segment_count++] = segment;
	}

	size_t offset = 0U;
	output[offset++] = '/';
	output[offset] = '\0';
	for (size_t i = 0U; i < segment_count; ++i) {
		int written = snprintf(output + offset, output_len - offset, "%s%s",
				       i == 0U ? "" : "/", segments[i]);
		if (written <= 0 || static_cast<size_t>(written) >= output_len - offset) {
			return -ENOSPC;
		}
		offset += static_cast<size_t>(written);
	}

	return 0;
}

int JoinTokens(char *buffer, size_t buffer_len, char *argv[], int argc, int start)
{
	if (buffer == nullptr || buffer_len == 0U || argv == nullptr || start >= argc) {
		return -EINVAL;
	}

	size_t offset = 0U;
	buffer[0] = '\0';
	for (int i = start; i < argc; ++i) {
		int written = snprintf(buffer + offset, buffer_len - offset, "%s%s",
				       i == start ? "" : " ", argv[i]);
		if (written <= 0 || static_cast<size_t>(written) >= buffer_len - offset) {
			return -ENOSPC;
		}
		offset += static_cast<size_t>(written);
	}

	return 0;
}

int ApplyConfigFromStore(const Runtime &runtime)
{
	settings::AppConfig config = {};
	int rc = settings::LoadConfig(&config);

	if (rc != 0) {
		return rc;
	}

	if (runtime.host_control != nullptr) {
		runtime.host_control->Configure(config);
	}

	settings::WifiConfig wifi_config = {};
	if (settings::LoadWifiConfig(&wifi_config) != 0) {
		settings::FillWifiDefaults(&wifi_config);
	}

	if (runtime.wifi_manager == nullptr) {
		return 0;
	}

	return (wifi_config.sta_ssid[0] == '\0')
		       ? runtime.wifi_manager->ClearCredentials()
		       : runtime.wifi_manager->SaveAndConnect(wifi_config.sta_ssid, wifi_config.sta_psk);
}

void PrintStatus(const Runtime &runtime, const Io &io)
{
	if (runtime.fan_controller == nullptr || runtime.wifi_manager == nullptr) {
		EmitLine(io, "status unavailable");
		return;
	}

	FanState fans[kFanCount];
	WifiSnapshot wifi = {};

	runtime.fan_controller->GetAllStates(fans);
	runtime.wifi_manager->GetSnapshot(&wifi);

	EmitLinef(io, "AP: %s  SSID: %s  IP: %s  Clients: %d",
		  wifi.ap_enabled ? "on" : "off", wifi.ap_ssid, kApIpAddr, wifi.ap_clients);
	EmitLinef(io, "STA: %s  Saved SSID: %s  IP: %s  RSSI: %d",
		  wifi.sta_connected ? "connected" : wifi.sta_state,
		  wifi.saved_ssid[0] != '\0' ? wifi.saved_ssid : "-",
		  wifi.sta_ip[0] != '\0' ? wifi.sta_ip : "-", wifi.sta_rssi);
	EmitLinef(io, "HTTP: AP=http://%s/  STA=http://%s/",
		  kApIpAddr, wifi.sta_ip[0] != '\0' ? wifi.sta_ip : "pending");

	for (size_t i = 0; i < kFanCount; ++i) {
		EmitLinef(io,
			  "Fan %u: enabled=%s adc_mode=%s manual=%u%% effective=%u%% pwm=%u%% rpm=%d adc_raw=%d voltage=%d mV",
			  static_cast<unsigned int>(i + 1), fans[i].enabled ? "true" : "false",
			  fans[i].use_adc_target ? "true" : "false", fans[i].percent,
			  fans[i].effective_percent, fans[i].pwm_percent, fans[i].actual_rpm,
			  fans[i].adc_raw, fans[i].mapped_voltage_mv);
	}
}

void EmitHelp(const Io &io)
{
	Emit(io,
	     "Commands:\n"
	     "  help\n"
	     "  pwd\n"
	     "  cd [path]\n"
	     "  ls [path]\n"
	     "  cat <path>\n"
	     "  cf\n"
	     "  duf\n"
	     "  touch <path>\n"
	     "  mkdir <path>\n"
	     "  rm <path>\n"
	     "  cp <source> <target>\n"
	     "  mv <source> <target>\n"
	     "  writefile <path> <text>\n"
	     "  fanctl status|set|wifi|ap|config|clearwifi|factoryreset\n"
	     "  show fans [1|2]|curves|system|wifi|storage|host|ntp|ssh|all\n"
	     "  top [samples] [interval_ms]\n"
	     "  edit open|status|show|set|ins|app|del|write|saveas|quit\n"
	     "  kilo <path>\n"
	     "  whoami\n"
	     "  hostname\n"
	     "  uname\n"
	     "  clear\n"
	     "  exit\n"
	     "  reboot\n");
}

int HandleLs(const Runtime &, const State &state, const char *path, const Io &io)
{
	char user_path[128];
	int rc = ResolvePath(state, path, user_path, sizeof(user_path));
	if (rc != 0) {
		EmitLinef(io, "invalid path: %d", rc);
		return rc;
	}

	char fs_path[128];
	rc = storage::ResolveManagedPath(user_path, fs_path, sizeof(fs_path));
	if (rc != 0) {
		EmitLinef(io, "invalid path: %d", rc);
		return rc;
	}

	struct fs_dir_t dir;
	fs_dir_t_init(&dir);
	rc = fs_opendir(&dir, fs_path);
	if (rc != 0) {
		EmitLinef(io, "opendir failed: %d", rc);
		return rc;
	}

	EmitLinef(io, "Listing %s", user_path);
	while (true) {
		struct fs_dirent entry = {};
		rc = fs_readdir(&dir, &entry);
		if (rc != 0) {
			EmitLinef(io, "readdir failed: %d", rc);
			break;
		}
		if (entry.name[0] == '\0') {
			rc = 0;
			break;
		}
		EmitLinef(io, "%c %8u %s", entry.type == FS_DIR_ENTRY_DIR ? 'd' : '-',
			  static_cast<unsigned int>(entry.size), entry.name);
	}

	(void)fs_closedir(&dir);
	return rc;
}

int HandleCat(const Runtime &, const State &state, const char *path, const Io &io)
{
	char user_path[128];
	int rc = ResolvePath(state, path, user_path, sizeof(user_path));
	if (rc != 0) {
		EmitLinef(io, "invalid path: %d", rc);
		return rc;
	}

	char fs_path[128];
	rc = storage::ResolveManagedPath(user_path, fs_path, sizeof(fs_path));
	if (rc != 0) {
		EmitLinef(io, "invalid path: %d", rc);
		return rc;
	}

	struct fs_file_t file;
	char chunk[128];

	fs_file_t_init(&file);
	rc = fs_open(&file, fs_path, FS_O_READ);
	if (rc != 0) {
		EmitLinef(io, "open failed: %d", rc);
		return rc;
	}

	while (true) {
		ssize_t read_len = fs_read(&file, chunk, sizeof(chunk) - 1U);
		if (read_len < 0) {
			rc = static_cast<int>(read_len);
			EmitLinef(io, "read failed: %d", rc);
			break;
		}
		if (read_len == 0) {
			rc = 0;
			break;
		}
		chunk[read_len] = '\0';
		Emit(io, chunk);
	}

	(void)fs_close(&file);
	Emit(io, "\n");
	return rc;
}

int HandleTouch(const Runtime &, const State &state, const char *path, const Io &io)
{
	char user_path[128];
	int rc = ResolvePath(state, path, user_path, sizeof(user_path));
	if (rc != 0) {
		EmitLinef(io, "touch: invalid path (%d)", rc);
		return rc;
	}

	if (storage::PathExists(user_path)) {
		EmitLinef(io, "updated %s", user_path);
		return 0;
	}

	rc = storage::WriteTextFile(user_path, "", 0U);
	if (rc != 0) {
		EmitLinef(io, "touch: failed (%d)", rc);
		return rc;
	}

	EmitLinef(io, "created %s", user_path);
	return 0;
}

int HandleMkdir(const Runtime &, const State &state, const char *path, const Io &io)
{
	char user_path[128];
	int rc = ResolvePath(state, path, user_path, sizeof(user_path));
	if (rc != 0) {
		EmitLinef(io, "mkdir failed: %d", rc);
		return rc;
	}

	rc = storage::MakeDirectory(user_path);
	if (rc != 0) {
		EmitLinef(io, "mkdir failed: %d", rc);
		return rc;
	}

	EmitLinef(io, "Created %s", user_path);
	return 0;
}

int HandleRm(const Runtime &, const State &state, const char *path, const Io &io)
{
	char user_path[128];
	int rc = ResolvePath(state, path, user_path, sizeof(user_path));
	if (rc != 0) {
		EmitLinef(io, "rm failed: %d", rc);
		return rc;
	}

	rc = storage::DeletePath(user_path);
	if (rc != 0) {
		EmitLinef(io, "rm failed: %d", rc);
		return rc;
	}

	EmitLinef(io, "Deleted %s", user_path);
	return 0;
}

int HandleCp(const Runtime &, const State &state, const char *source, const char *target,
	     const Io &io)
{
	char source_path[128];
	char target_path[128];
	int rc = ResolvePath(state, source, source_path, sizeof(source_path));
	if (rc != 0) {
		EmitLinef(io, "cp: invalid source (%d)", rc);
		return rc;
	}

	rc = ResolvePath(state, target, target_path, sizeof(target_path));
	if (rc != 0) {
		EmitLinef(io, "cp: invalid target (%d)", rc);
		return rc;
	}

	rc = storage::CopyPath(source_path, target_path);
	if (rc != 0) {
		EmitLinef(io, "cp failed: %d", rc);
		return rc;
	}

	EmitLinef(io, "Copied %s -> %s", source_path, target_path);
	return 0;
}

int HandleMv(const Runtime &, const State &state, const char *source, const char *target,
	     const Io &io)
{
	char source_path[128];
	char target_path[128];
	int rc = ResolvePath(state, source, source_path, sizeof(source_path));
	if (rc != 0) {
		EmitLinef(io, "mv: invalid source (%d)", rc);
		return rc;
	}

	rc = ResolvePath(state, target, target_path, sizeof(target_path));
	if (rc != 0) {
		EmitLinef(io, "mv: invalid target (%d)", rc);
		return rc;
	}

	rc = storage::MovePath(source_path, target_path);
	if (rc != 0) {
		EmitLinef(io, "mv failed: %d", rc);
		return rc;
	}

	EmitLinef(io, "Moved %s -> %s", source_path, target_path);
	return 0;
}

int HandleWriteFile(const Runtime &runtime, const State &state, char *argv[], int argc, const Io &io)
{
	char content[768];
	int rc = JoinTokens(content, sizeof(content), argv, argc, 2);
	if (rc != 0) {
		EmitLine(io, "content too long or invalid");
		return rc;
	}

	char user_path[128];
	rc = ResolvePath(state, argv[1], user_path, sizeof(user_path));
	if (rc != 0) {
		EmitLinef(io, "invalid path: %d", rc);
		return rc;
	}

	if (strcmp(user_path, settings::GetConfigRelativePath()) == 0) {
		rc = settings::WriteConfigJson(content, strlen(content));
		if (rc == 0) {
			rc = ApplyConfigFromStore(runtime);
		}
	} else if (strcmp(user_path, settings::GetSshConfigRelativePath()) == 0) {
		rc = settings::WriteSshConfigJson(content, strlen(content));
	} else {
		rc = storage::WriteTextFile(user_path, content, strlen(content));
	}

	if (rc != 0) {
		EmitLinef(io, "write failed: %d", rc);
		return rc;
	}

	EmitLinef(io, "Wrote %u bytes to %s", static_cast<unsigned int>(strlen(content)), user_path);
	return 0;
}

int HandleCd(const Runtime &, State *state, const char *path, const Io &io)
{
	char resolved[128];
	int rc = ResolvePath(*state, path != nullptr ? path : "/root", resolved, sizeof(resolved));
	if (rc != 0) {
		EmitLine(io, "cd: invalid path");
		return rc;
	}

	char fs_path[128];
	rc = storage::ResolveManagedPath(resolved, fs_path, sizeof(fs_path));
	if (rc != 0) {
		EmitLinef(io, "cd: %s: Invalid path", path != nullptr ? path : "/root");
		return rc;
	}

	struct fs_dir_t dir;
	fs_dir_t_init(&dir);
	rc = fs_opendir(&dir, fs_path);
	if (rc != 0) {
		EmitLinef(io, "cd: %s: Not a directory", path != nullptr ? path : "/root");
		return rc;
	}
	(void)fs_closedir(&dir);

	(void)snprintf(state->cwd, state->cwd_len, "%s", resolved);
	return 0;
}

int HandlePwd(const State &state, const Io &io)
{
	EmitLine(io, state.cwd);
	return 0;
}

int HandleFanctl(const Runtime &runtime, State &, char *argv[], int argc, const Io &io,
		 CommandResult *result)
{
	if (argc < 2) {
		EmitLine(io, "fanctl: missing subcommand");
		return -EINVAL;
	}

	if (strcmp(argv[1], "status") == 0) {
		PrintStatus(runtime, io);
		return 0;
	}

	if (strcmp(argv[1], "set") == 0) {
		if (argc < 5) {
			EmitLine(io, "usage: fanctl set <1|2> <0-100> <on|off>");
			return -EINVAL;
		}

		long fan_id = strtol(argv[2], nullptr, 10);
		long percent = strtol(argv[3], nullptr, 10);
		bool enabled = (strcmp(argv[4], "on") == 0 || strcmp(argv[4], "1") == 0);

		if (fan_id < 1 || fan_id > static_cast<long>(kFanCount) || percent < 0 || percent > 100) {
			EmitLine(io, "usage: fanctl set <1|2> <0-100> <on|off>");
			return -EINVAL;
		}

		FanState state = {};
		runtime.fan_controller->GetState(static_cast<size_t>(fan_id - 1), &state);
		int rc = runtime.fan_controller->ConfigureFan(static_cast<size_t>(fan_id - 1),
							      static_cast<uint8_t>(percent), enabled,
							      state.use_adc_target, false);
		if (rc != 0) {
			EmitLinef(io, "fanctl: set failed (%d)", rc);
			return rc;
		}

		PrintStatus(runtime, io);
		return 0;
	}

	if (strcmp(argv[1], "wifi") == 0) {
		if (argc < 3) {
			EmitLine(io, "usage: fanctl wifi <ssid> [psk]");
			return -EINVAL;
		}
		int rc = runtime.wifi_manager->SaveAndConnect(argv[2], argc > 3 ? argv[3] : "");
		if (rc != 0) {
			EmitLinef(io, "fanctl: wifi failed (%d)", rc);
			return rc;
		}
		EmitLinef(io, "saved Wi-Fi credentials for %s", argv[2]);
		return 0;
	}

	if (strcmp(argv[1], "ap") == 0) {
		if (argc < 3) {
			EmitLine(io, "usage: fanctl ap <on|off>");
			return -EINVAL;
		}

		int rc = strcmp(argv[2], "on") == 0 ? runtime.wifi_manager->EnableAp()
						    : strcmp(argv[2], "off") == 0 ? runtime.wifi_manager->DisableAp()
										  : -EINVAL;
		if (rc != 0) {
			EmitLinef(io, "fanctl: ap failed (%d)", rc);
			return rc;
		}

		PrintStatus(runtime, io);
		return 0;
	}

	if (strcmp(argv[1], "config") == 0) {
		char json[1024];
		size_t out_len = 0U;
		int rc = settings::ReadConfigJson(json, sizeof(json), &out_len);
		if (rc != 0) {
			EmitLinef(io, "fanctl: config read failed (%d)", rc);
			return rc;
		}
		EmitLinef(io, "Path: %s", settings::GetConfigRelativePath());
		io.write(io.ctx, json, out_len);
		Emit(io, "\n");
		return 0;
	}

	if (strcmp(argv[1], "clearwifi") == 0) {
		int rc = runtime.wifi_manager->ClearCredentials();
		if (rc != 0) {
			EmitLinef(io, "fanctl: clearwifi failed (%d)", rc);
			return rc;
		}
		EmitLine(io, "stored Wi-Fi credentials cleared");
		return 0;
	}

	if (strcmp(argv[1], "factoryreset") == 0) {
		if (argc < 3 || strcmp(argv[2], "confirm") != 0) {
			EmitLine(io, "warning: this will erase controller settings and reboot");
			EmitLine(io, "usage: fanctl factoryreset confirm");
			return -EINVAL;
		}

		int rc = settings::FactoryReset();
		if (rc != 0) {
			EmitLinef(io, "fanctl: factoryreset failed (%d)", rc);
			return rc;
		}

		EmitLine(io, "factory reset complete, rebooting...");
		if (result != nullptr) {
			result->reboot_requested = true;
			result->exit_requested = true;
		}
		return 0;
	}

	if (strcmp(argv[1], "reboot") == 0) {
		EmitLine(io, "rebooting...");
		if (result != nullptr) {
			result->reboot_requested = true;
			result->exit_requested = true;
		}
		return 0;
	}

	EmitLine(io, "fanctl: unknown subcommand");
	return -EINVAL;
}

int HandleShow(const Runtime &runtime, State &, char *argv[], int argc, const Io &io)
{
	if (argc < 2) {
		EmitLine(io, "show: missing section");
		return -EINVAL;
	}

	if (strcmp(argv[1], "fans") == 0) {
		int fan_id = 0;
		if (argc > 2) {
			fan_id = static_cast<int>(strtol(argv[2], nullptr, 10));
		}
		int rc = show_status::WriteFans(io.line, io.ctx, *runtime.fan_controller, fan_id);
		if (rc != 0) {
			EmitLine(io, "usage: show fans [1|2]");
		}
		return rc;
	}

	if (strcmp(argv[1], "curves") == 0) {
		show_status::WriteCurves(io.line, io.ctx);
		return 0;
	}
	if (strcmp(argv[1], "system") == 0) {
		show_status::WriteSystem(io.line, io.ctx);
		return 0;
	}
	if (strcmp(argv[1], "wifi") == 0) {
		show_status::WriteWifi(io.line, io.ctx, *runtime.wifi_manager);
		return 0;
	}
	if (strcmp(argv[1], "storage") == 0) {
		show_status::WriteStorage(io.line, io.ctx);
		return 0;
	}
	if (strcmp(argv[1], "host") == 0) {
		show_status::WriteHost(io.line, io.ctx, *runtime.host_control);
		return 0;
	}
	if (strcmp(argv[1], "ntp") == 0) {
		show_status::WriteNtp(io.line, io.ctx);
		return 0;
	}
	if (strcmp(argv[1], "ssh") == 0) {
		show_status::WriteSsh(io.line, io.ctx);
		return 0;
	}
	if (strcmp(argv[1], "all") == 0) {
		show_status::WriteAll(io.line, io.ctx, *runtime.fan_controller, *runtime.wifi_manager,
				      *runtime.host_control);
		return 0;
	}

	EmitLine(io, "show: unknown section");
	return -EINVAL;
}

int HandleEdit(const Runtime &runtime, State &state, char *argv[], int argc, const Io &io)
{
	if (argc < 2) {
		EmitLine(io, "edit: missing subcommand");
		return -EINVAL;
	}

	if (strcmp(argv[1], "open") == 0) {
		if (argc < 3) {
			EmitLine(io, "usage: edit open <path>");
			return -EINVAL;
		}
		char resolved[128];
		int rc = ResolvePath(state, argv[2], resolved, sizeof(resolved));
		if (rc != 0) {
			EmitLinef(io, "edit: invalid path (%d)", rc);
			return rc;
		}
		rc = state.editor->Open(resolved);
		if (rc != 0) {
			EmitLinef(io, "edit: open failed (%d)", rc);
			return rc;
		}
		EmitLinef(io, "opened %s (%u lines)", state.editor->GetPath(),
			  static_cast<unsigned int>(state.editor->GetLineCount()));
		return 0;
	}

	if (strcmp(argv[1], "status") == 0) {
		if (!state.editor->IsOpen()) {
			EmitLine(io, "editor: closed");
			return 0;
		}
		EmitLinef(io, "editor: open path=%s lines=%u dirty=%s", state.editor->GetPath(),
			  static_cast<unsigned int>(state.editor->GetLineCount()),
			  state.editor->IsDirty() ? "true" : "false");
		return 0;
	}

	if (!state.editor->IsOpen()) {
		EmitLine(io, "edit: editor is not open");
		return -EINVAL;
	}

	if (strcmp(argv[1], "show") == 0) {
		size_t start = (argc > 2) ? static_cast<size_t>(strtoul(argv[2], nullptr, 10)) : 1U;
		size_t end = (argc > 3) ? static_cast<size_t>(strtoul(argv[3], nullptr, 10)) : 0U;
		char output[1536];
		int rc = state.editor->PrintLines(output, sizeof(output), start, end);
		if (rc != 0) {
			EmitLinef(io, "edit: show failed (%d)", rc);
			return rc;
		}
		Emit(io, output);
		return 0;
	}

	if (strcmp(argv[1], "set") == 0 || strcmp(argv[1], "ins") == 0) {
		if (argc < 4) {
			EmitLinef(io, "usage: edit %s <line> <text>", argv[1]);
			return -EINVAL;
		}

		char text[256];
		int rc = JoinTokens(text, sizeof(text), argv, argc, 3);
		if (rc != 0) {
			EmitLine(io, "edit: line too long");
			return rc;
		}

		size_t line_no = static_cast<size_t>(strtoul(argv[2], nullptr, 10));
		rc = strcmp(argv[1], "set") == 0 ? state.editor->ReplaceLine(line_no, text)
						 : state.editor->InsertLine(line_no, text);
		if (rc != 0) {
			EmitLinef(io, "edit: %s failed (%d)", argv[1], rc);
			return rc;
		}
		EmitLine(io, "ok");
		return 0;
	}

	if (strcmp(argv[1], "app") == 0) {
		if (argc < 3) {
			EmitLine(io, "usage: edit app <text>");
			return -EINVAL;
		}

		char text[256];
		int rc = JoinTokens(text, sizeof(text), argv, argc, 2);
		if (rc != 0) {
			EmitLine(io, "edit: line too long");
			return rc;
		}
		rc = state.editor->AppendLine(text);
		if (rc != 0) {
			EmitLinef(io, "edit: append failed (%d)", rc);
			return rc;
		}
		EmitLine(io, "ok");
		return 0;
	}

	if (strcmp(argv[1], "del") == 0) {
		if (argc < 3) {
			EmitLine(io, "usage: edit del <line>");
			return -EINVAL;
		}
		int rc = state.editor->DeleteLine(static_cast<size_t>(strtoul(argv[2], nullptr, 10)));
		if (rc != 0) {
			EmitLinef(io, "edit: delete failed (%d)", rc);
			return rc;
		}
		EmitLine(io, "ok");
		return 0;
	}

	if (strcmp(argv[1], "write") == 0 || strcmp(argv[1], "saveas") == 0) {
		const char *target = state.editor->GetPath();
		char resolved[128];
		if (strcmp(argv[1], "saveas") == 0) {
			if (argc < 3) {
				EmitLine(io, "usage: edit saveas <path>");
				return -EINVAL;
			}
			int rc = ResolvePath(state, argv[2], resolved, sizeof(resolved));
			if (rc != 0) {
				EmitLinef(io, "edit: invalid path (%d)", rc);
				return rc;
			}
			target = resolved;
		}

		int rc = SaveEditorToTarget(runtime, *state.editor, target, io);
		if (rc != 0) {
			return rc;
		}

		if (strcmp(argv[1], "saveas") == 0) {
			rc = state.editor->Open(target);
			if (rc != 0) {
				EmitLinef(io, "edit: reopen failed (%d)", rc);
				return rc;
			}
		}

		EmitLinef(io, "saved %s", target);
		return 0;
	}

	if (strcmp(argv[1], "quit") == 0) {
		bool dirty = state.editor->IsDirty();
		state.editor->Close();
		EmitLinef(io, "editor closed%s", dirty ? " (unsaved changes discarded)" : "");
		return 0;
	}

	EmitLine(io, "edit: unknown subcommand");
	return -EINVAL;
}

int HandleTop(const Runtime &runtime, char *argv[], int argc, const Io &io)
{
	long samples = 20;
	long interval_ms = 1000;

	if (argc > 1) {
		samples = strtol(argv[1], nullptr, 10);
		if (samples < 0) {
			EmitLine(io, "usage: top [samples>=0] [interval_ms>=100]");
			return -EINVAL;
		}
	}

	if (argc > 2) {
		interval_ms = strtol(argv[2], nullptr, 10);
		if (interval_ms < 100) {
			EmitLine(io, "usage: top [samples>=0] [interval_ms>=100]");
			return -EINVAL;
		}
	}

	for (long i = 0; samples == 0 || i < samples; ++i) {
		Emit(io, "\033[2J\033[H");
		show_status::WriteMonitor(io.line, io.ctx, *runtime.fan_controller, *runtime.wifi_manager,
					  *runtime.host_control);

		if (samples == 0) {
			EmitLinef(io, "sample: %ld  interval: %ld ms  mode: continuous", i + 1,
				  interval_ms);
		} else {
			EmitLinef(io, "sample: %ld/%ld  interval: %ld ms", i + 1, samples,
				  interval_ms);
		}

		if (samples != 0 && i + 1 >= samples) {
			break;
		}

		k_sleep(K_MSEC(interval_ms));
	}

	return 0;
}

int HandleStorageSummary(const Io &io)
{
	show_status::WriteStorage(io.line, io.ctx);
	return 0;
}

} // namespace fanctl::cli
