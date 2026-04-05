/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#include "core/common.hpp"
#include "curve_profiles.hpp"
#include "fan_controller.hpp"
#include "host_control_manager.hpp"
#include "http_server.hpp"
#include "settings_store.hpp"
#include "core/service_context.hpp"
#include "shell_commands.hpp"
#include "ssh_server.hpp"
#include "storage.hpp"
#include "wifi_manager.hpp"

LOG_MODULE_REGISTER(fanctl_app, LOG_LEVEL_INF);

namespace {

fanctl::FanController g_fan_controller;
fanctl::HostControlManager g_host_control(g_fan_controller);
fanctl::WifiManager g_wifi_manager;
fanctl::ServiceContext g_services = { &g_fan_controller, &g_wifi_manager, &g_host_control };
fanctl::HttpServer g_http_server(g_services);
fanctl::SshServer g_ssh_server(g_services);

K_THREAD_STACK_DEFINE(g_telemetry_stack, fanctl::kTelemetryStackSize);
struct k_thread g_telemetry_thread;

void TelemetryThread(void *, void *, void *)
{
	while (true) {
		fanctl::WifiSnapshot snapshot = {};

		g_wifi_manager.GetSnapshot(&snapshot);
		g_fan_controller.UpdateTelemetry(snapshot.sta_connected, snapshot.ap_enabled);
		g_host_control.Tick();
		k_sleep(K_SECONDS(1));
	}
}

} // namespace

int main()
{
	g_fan_controller.InitRuntime();

	int rc = fanctl::storage::Init();
	if (rc != 0) {
		LOG_ERR("storage init failed: %d", rc);
		return rc;
	}

	rc = fanctl::settings::Init();
	if (rc != 0) {
		LOG_ERR("settings init failed: %d", rc);
		return rc;
	}

	g_fan_controller.LoadPersistedState();

	rc = g_fan_controller.Init();
	if (rc != 0) {
		LOG_ERR("fan controller init failed: %d", rc);
		return rc;
	}

	rc = g_wifi_manager.Init();
	if (rc != 0) {
		LOG_ERR("wifi manager init failed: %d", rc);
		return rc;
	}

	rc = g_host_control.Init();
	if (rc != 0) {
		LOG_ERR("host control init failed: %d", rc);
		return rc;
	}

	rc = g_ssh_server.Init();
	if (rc != 0) {
		LOG_ERR("ssh server init failed: %d", rc);
		return rc;
	}

	fanctl::shell_commands::Init(g_services);

	k_thread_create(&g_telemetry_thread, g_telemetry_stack,
			K_THREAD_STACK_SIZEOF(g_telemetry_stack), TelemetryThread, nullptr, nullptr,
			nullptr, 5, 0, K_NO_WAIT);
	k_thread_name_set(&g_telemetry_thread, "fanctl_telemetry");

	g_http_server.Start();
	g_ssh_server.Start();

	fanctl::WifiSnapshot wifi = {};
	g_wifi_manager.GetSnapshot(&wifi);

	printk("Fan controller ready.\n");
	printk("AP SSID: %s  PSK: %s  IP: %s\n", wifi.ap_ssid, fanctl::kApPsk, fanctl::kApIpAddr);
	printk("HTTP: http://%s/\n", fanctl::kApIpAddr);
	printk("Shell: fanctl status\n");
	printk("Config: %s\n", fanctl::settings::GetConfigRelativePath());
	printk("Curves: %s  %s  %s  %s\n", fanctl::curves::CurveProfiles::GetAdcToVoltagePath(),
	       fanctl::curves::CurveProfiles::GetVoltageToPercentPath(),
	       fanctl::curves::CurveProfiles::GetPercentToPwmPath(),
	       fanctl::curves::CurveProfiles::GetPercentToRpmPath());
	printk("SSH Config: %s\n", fanctl::settings::GetSshConfigRelativePath());
	printk("Authorized Keys: %s\n", fanctl::settings::GetAuthorizedKeysRelativePath());
	if (g_ssh_server.IsEnabled()) {
		printk("SSH: ssh root@%s -p %d\n", fanctl::kApIpAddr, g_ssh_server.GetListenPort());
	}

	while (true) {
		k_sleep(K_SECONDS(60));
	}
}
