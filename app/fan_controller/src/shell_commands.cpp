/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "shell_commands.hpp"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>

#include "curve_profiles.hpp"
#include "line_editor.hpp"
#include "settings_store.hpp"
#include "storage.hpp"
#include "wifi_manager.hpp"

namespace fanctl::shell_commands {

namespace {

FanController *g_fan_controller = nullptr;
WifiManager *g_wifi_manager = nullptr;
HostControlManager *g_host_control = nullptr;
LineEditor g_editor;
char g_cwd[128] = "/";

int JoinArgs(char *buffer, size_t buffer_len, size_t argc, char **argv, size_t start_index)
{
	size_t offset = 0U;

	if (buffer == nullptr || buffer_len == 0U || start_index >= argc) {
		return -EINVAL;
	}

	buffer[0] = '\0';
	for (size_t i = start_index; i < argc; ++i) {
		int written = snprintf(buffer + offset, buffer_len - offset, "%s%s",
				       (i == start_index) ? "" : " ", argv[i]);
		if (written <= 0 || static_cast<size_t>(written) >= buffer_len - offset) {
			return -ENOSPC;
		}
		offset += static_cast<size_t>(written);
	}

	return 0;
}

int ApplyConfigFromStore()
{
	settings::AppConfig config = {};
	int rc = settings::LoadConfig(&config);

	if (rc != 0) {
		return rc;
	}

	if (g_host_control != nullptr) {
		g_host_control->Configure(config);
	}

	// Load WiFi config separately
	settings::WifiConfig wifi_config = {};
	if (settings::LoadWifiConfig(&wifi_config) != 0) {
		settings::FillWifiDefaults(&wifi_config);
	}

	return (wifi_config.sta_ssid[0] == '\0')
		       ? g_wifi_manager->ClearCredentials()
		       : g_wifi_manager->SaveAndConnect(wifi_config.sta_ssid, wifi_config.sta_psk);
}

void PrintStatus(const struct shell *sh)
{
	FanState fans[kFanCount];
	WifiSnapshot wifi = {};

	g_fan_controller->GetAllStates(fans);
	g_wifi_manager->GetSnapshot(&wifi);

	shell_print(sh, "AP: %s  SSID: %s  IP: %s  Clients: %d",
		    wifi.ap_enabled ? "on" : "off", wifi.ap_ssid, kApIpAddr, wifi.ap_clients);
	shell_print(sh, "STA: %s  Saved SSID: %s  RSSI: %d",
		    wifi.sta_connected ? "connected" : wifi.sta_state,
		    wifi.saved_ssid[0] != '\0' ? wifi.saved_ssid : "-", wifi.sta_rssi);

	for (size_t i = 0; i < kFanCount; ++i) {
		shell_print(sh,
			    "Fan %u: enabled=%s adc_mode=%s manual=%u%% effective=%u%% pwm=%u%% rpm=%d adc_raw=%d voltage=%d mV",
			    static_cast<unsigned int>(i + 1), fans[i].enabled ? "true" : "false",
			    fans[i].use_adc_target ? "true" : "false", fans[i].percent,
			    fans[i].effective_percent, fans[i].pwm_percent, fans[i].actual_rpm,
			    fans[i].adc_raw, fans[i].mapped_voltage_mv);
	}
}

void PrintFanShow(const struct shell *sh, size_t index, const FanState &fan)
{
	shell_print(sh, "Fan %u", static_cast<unsigned int>(index + 1U));
	shell_print(sh, "  enabled            : %s", fan.enabled ? "true" : "false");
	shell_print(sh, "  adc mode           : %s", fan.use_adc_target ? "true" : "false");
	shell_print(sh, "  manual percent     : %u%%", fan.percent);
	shell_print(sh, "  effective percent  : %u%%", fan.effective_percent);
	shell_print(sh, "  pwm output percent : %u%%", fan.pwm_percent);
	shell_print(sh, "  pwm pulse          : %u ns", static_cast<unsigned int>(fan.pwm_pulse_ns));
	shell_print(sh, "  adc raw            : %d", fan.adc_raw);
	shell_print(sh, "  adc sample         : %d mV", fan.adc_mv);
	shell_print(sh, "  mapped voltage     : %d mV", fan.mapped_voltage_mv);
	shell_print(sh, "  adc target percent : %u%%", fan.adc_target_percent);
	shell_print(sh, "  target rpm curve   : %d rpm", fan.target_rpm);
	shell_print(sh, "  actual rpm         : %d rpm", fan.actual_rpm);
	shell_print(sh, "  actual percent est : %u%%", fan.actual_percent);
}

int CmdShowFans(const struct shell *sh, size_t argc, char **argv)
{
	FanState fans[kFanCount];
	g_fan_controller->GetAllStates(fans);

	if (argc > 1) {
		long fan_id = strtol(argv[1], nullptr, 10);
		if (fan_id < 1 || fan_id > static_cast<long>(kFanCount)) {
			shell_error(sh, "Usage: show fans [1|2]");
			return -EINVAL;
		}

		PrintFanShow(sh, static_cast<size_t>(fan_id - 1), fans[fan_id - 1]);
		return 0;
	}

	for (size_t i = 0; i < kFanCount; ++i) {
		PrintFanShow(sh, i, fans[i]);
	}

	return 0;
}

int CmdShowCurves(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Curve files");
	shell_print(sh, "  adc raw -> voltage   : %s", curves::CurveProfiles::GetAdcToVoltagePath());
	shell_print(sh, "  voltage -> percent   : %s", curves::CurveProfiles::GetVoltageToPercentPath());
	shell_print(sh, "  speed percent -> pwm : %s", curves::CurveProfiles::GetPercentToPwmPath());
	shell_print(sh, "  speed percent -> rpm : %s", curves::CurveProfiles::GetPercentToRpmPath());
	return 0;
}

int CmdStatus(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	PrintStatus(sh);
	return 0;
}

int CmdSet(const struct shell *sh, size_t argc, char **argv)
{
	long fan_id = strtol(argv[1], nullptr, 10);
	long percent = strtol(argv[2], nullptr, 10);
	bool enabled = (strcmp(argv[3], "on") == 0 || strcmp(argv[3], "1") == 0);

	if (fan_id < 1 || fan_id > static_cast<long>(kFanCount) || percent < 0 || percent > 100) {
		shell_error(sh, "Usage: fanctl set <1|2> <0-100> <on|off>");
		return -EINVAL;
	}

	FanState state = {};
	g_fan_controller->GetState(static_cast<size_t>(fan_id - 1), &state);
	int rc = g_fan_controller->ConfigureFan(static_cast<size_t>(fan_id - 1),
						static_cast<uint8_t>(percent), enabled,
						state.use_adc_target, false);
	if (rc != 0) {
		shell_error(sh, "fan set failed: %d", rc);
		return rc;
	}

	PrintStatus(sh);
	return 0;
}

int CmdWifi(const struct shell *sh, size_t argc, char **argv)
{
	const char *psk = (argc > 2) ? argv[2] : "";
	int rc = g_wifi_manager->SaveAndConnect(argv[1], psk);

	if (rc != 0) {
		shell_error(sh, "wifi connect failed: %d", rc);
		return rc;
	}

	shell_print(sh, "Saved Wi-Fi credentials for %s", argv[1]);
	return 0;
}

int CmdScan(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Starting Wi-Fi scan...");
	
	int rc = g_wifi_manager->StartScan();
	if (rc != 0) {
		shell_error(sh, "Scan failed: %d", rc);
		return rc;
	}
	
	// Wait for scan to complete (max 10 seconds)
	int timeout = 100;
	while (!g_wifi_manager->IsScanComplete() && timeout-- > 0) {
		k_sleep(K_MSEC(100));
	}
	
	if (!g_wifi_manager->IsScanComplete()) {
		shell_warn(sh, "Scan timeout");
	}
	
	// Get and display results - use static buffer to reduce stack usage
	static WifiScanResult scan_results[16];
	size_t count = 0;
	g_wifi_manager->GetScanResults(scan_results, ARRAY_SIZE(scan_results), &count);
	
	if (count == 0) {
		shell_print(sh, "No networks found");
		return 0;
	}
	
	shell_print(sh, "Found %u network(s):", (unsigned int)count);
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
			(unsigned int)(i + 1),
			scan_results[i].ssid,
			scan_results[i].rssi,
			scan_results[i].channel,
			security);
	}
	
	return 0;
}

int CmdWifiConnect(const struct shell *sh, size_t argc, char **argv)
{
	// Usage: wificonnect [network_number] [password]
	// If no args provided, show scan results
	
	if (argc == 1) {
		// No arguments - perform scan and show results
		shell_print(sh, "Wi-Fi Connection Utility");
		shell_print(sh, "========================");
		shell_print(sh, "");
		shell_print(sh, "Starting scan...");
		
		int rc = g_wifi_manager->StartScan();
		if (rc != 0) {
			shell_error(sh, "Scan failed: %d", rc);
			return rc;
		}
		
		// Wait for scan to complete
		int timeout = 100;
		while (!g_wifi_manager->IsScanComplete() && timeout-- > 0) {
			k_sleep(K_MSEC(100));
		}
		
		// Use static buffer to reduce stack usage
		static WifiScanResult connect_scan_results[16];
		size_t count = 0;
		g_wifi_manager->GetScanResults(connect_scan_results, ARRAY_SIZE(connect_scan_results), &count);
		
		if (count == 0) {
			shell_print(sh, "No networks found");
			return 0;
		}
		
		// Display numbered list
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
				(unsigned int)(i + 1),
				connect_scan_results[i].ssid,
				connect_scan_results[i].rssi,
				connect_scan_results[i].channel,
				security);
		}
		
		shell_print(sh, "");
		shell_print(sh, "Usage: wificonnect <number> [password]");
		shell_print(sh, "Example: wificonnect 1 mypassword");
		return 0;
	}
	
	// Parse network number
	long choice = strtol(argv[1], nullptr, 10);
	if (choice <= 0) {
		shell_error(sh, "Invalid network number");
		return -EINVAL;
	}
	
	// Get scan results - reuse static buffer
	static WifiScanResult connect_scan_results[16];
	size_t count = 0;
	g_wifi_manager->GetScanResults(connect_scan_results, ARRAY_SIZE(connect_scan_results), &count);
	
	if (choice > static_cast<long>(count)) {
		shell_error(sh, "Network number out of range, run 'wificonnect' first to scan");
		return -EINVAL;
	}
	
	const WifiScanResult &selected = connect_scan_results[choice - 1];
	
	// Get password
	const char *psk = "";
	if (argc > 2) {
		psk = argv[2];
	} else if (selected.security != WIFI_SECURITY_TYPE_NONE) {
		shell_error(sh, "Network requires password: wificonnect %s <password>", argv[1]);
		return -EINVAL;
	}
	
	shell_print(sh, "Connecting to %s...", selected.ssid);
	int rc = g_wifi_manager->SaveAndConnect(selected.ssid, psk);
	if (rc != 0) {
		shell_error(sh, "Connection failed: %d", rc);
		return rc;
	}
	
	shell_print(sh, "Connected successfully!");
	PrintStatus(sh);
	return 0;
}

int CmdAp(const struct shell *sh, size_t argc, char **argv)
{
	int rc = -EINVAL;

	if (strcmp(argv[1], "on") == 0) {
		rc = g_wifi_manager->EnableAp();
	} else if (strcmp(argv[1], "off") == 0) {
		rc = g_wifi_manager->DisableAp();
	}

	if (rc != 0) {
		shell_error(sh, "Usage: fanctl ap <on|off>");
		return rc;
	}

	PrintStatus(sh);
	return 0;
}

int CmdClearWifi(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int rc = g_wifi_manager->ClearCredentials();
	if (rc != 0) {
		shell_error(sh, "clear wifi failed: %d", rc);
		return rc;
	}

	shell_print(sh, "Stored Wi-Fi credentials cleared");
	return 0;
}

int CmdConfig(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	char json[1024];
	size_t out_len = 0U;
	int rc = settings::ReadConfigJson(json, sizeof(json), &out_len);

	if (rc != 0) {
		shell_error(sh, "config read failed: %d", rc);
		return rc;
	}

	shell_print(sh, "Path: %s", settings::GetConfigRelativePath());
	shell_fprintf(sh, SHELL_NORMAL, "%s\n", json);
	return 0;
}

int CmdLs(const struct shell *sh, size_t argc, char **argv)
{
	const char *user_path = (argc > 1) ? argv[1] : "/";
	char fs_path[128];
	int rc = storage::ResolveManagedPath(user_path, fs_path, sizeof(fs_path));

	if (rc != 0) {
		shell_error(sh, "invalid path: %d", rc);
		return rc;
	}

	struct fs_dir_t dir;
	fs_dir_t_init(&dir);
	rc = fs_opendir(&dir, fs_path);
	if (rc != 0) {
		shell_error(sh, "opendir failed: %d", rc);
		return rc;
	}

	shell_print(sh, "Listing %s", user_path);
	while (true) {
		struct fs_dirent entry = {};

		rc = fs_readdir(&dir, &entry);
		if (rc != 0) {
			shell_error(sh, "readdir failed: %d", rc);
			break;
		}

		if (entry.name[0] == '\0') {
			break;
		}

		shell_print(sh, "%c %8u %s", entry.type == FS_DIR_ENTRY_DIR ? 'd' : '-',
			    static_cast<unsigned int>(entry.size), entry.name);
	}

	(void)fs_closedir(&dir);
	return rc;
}

int CmdCat(const struct shell *sh, size_t argc, char **argv)
{
	char fs_path[128];
	int rc = storage::ResolveManagedPath(argv[1], fs_path, sizeof(fs_path));

	if (rc != 0) {
		shell_error(sh, "invalid path: %d", rc);
		return rc;
	}

	struct fs_file_t file;
	char chunk[128];

	fs_file_t_init(&file);
	rc = fs_open(&file, fs_path, FS_O_READ);
	if (rc != 0) {
		shell_error(sh, "open failed: %d", rc);
		return rc;
	}

	while (true) {
		ssize_t read_len = fs_read(&file, chunk, sizeof(chunk) - 1U);
		if (read_len < 0) {
			rc = static_cast<int>(read_len);
			shell_error(sh, "read failed: %d", rc);
			break;
		}

		if (read_len == 0) {
			rc = 0;
			break;
		}

		chunk[read_len] = '\0';
		shell_fprintf(sh, SHELL_NORMAL, "%s", chunk);
	}

	(void)fs_close(&file);
	shell_fprintf(sh, SHELL_NORMAL, "\n");
	return rc;
}

int CmdMkdir(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	int rc = storage::MakeDirectory(argv[1]);
	if (rc != 0) {
		shell_error(sh, "mkdir failed: %d", rc);
		return rc;
	}

	shell_print(sh, "Created %s", argv[1]);
	return 0;
}

int CmdRm(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	int rc = storage::DeletePath(argv[1]);
	if (rc != 0) {
		shell_error(sh, "rm failed: %d", rc);
		return rc;
	}

	shell_print(sh, "Deleted %s", argv[1]);
	return 0;
}

int CmdWriteFile(const struct shell *sh, size_t argc, char **argv)
{
	char content[512];
	int rc = JoinArgs(content, sizeof(content), argc, argv, 2);

	if (rc != 0) {
		shell_error(sh, "content too long or invalid");
		return rc;
	}

	if (strcmp(argv[1], settings::GetConfigRelativePath()) == 0) {
		rc = settings::WriteConfigJson(content, strlen(content));
		if (rc == 0) {
			rc = ApplyConfigFromStore();
		}
	} else {
		rc = storage::WriteTextFile(argv[1], content, strlen(content));
	}
	if (rc != 0) {
		shell_error(sh, "write failed: %d", rc);
		return rc;
	}

	shell_print(sh, "Wrote %u bytes to %s", static_cast<unsigned int>(strlen(content)), argv[1]);
	return 0;
}

int CmdCd(const struct shell *sh, size_t argc, char **argv)
{
	const char *path = (argc > 1) ? argv[1] : "/";
	char resolved[128];
	
	// Handle relative path
	if (path[0] != '/') {
		if (strcmp(g_cwd, "/") == 0) {
			snprintf(resolved, sizeof(resolved), "/%s", path);
		} else {
			snprintf(resolved, sizeof(resolved), "%s/%s", g_cwd, path);
		}
	} else {
		snprintf(resolved, sizeof(resolved), "%s", path);
	}
	
	// Check if directory exists
	char fs_path[128];
	int rc = storage::ResolveManagedPath(resolved, fs_path, sizeof(fs_path));
	if (rc != 0) {
		shell_error(sh, "cd: %s: Invalid path", path);
		return rc;
	}
	
	struct fs_dir_t dir;
	fs_dir_t_init(&dir);
	rc = fs_opendir(&dir, fs_path);
	if (rc != 0) {
		shell_error(sh, "cd: %s: Not a directory", path);
		return rc;
	}
	fs_closedir(&dir);
	
	// Update cwd
	snprintf(g_cwd, sizeof(g_cwd), "%s", resolved);
	return 0;
}

int CmdPwd(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	
	shell_print(sh, "%s", g_cwd);
	return 0;
}

int SaveEditorToTarget(const struct shell *sh, const char *path)
{
	if (strcmp(path, settings::GetConfigRelativePath()) == 0) {
		int rc = settings::WriteConfigJson(g_editor.GetContent(), strlen(g_editor.GetContent()));
		if (rc != 0) {
			shell_error(sh, "config validation failed: %d", rc);
			return rc;
		}

		rc = ApplyConfigFromStore();
		if (rc != 0) {
			shell_error(sh, "config apply failed: %d", rc);
			return rc;
		}

		return 0;
	}

	int rc = storage::WriteTextFile(path, g_editor.GetContent(), strlen(g_editor.GetContent()));
	if (rc != 0) {
		shell_error(sh, "save failed: %d", rc);
	}

	return rc;
}

int CmdEditOpen(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	int rc = g_editor.Open(argv[1]);
	if (rc != 0) {
		shell_error(sh, "edit open failed: %d", rc);
		return rc;
	}

	shell_print(sh, "Opened %s (%u lines)", g_editor.GetPath(),
		    static_cast<unsigned int>(g_editor.GetLineCount()));
	return 0;
}

int CmdEditStatus(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!g_editor.IsOpen()) {
		shell_print(sh, "Editor: closed");
		return 0;
	}

	shell_print(sh, "Editor: open path=%s lines=%u dirty=%s", g_editor.GetPath(),
		    static_cast<unsigned int>(g_editor.GetLineCount()),
		    g_editor.IsDirty() ? "true" : "false");
	return 0;
}

int CmdEditShow(const struct shell *sh, size_t argc, char **argv)
{
	if (!g_editor.IsOpen()) {
		shell_error(sh, "editor is not open");
		return -EINVAL;
	}

	size_t start = (argc > 1) ? static_cast<size_t>(strtoul(argv[1], nullptr, 10)) : 1U;
	size_t end = (argc > 2) ? static_cast<size_t>(strtoul(argv[2], nullptr, 10)) : 0U;
	char output[1536];
	int rc = g_editor.PrintLines(output, sizeof(output), start, end);

	if (rc != 0) {
		shell_error(sh, "edit show failed: %d", rc);
		return rc;
	}

	shell_fprintf(sh, SHELL_NORMAL, "%s", output);
	return 0;
}

int CmdEditSet(const struct shell *sh, size_t argc, char **argv)
{
	if (!g_editor.IsOpen()) {
		shell_error(sh, "editor is not open");
		return -EINVAL;
	}

	char text[256];
	int rc = JoinArgs(text, sizeof(text), argc, argv, 2);
	if (rc != 0) {
		shell_error(sh, "line too long");
		return rc;
	}

	rc = g_editor.ReplaceLine(static_cast<size_t>(strtoul(argv[1], nullptr, 10)), text);
	if (rc != 0) {
		shell_error(sh, "edit set failed: %d", rc);
		return rc;
	}

	shell_print(sh, "Line updated");
	return 0;
}

int CmdEditInsert(const struct shell *sh, size_t argc, char **argv)
{
	if (!g_editor.IsOpen()) {
		shell_error(sh, "editor is not open");
		return -EINVAL;
	}

	char text[256];
	int rc = JoinArgs(text, sizeof(text), argc, argv, 2);
	if (rc != 0) {
		shell_error(sh, "line too long");
		return rc;
	}

	rc = g_editor.InsertLine(static_cast<size_t>(strtoul(argv[1], nullptr, 10)), text);
	if (rc != 0) {
		shell_error(sh, "edit ins failed: %d", rc);
		return rc;
	}

	shell_print(sh, "Line inserted");
	return 0;
}

int CmdEditAppend(const struct shell *sh, size_t argc, char **argv)
{
	if (!g_editor.IsOpen()) {
		shell_error(sh, "editor is not open");
		return -EINVAL;
	}

	char text[256];
	int rc = JoinArgs(text, sizeof(text), argc, argv, 1);
	if (rc != 0) {
		shell_error(sh, "line too long");
		return rc;
	}

	rc = g_editor.AppendLine(text);
	if (rc != 0) {
		shell_error(sh, "edit app failed: %d", rc);
		return rc;
	}

	shell_print(sh, "Line appended");
	return 0;
}

int CmdEditDelete(const struct shell *sh, size_t argc, char **argv)
{
	if (!g_editor.IsOpen()) {
		shell_error(sh, "editor is not open");
		return -EINVAL;
	}

	int rc = g_editor.DeleteLine(static_cast<size_t>(strtoul(argv[1], nullptr, 10)));
	if (rc != 0) {
		shell_error(sh, "edit del failed: %d", rc);
		return rc;
	}

	shell_print(sh, "Line deleted");
	return 0;
}

int CmdEditWrite(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!g_editor.IsOpen()) {
		shell_error(sh, "editor is not open");
		return -EINVAL;
	}

	int rc = SaveEditorToTarget(sh, g_editor.GetPath());
	if (rc != 0) {
		return rc;
	}

	shell_print(sh, "Saved %s", g_editor.GetPath());
	return 0;
}

int CmdEditSaveAs(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (!g_editor.IsOpen()) {
		shell_error(sh, "editor is not open");
		return -EINVAL;
	}

	int rc = SaveEditorToTarget(sh, argv[1]);
	if (rc != 0) {
		return rc;
	}

	rc = g_editor.Open(argv[1]);
	if (rc != 0) {
		shell_error(sh, "reopen failed: %d", rc);
		return rc;
	}

	shell_print(sh, "Saved as %s", argv[1]);
	return 0;
}

int CmdEditQuit(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	bool dirty = g_editor.IsDirty();
	g_editor.Close();
	shell_print(sh, "Editor closed%s", dirty ? " (unsaved changes discarded)" : "");
	return 0;
}

int CmdReboot(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Rebooting...");
	k_sleep(K_MSEC(50));
	sys_reboot(SYS_REBOOT_COLD);
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
	SHELL_CMD(reboot, NULL, "Reboot the controller", CmdReboot),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_edit,
	SHELL_CMD_ARG(open, NULL, "edit open <path>", CmdEditOpen, 2, 0),
	SHELL_CMD(status, NULL, "Show current editor state", CmdEditStatus),
	SHELL_CMD_ARG(show, NULL, "edit show [start] [end]", CmdEditShow, 1, 2),
	SHELL_CMD_ARG(set, NULL, "edit set <line> <text>", CmdEditSet, 3, 16),
	SHELL_CMD_ARG(ins, NULL, "edit ins <line> <text>", CmdEditInsert, 3, 16),
	SHELL_CMD_ARG(app, NULL, "edit app <text>", CmdEditAppend, 2, 16),
	SHELL_CMD_ARG(del, NULL, "edit del <line>", CmdEditDelete, 2, 0),
	SHELL_CMD(write, NULL, "Save current file", CmdEditWrite),
	SHELL_CMD_ARG(saveas, NULL, "edit saveas <path>", CmdEditSaveAs, 2, 0),
	SHELL_CMD(quit, NULL, "Close editor", CmdEditQuit),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_show,
	SHELL_CMD_ARG(fans, NULL, "show fans [1|2]", CmdShowFans, 1, 1),
	SHELL_CMD(curves, NULL, "show curves", CmdShowCurves),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(fanctl, &sub_fanctl, "Fan controller commands", NULL);
SHELL_CMD_REGISTER(edit, &sub_edit, "Minimal line editor", NULL);
SHELL_CMD_REGISTER(show, &sub_show, "Operational show commands", NULL);
SHELL_CMD_ARG_REGISTER(ls, NULL, "ls [path]", CmdLs, 1, 1);
SHELL_CMD_ARG_REGISTER(cat, NULL, "cat <path>", CmdCat, 2, 0);
SHELL_CMD_ARG_REGISTER(mkdir, NULL, "mkdir <path>", CmdMkdir, 2, 0);
SHELL_CMD_ARG_REGISTER(cd, NULL, "cd <path>", CmdCd, 1, 1);
SHELL_CMD_REGISTER(pwd, NULL, "Print current directory", CmdPwd);
SHELL_CMD_ARG_REGISTER(rm, NULL, "rm <path>", CmdRm, 2, 0);
SHELL_CMD_ARG_REGISTER(writefile, NULL, "writefile <path> <text>", CmdWriteFile, 3, 16);
SHELL_CMD_ARG_REGISTER(wificonnect, NULL, "Wi-Fi connection utility: wificonnect [number] [password]", CmdWifiConnect, 1, 2);

} // namespace

void Init(FanController &fan_controller, WifiManager &wifi_manager,
	  HostControlManager &host_control)
{
	g_fan_controller = &fan_controller;
	g_wifi_manager = &wifi_manager;
	g_host_control = &host_control;
}

} // namespace fanctl::shell_commands
