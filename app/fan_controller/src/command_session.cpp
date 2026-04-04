/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "command_session.hpp"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/sys/util.h>

#include "common.hpp"
#include "settings_store.hpp"
#include "storage.hpp"

namespace fanctl {

namespace {

constexpr size_t kMaxArgs = 24;

} // namespace

CommandSession::CommandSession(FanController &fan_controller, WifiManager &wifi_manager,
			       HostControlManager &host_control)
	: fan_controller_(fan_controller), wifi_manager_(wifi_manager), host_control_(host_control)
{
	Reset();
}

void CommandSession::Reset()
{
	editor_.Close();
	(void)snprintf(cwd_, sizeof(cwd_), "/root");
}

void CommandSession::BuildPrompt(char *buffer, size_t buffer_len) const
{
	if (buffer == nullptr || buffer_len == 0U) {
		return;
	}

	(void)snprintf(buffer, buffer_len, "root@%s:%s# ", kDeviceHostname, cwd_);
}

void CommandSession::Emit(SessionWriteFn writer, void *ctx, const char *text) const
{
	if (writer == nullptr || text == nullptr) {
		return;
	}

	writer(ctx, text, strlen(text));
}

void CommandSession::Emitf(SessionWriteFn writer, void *ctx, const char *fmt, ...) const
{
	if (writer == nullptr || fmt == nullptr) {
		return;
	}

	char buffer[512];
	va_list args;

	va_start(args, fmt);
	int written = vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	if (written <= 0) {
		return;
	}

	writer(ctx, buffer, static_cast<size_t>(MIN(written, static_cast<int>(sizeof(buffer) - 1))));
}

int CommandSession::ResolvePath(const char *input, char *output, size_t output_len) const
{
	if (output == nullptr || output_len == 0U) {
		return -EINVAL;
	}

	const char *source = input;
	if (source == nullptr || source[0] == '\0') {
		source = cwd_;
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
	} else if (strcmp(cwd_, "/") == 0) {
		(void)snprintf(combined, sizeof(combined), "/%s", source);
	} else {
		(void)snprintf(combined, sizeof(combined), "%s/%s", cwd_, source);
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

int CommandSession::Tokenize(char *line, char *argv[], size_t argv_len) const
{
	if (line == nullptr || argv == nullptr || argv_len == 0U) {
		return -EINVAL;
	}

	size_t argc = 0U;
	char *cursor = line;

	while (*cursor != '\0') {
		while (*cursor != '\0' && isspace(static_cast<unsigned char>(*cursor)) != 0) {
			++cursor;
		}
		if (*cursor == '\0') {
			break;
		}

		if (argc >= argv_len) {
			return -ENOSPC;
		}

		char quote = '\0';
		if (*cursor == '"' || *cursor == '\'') {
			quote = *cursor++;
		}

		argv[argc++] = cursor;
		while (*cursor != '\0') {
			if (quote != '\0') {
				if (*cursor == quote) {
					*cursor++ = '\0';
					break;
				}
				++cursor;
				continue;
			}

			if (isspace(static_cast<unsigned char>(*cursor)) != 0) {
				*cursor++ = '\0';
				break;
			}
			++cursor;
		}
	}

	return static_cast<int>(argc);
}

int CommandSession::JoinTokens(char *buffer, size_t buffer_len, char *argv[], int argc, int start) const
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

int CommandSession::ApplyConfigFromStore() const
{
	settings::AppConfig config = {};
	int rc = settings::LoadConfig(&config);

	if (rc != 0) {
		return rc;
	}

	host_control_.Configure(config);

	return (config.wifi_ssid[0] == '\0') ? wifi_manager_.ClearCredentials()
					     : wifi_manager_.SaveAndConnect(config.wifi_ssid,
									    config.wifi_psk);
}

int CommandSession::HandleLs(const char *path, SessionWriteFn writer, void *ctx) const
{
	char resolved[128];
	int rc = ResolvePath(path, resolved, sizeof(resolved));
	if (rc != 0) {
		Emitf(writer, ctx, "ls: invalid path (%d)\r\n", rc);
		return rc;
	}

	char fs_path[160];
	rc = storage::ResolveManagedPath(resolved, fs_path, sizeof(fs_path));
	if (rc != 0) {
		Emitf(writer, ctx, "ls: resolve failed (%d)\r\n", rc);
		return rc;
	}

	struct fs_dir_t dir;
	fs_dir_t_init(&dir);
	rc = fs_opendir(&dir, fs_path);
	if (rc != 0) {
		Emitf(writer, ctx, "ls: opendir failed (%d)\r\n", rc);
		return rc;
	}

	while (true) {
		struct fs_dirent entry = {};
		rc = fs_readdir(&dir, &entry);
		if (rc != 0) {
			break;
		}
		if (entry.name[0] == '\0') {
			rc = 0;
			break;
		}
		Emitf(writer, ctx, "%c %8u %s\r\n", entry.type == FS_DIR_ENTRY_DIR ? 'd' : '-',
		      static_cast<unsigned int>(entry.size), entry.name);
	}

	(void)fs_closedir(&dir);
	if (rc != 0) {
		Emitf(writer, ctx, "ls: readdir failed (%d)\r\n", rc);
	}
	return rc;
}

int CommandSession::HandleCat(const char *path, SessionWriteFn writer, void *ctx) const
{
	char resolved[128];
	int rc = ResolvePath(path, resolved, sizeof(resolved));
	if (rc != 0) {
		Emitf(writer, ctx, "cat: invalid path (%d)\r\n", rc);
		return rc;
	}

	char fs_path[160];
	rc = storage::ResolveManagedPath(resolved, fs_path, sizeof(fs_path));
	if (rc != 0) {
		Emitf(writer, ctx, "cat: resolve failed (%d)\r\n", rc);
		return rc;
	}

	struct fs_file_t file;
	char chunk[160];

	fs_file_t_init(&file);
	rc = fs_open(&file, fs_path, FS_O_READ);
	if (rc != 0) {
		Emitf(writer, ctx, "cat: open failed (%d)\r\n", rc);
		return rc;
	}

	while (true) {
		ssize_t read_len = fs_read(&file, chunk, sizeof(chunk));
		if (read_len < 0) {
			rc = static_cast<int>(read_len);
			break;
		}
		if (read_len == 0) {
			rc = 0;
			break;
		}
		writer(ctx, chunk, static_cast<size_t>(read_len));
	}

	(void)fs_close(&file);
	Emit(writer, ctx, "\r\n");
	if (rc != 0) {
		Emitf(writer, ctx, "cat: read failed (%d)\r\n", rc);
	}
	return rc;
}

int CommandSession::HandleMkdir(const char *path, SessionWriteFn writer, void *ctx) const
{
	char resolved[128];
	int rc = ResolvePath(path, resolved, sizeof(resolved));
	if (rc != 0) {
		Emitf(writer, ctx, "mkdir: invalid path (%d)\r\n", rc);
		return rc;
	}

	rc = storage::MakeDirectory(resolved);
	if (rc != 0) {
		Emitf(writer, ctx, "mkdir: failed (%d)\r\n", rc);
		return rc;
	}

	Emitf(writer, ctx, "created %s\r\n", resolved);
	return 0;
}

int CommandSession::HandleRm(const char *path, SessionWriteFn writer, void *ctx) const
{
	char resolved[128];
	int rc = ResolvePath(path, resolved, sizeof(resolved));
	if (rc != 0) {
		Emitf(writer, ctx, "rm: invalid path (%d)\r\n", rc);
		return rc;
	}

	rc = storage::DeletePath(resolved);
	if (rc != 0) {
		Emitf(writer, ctx, "rm: failed (%d)\r\n", rc);
		return rc;
	}

	Emitf(writer, ctx, "deleted %s\r\n", resolved);
	return 0;
}

int CommandSession::HandleTouch(const char *path, SessionWriteFn writer, void *ctx) const
{
	char resolved[128];
	int rc = ResolvePath(path, resolved, sizeof(resolved));
	if (rc != 0) {
		Emitf(writer, ctx, "touch: invalid path (%d)\r\n", rc);
		return rc;
	}

	if (storage::PathExists(resolved)) {
		Emitf(writer, ctx, "updated %s\r\n", resolved);
		return 0;
	}

	rc = storage::WriteTextFile(resolved, "", 0U);
	if (rc != 0) {
		Emitf(writer, ctx, "touch: failed (%d)\r\n", rc);
		return rc;
	}

	Emitf(writer, ctx, "created %s\r\n", resolved);
	return 0;
}

int CommandSession::HandleWriteFile(char *argv[], int argc, SessionWriteFn writer, void *ctx)
{
	char resolved[128];
	int rc = ResolvePath(argv[1], resolved, sizeof(resolved));
	if (rc != 0) {
		Emitf(writer, ctx, "writefile: invalid path (%d)\r\n", rc);
		return rc;
	}

	char content[768];
	rc = JoinTokens(content, sizeof(content), argv, argc, 2);
	if (rc != 0) {
		Emit(writer, ctx, "writefile: content too long\r\n");
		return rc;
	}

	if (strcmp(resolved, settings::GetConfigRelativePath()) == 0) {
		rc = settings::WriteConfigJson(content, strlen(content));
		if (rc == 0) {
			rc = ApplyConfigFromStore();
		}
	} else if (strcmp(resolved, settings::GetSshConfigRelativePath()) == 0) {
		rc = settings::WriteSshConfigJson(content, strlen(content));
	} else {
		rc = storage::WriteTextFile(resolved, content, strlen(content));
	}

	if (rc != 0) {
		Emitf(writer, ctx, "writefile: failed (%d)\r\n", rc);
		return rc;
	}

	Emitf(writer, ctx, "wrote %u bytes to %s\r\n", static_cast<unsigned int>(strlen(content)),
	      resolved);
	return 0;
}

int CommandSession::HandleFanctl(char *argv[], int argc, SessionWriteFn writer, void *ctx)
{
	if (argc < 2) {
		Emit(writer, ctx, "fanctl: missing subcommand\r\n");
		return -EINVAL;
	}

	if (strcmp(argv[1], "status") == 0) {
		FanState fans[kFanCount];
		WifiSnapshot wifi = {};
		fan_controller_.GetAllStates(fans);
		wifi_manager_.GetSnapshot(&wifi);

		Emitf(writer, ctx, "AP: %s  SSID: %s  IP: %s  Clients: %d\r\n",
		      wifi.ap_enabled ? "on" : "off", wifi.ap_ssid, kApIpAddr, wifi.ap_clients);
		Emitf(writer, ctx, "STA: %s  Saved SSID: %s  RSSI: %d\r\n",
		      wifi.sta_connected ? "connected" : wifi.sta_state,
		      wifi.saved_ssid[0] != '\0' ? wifi.saved_ssid : "-", wifi.sta_rssi);
		for (size_t i = 0; i < kFanCount; ++i) {
			Emitf(writer, ctx,
			      "Fan %u: enabled=%s adc_mode=%s manual=%u%% effective=%u%% pwm=%u%% rpm=%d adc_raw=%d voltage=%d mV\r\n",
			      static_cast<unsigned int>(i + 1), fans[i].enabled ? "true" : "false",
			      fans[i].use_adc_target ? "true" : "false", fans[i].percent,
			      fans[i].effective_percent, fans[i].pwm_percent, fans[i].actual_rpm,
			      fans[i].adc_raw, fans[i].mapped_voltage_mv);
		}
		return 0;
	}

	if (strcmp(argv[1], "set") == 0) {
		if (argc < 5) {
			Emit(writer, ctx, "usage: fanctl set <1|2> <0-100> <on|off>\r\n");
			return -EINVAL;
		}

		long fan_id = strtol(argv[2], nullptr, 10);
		long percent = strtol(argv[3], nullptr, 10);
		bool enabled = (strcmp(argv[4], "on") == 0 || strcmp(argv[4], "1") == 0);
		if (fan_id < 1 || fan_id > static_cast<long>(kFanCount) || percent < 0 || percent > 100) {
			Emit(writer, ctx, "usage: fanctl set <1|2> <0-100> <on|off>\r\n");
			return -EINVAL;
		}

		FanState state = {};
		fan_controller_.GetState(static_cast<size_t>(fan_id - 1), &state);
		int rc = fan_controller_.ConfigureFan(static_cast<size_t>(fan_id - 1),
						      static_cast<uint8_t>(percent), enabled,
						      state.use_adc_target, false);
		if (rc != 0) {
			Emitf(writer, ctx, "fanctl: set failed (%d)\r\n", rc);
			return rc;
		}
		return HandleFanctl(argv, 2, writer, ctx);
	}

	if (strcmp(argv[1], "wifi") == 0) {
		if (argc < 3) {
			Emit(writer, ctx, "usage: fanctl wifi <ssid> [psk]\r\n");
			return -EINVAL;
		}
		int rc = wifi_manager_.SaveAndConnect(argv[2], argc > 3 ? argv[3] : "");
		if (rc != 0) {
			Emitf(writer, ctx, "fanctl: wifi failed (%d)\r\n", rc);
			return rc;
		}
		Emitf(writer, ctx, "saved Wi-Fi credentials for %s\r\n", argv[2]);
		return 0;
	}

	if (strcmp(argv[1], "ap") == 0) {
		if (argc < 3) {
			Emit(writer, ctx, "usage: fanctl ap <on|off>\r\n");
			return -EINVAL;
		}

		int rc = strcmp(argv[2], "on") == 0 ? wifi_manager_.EnableAp()
						    : strcmp(argv[2], "off") == 0 ? wifi_manager_.DisableAp()
										  : -EINVAL;
		if (rc != 0) {
			Emitf(writer, ctx, "fanctl: ap failed (%d)\r\n", rc);
			return rc;
		}
		return HandleFanctl(argv, 2, writer, ctx);
	}

	if (strcmp(argv[1], "config") == 0) {
		char json[1024];
		size_t out_len = 0U;
		int rc = settings::ReadConfigJson(json, sizeof(json), &out_len);
		if (rc != 0) {
			Emitf(writer, ctx, "fanctl: config read failed (%d)\r\n", rc);
			return rc;
		}
		Emitf(writer, ctx, "Path: %s\r\n", settings::GetConfigRelativePath());
		writer(ctx, json, out_len);
		Emit(writer, ctx, "\r\n");
		return 0;
	}

	if (strcmp(argv[1], "clearwifi") == 0) {
		int rc = wifi_manager_.ClearCredentials();
		if (rc != 0) {
			Emitf(writer, ctx, "fanctl: clearwifi failed (%d)\r\n", rc);
			return rc;
		}
		Emit(writer, ctx, "stored Wi-Fi credentials cleared\r\n");
		return 0;
	}

	Emit(writer, ctx, "fanctl: unknown subcommand\r\n");
	return -EINVAL;
}

int CommandSession::HandleShow(char *argv[], int argc, SessionWriteFn writer, void *ctx) const
{
	if (argc < 2) {
		Emit(writer, ctx, "show: missing section\r\n");
		return -EINVAL;
	}

	if (strcmp(argv[1], "fans") == 0) {
		FanState fans[kFanCount];
		fan_controller_.GetAllStates(fans);

		size_t start = 0U;
		size_t end = kFanCount;
		if (argc > 2) {
			long fan_id = strtol(argv[2], nullptr, 10);
			if (fan_id < 1 || fan_id > static_cast<long>(kFanCount)) {
				Emit(writer, ctx, "usage: show fans [1|2]\r\n");
				return -EINVAL;
			}
			start = static_cast<size_t>(fan_id - 1);
			end = start + 1U;
		}

		for (size_t i = start; i < end; ++i) {
			Emitf(writer, ctx, "Fan %u\r\n", static_cast<unsigned int>(i + 1U));
			Emitf(writer, ctx, "  enabled            : %s\r\n", fans[i].enabled ? "true" : "false");
			Emitf(writer, ctx, "  adc mode           : %s\r\n",
			      fans[i].use_adc_target ? "true" : "false");
			Emitf(writer, ctx, "  manual percent     : %u%%\r\n", fans[i].percent);
			Emitf(writer, ctx, "  effective percent  : %u%%\r\n", fans[i].effective_percent);
			Emitf(writer, ctx, "  pwm output percent : %u%%\r\n", fans[i].pwm_percent);
			Emitf(writer, ctx, "  pwm pulse          : %u ns\r\n",
			      static_cast<unsigned int>(fans[i].pwm_pulse_ns));
			Emitf(writer, ctx, "  adc raw            : %d\r\n", fans[i].adc_raw);
			Emitf(writer, ctx, "  adc sample         : %d mV\r\n", fans[i].adc_mv);
			Emitf(writer, ctx, "  mapped voltage     : %d mV\r\n", fans[i].mapped_voltage_mv);
			Emitf(writer, ctx, "  adc target percent : %u%%\r\n", fans[i].adc_target_percent);
			Emitf(writer, ctx, "  target rpm curve   : %d rpm\r\n", fans[i].target_rpm);
			Emitf(writer, ctx, "  actual rpm         : %d rpm\r\n", fans[i].actual_rpm);
			Emitf(writer, ctx, "  actual percent est : %u%%\r\n", fans[i].actual_percent);
		}
		return 0;
	}

	if (strcmp(argv[1], "curves") == 0) {
		Emitf(writer, ctx, "adc raw -> voltage   : %s\r\n",
		      curves::CurveProfiles::GetAdcToVoltagePath());
		Emitf(writer, ctx, "voltage -> percent   : %s\r\n",
		      curves::CurveProfiles::GetVoltageToPercentPath());
		Emitf(writer, ctx, "speed percent -> pwm : %s\r\n",
		      curves::CurveProfiles::GetPercentToPwmPath());
		Emitf(writer, ctx, "speed percent -> rpm : %s\r\n",
		      curves::CurveProfiles::GetPercentToRpmPath());
		return 0;
	}

	Emit(writer, ctx, "show: unknown section\r\n");
	return -EINVAL;
}

int CommandSession::HandleEdit(char *argv[], int argc, SessionWriteFn writer, void *ctx)
{
	if (argc < 2) {
		Emit(writer, ctx, "edit: missing subcommand\r\n");
		return -EINVAL;
	}

	if (strcmp(argv[1], "open") == 0) {
		if (argc < 3) {
			Emit(writer, ctx, "usage: edit open <path>\r\n");
			return -EINVAL;
		}

		char resolved[128];
		int rc = ResolvePath(argv[2], resolved, sizeof(resolved));
		if (rc != 0) {
			Emitf(writer, ctx, "edit: invalid path (%d)\r\n", rc);
			return rc;
		}

		rc = editor_.Open(resolved);
		if (rc != 0) {
			Emitf(writer, ctx, "edit: open failed (%d)\r\n", rc);
			return rc;
		}
		Emitf(writer, ctx, "opened %s (%u lines)\r\n", editor_.GetPath(),
		      static_cast<unsigned int>(editor_.GetLineCount()));
		return 0;
	}

	if (strcmp(argv[1], "status") == 0) {
		if (!editor_.IsOpen()) {
			Emit(writer, ctx, "editor: closed\r\n");
			return 0;
		}
		Emitf(writer, ctx, "editor: open path=%s lines=%u dirty=%s\r\n", editor_.GetPath(),
		      static_cast<unsigned int>(editor_.GetLineCount()),
		      editor_.IsDirty() ? "true" : "false");
		return 0;
	}

	if (!editor_.IsOpen()) {
		Emit(writer, ctx, "edit: editor is not open\r\n");
		return -EINVAL;
	}

	if (strcmp(argv[1], "show") == 0) {
		size_t start = (argc > 2) ? static_cast<size_t>(strtoul(argv[2], nullptr, 10)) : 1U;
		size_t end = (argc > 3) ? static_cast<size_t>(strtoul(argv[3], nullptr, 10)) : 0U;
		char output[1536];
		int rc = editor_.PrintLines(output, sizeof(output), start, end);
		if (rc != 0) {
			Emitf(writer, ctx, "edit: show failed (%d)\r\n", rc);
			return rc;
		}
		Emit(writer, ctx, output);
		return 0;
	}

	if (strcmp(argv[1], "set") == 0 || strcmp(argv[1], "ins") == 0) {
		if (argc < 4) {
			Emitf(writer, ctx, "usage: edit %s <line> <text>\r\n", argv[1]);
			return -EINVAL;
		}

		char text[256];
		int rc = JoinTokens(text, sizeof(text), argv, argc, 3);
		if (rc != 0) {
			Emit(writer, ctx, "edit: line too long\r\n");
			return rc;
		}

		size_t line_no = static_cast<size_t>(strtoul(argv[2], nullptr, 10));
		rc = strcmp(argv[1], "set") == 0 ? editor_.ReplaceLine(line_no, text)
						 : editor_.InsertLine(line_no, text);
		if (rc != 0) {
			Emitf(writer, ctx, "edit: %s failed (%d)\r\n", argv[1], rc);
			return rc;
		}
		Emit(writer, ctx, "ok\r\n");
		return 0;
	}

	if (strcmp(argv[1], "app") == 0) {
		if (argc < 3) {
			Emit(writer, ctx, "usage: edit app <text>\r\n");
			return -EINVAL;
		}
		char text[256];
		int rc = JoinTokens(text, sizeof(text), argv, argc, 2);
		if (rc != 0) {
			Emit(writer, ctx, "edit: line too long\r\n");
			return rc;
		}
		rc = editor_.AppendLine(text);
		if (rc != 0) {
			Emitf(writer, ctx, "edit: append failed (%d)\r\n", rc);
			return rc;
		}
		Emit(writer, ctx, "ok\r\n");
		return 0;
	}

	if (strcmp(argv[1], "del") == 0) {
		if (argc < 3) {
			Emit(writer, ctx, "usage: edit del <line>\r\n");
			return -EINVAL;
		}
		int rc = editor_.DeleteLine(static_cast<size_t>(strtoul(argv[2], nullptr, 10)));
		if (rc != 0) {
			Emitf(writer, ctx, "edit: delete failed (%d)\r\n", rc);
			return rc;
		}
		Emit(writer, ctx, "ok\r\n");
		return 0;
	}

	if (strcmp(argv[1], "write") == 0 || strcmp(argv[1], "saveas") == 0) {
		const char *target = editor_.GetPath();
		char resolved[128];
		if (strcmp(argv[1], "saveas") == 0) {
			if (argc < 3) {
				Emit(writer, ctx, "usage: edit saveas <path>\r\n");
				return -EINVAL;
			}
			int rc = ResolvePath(argv[2], resolved, sizeof(resolved));
			if (rc != 0) {
				Emitf(writer, ctx, "edit: invalid path (%d)\r\n", rc);
				return rc;
			}
			target = resolved;
		}

		int rc = 0;
		if (strcmp(target, settings::GetConfigRelativePath()) == 0) {
			rc = settings::WriteConfigJson(editor_.GetContent(), strlen(editor_.GetContent()));
			if (rc == 0) {
				rc = ApplyConfigFromStore();
			}
		} else if (strcmp(target, settings::GetSshConfigRelativePath()) == 0) {
			rc = settings::WriteSshConfigJson(editor_.GetContent(), strlen(editor_.GetContent()));
		} else {
			rc = storage::WriteTextFile(target, editor_.GetContent(), strlen(editor_.GetContent()));
		}
		if (rc != 0) {
			Emitf(writer, ctx, "edit: save failed (%d)\r\n", rc);
			return rc;
		}

		if (strcmp(argv[1], "saveas") == 0) {
			rc = editor_.Open(target);
			if (rc != 0) {
				Emitf(writer, ctx, "edit: reopen failed (%d)\r\n", rc);
				return rc;
			}
		}

		Emitf(writer, ctx, "saved %s\r\n", target);
		return 0;
	}

	if (strcmp(argv[1], "quit") == 0) {
		bool dirty = editor_.IsDirty();
		editor_.Close();
		Emitf(writer, ctx, "editor closed%s\r\n", dirty ? " (unsaved changes discarded)" : "");
		return 0;
	}

	Emit(writer, ctx, "edit: unknown subcommand\r\n");
	return -EINVAL;
}

void CommandSession::EmitHelp(SessionWriteFn writer, void *ctx) const
{
	Emit(writer, ctx,
	     "Commands:\r\n"
	     "  help\r\n"
	     "  pwd\r\n"
	     "  cd [path]\r\n"
	     "  ls [path]\r\n"
	     "  cat <path>\r\n"
	     "  touch <path>\r\n"
	     "  mkdir <path>\r\n"
	     "  rm <path>\r\n"
	     "  writefile <path> <text>\r\n"
	     "  fanctl status|set|wifi|ap|config|clearwifi\r\n"
	     "  show fans [1|2]|curves\r\n"
	     "  edit open|status|show|set|ins|app|del|write|saveas|quit\r\n"
	     "  whoami\r\n"
	     "  hostname\r\n"
	     "  uname\r\n"
	     "  clear\r\n"
	     "  exit\r\n"
	     "  reboot\r\n");
}

int CommandSession::Execute(const char *command_line, SessionWriteFn writer, void *ctx,
			    CommandSessionResult *result)
{
	if (result != nullptr) {
		result->exit_requested = false;
		result->reboot_requested = false;
	}

	if (command_line == nullptr) {
		return -EINVAL;
	}

	char line[512];
	(void)snprintf(line, sizeof(line), "%s", command_line);

	size_t line_len = strlen(line);
	while (line_len > 0U && (line[line_len - 1U] == '\r' || line[line_len - 1U] == '\n')) {
		line[--line_len] = '\0';
	}

	char *argv[kMaxArgs];
	int argc = Tokenize(line, argv, ARRAY_SIZE(argv));
	if (argc < 0) {
		Emit(writer, ctx, "parse error\r\n");
		return argc;
	}
	if (argc == 0) {
		return 0;
	}

	if (strcmp(argv[0], "help") == 0) {
		EmitHelp(writer, ctx);
		return 0;
	}

	if (strcmp(argv[0], "pwd") == 0) {
		Emitf(writer, ctx, "%s\r\n", cwd_);
		return 0;
	}

	if (strcmp(argv[0], "cd") == 0) {
		char resolved[128];
		int rc = ResolvePath(argc > 1 ? argv[1] : "/root", resolved, sizeof(resolved));
		if (rc != 0) {
			Emitf(writer, ctx, "cd: invalid path (%d)\r\n", rc);
			return rc;
		}

		char fs_path[160];
		rc = storage::ResolveManagedPath(resolved, fs_path, sizeof(fs_path));
		if (rc != 0) {
			Emitf(writer, ctx, "cd: resolve failed (%d)\r\n", rc);
			return rc;
		}

		struct fs_dir_t dir;
		fs_dir_t_init(&dir);
		rc = fs_opendir(&dir, fs_path);
		if (rc != 0) {
			Emitf(writer, ctx, "cd: not a directory (%d)\r\n", rc);
			return rc;
		}
		(void)fs_closedir(&dir);
		(void)snprintf(cwd_, sizeof(cwd_), "%s", resolved);
		return 0;
	}

	if (strcmp(argv[0], "ls") == 0) {
		return HandleLs(argc > 1 ? argv[1] : cwd_, writer, ctx);
	}

	if (strcmp(argv[0], "cat") == 0) {
		if (argc < 2) {
			Emit(writer, ctx, "usage: cat <path>\r\n");
			return -EINVAL;
		}
		return HandleCat(argv[1], writer, ctx);
	}

	if (strcmp(argv[0], "mkdir") == 0) {
		if (argc < 2) {
			Emit(writer, ctx, "usage: mkdir <path>\r\n");
			return -EINVAL;
		}
		return HandleMkdir(argv[1], writer, ctx);
	}

	if (strcmp(argv[0], "touch") == 0) {
		if (argc < 2) {
			Emit(writer, ctx, "usage: touch <path>\r\n");
			return -EINVAL;
		}
		return HandleTouch(argv[1], writer, ctx);
	}

	if (strcmp(argv[0], "rm") == 0) {
		if (argc < 2) {
			Emit(writer, ctx, "usage: rm <path>\r\n");
			return -EINVAL;
		}
		return HandleRm(argv[1], writer, ctx);
	}

	if (strcmp(argv[0], "writefile") == 0) {
		if (argc < 3) {
			Emit(writer, ctx, "usage: writefile <path> <text>\r\n");
			return -EINVAL;
		}
		return HandleWriteFile(argv, argc, writer, ctx);
	}

	if (strcmp(argv[0], "fanctl") == 0) {
		return HandleFanctl(argv, argc, writer, ctx);
	}

	if (strcmp(argv[0], "show") == 0) {
		return HandleShow(argv, argc, writer, ctx);
	}

	if (strcmp(argv[0], "edit") == 0) {
		return HandleEdit(argv, argc, writer, ctx);
	}

	if (strcmp(argv[0], "whoami") == 0) {
		Emit(writer, ctx, "root\r\n");
		return 0;
	}

	if (strcmp(argv[0], "hostname") == 0) {
		Emitf(writer, ctx, "%s\r\n", kDeviceHostname);
		return 0;
	}

	if (strcmp(argv[0], "uname") == 0) {
		Emit(writer, ctx, "Zephyr fanctl\r\n");
		return 0;
	}

	if (strcmp(argv[0], "echo") == 0) {
		if (argc > 1) {
			char buffer[384];
			int rc = JoinTokens(buffer, sizeof(buffer), argv, argc, 1);
			if (rc != 0) {
				Emit(writer, ctx, "echo: text too long\r\n");
				return rc;
			}
			Emitf(writer, ctx, "%s\r\n", buffer);
		} else {
			Emit(writer, ctx, "\r\n");
		}
		return 0;
	}

	if (strcmp(argv[0], "clear") == 0) {
		Emit(writer, ctx, "\033[2J\033[H");
		return 0;
	}

	if (strcmp(argv[0], "exit") == 0) {
		if (result != nullptr) {
			result->exit_requested = true;
		}
		return 0;
	}

	if (strcmp(argv[0], "reboot") == 0) {
		Emit(writer, ctx, "rebooting...\r\n");
		if (result != nullptr) {
			result->reboot_requested = true;
			result->exit_requested = true;
		}
		return 0;
	}

	Emitf(writer, ctx, "%s: command not found\r\n", argv[0]);
	return -ENOENT;
}

} // namespace fanctl
