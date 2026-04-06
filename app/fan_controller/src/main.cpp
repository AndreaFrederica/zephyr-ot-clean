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
#include "memory_domains.hpp"
#include "settings_store.hpp"
#include "core/service_context.hpp"
#include "shell_commands.hpp"
#include "ssh_server.hpp"
#include "storage.hpp"
#include "wifi_manager.hpp"

LOG_MODULE_REGISTER(fanctl_app, LOG_LEVEL_INF);

// 全局对象（用于跨模块访问）
namespace fanctl {
	FanController g_fan_controller;
	HostControlManager g_host_control(g_fan_controller.GetSharedState(), &g_fan_controller);
	WifiManager g_wifi_manager;
}

// 让 main 函数可以方便访问
using fanctl::g_fan_controller;
using fanctl::g_wifi_manager;
using fanctl::g_host_control;

// 检查共享内存状态有效
static_assert(sizeof(fanctl::FanControllerSharedState) < 1024, "Shared state too large");

namespace {

fanctl::ServiceContext g_services = { &g_fan_controller, &g_wifi_manager, &g_host_control };
fanctl::HttpServer g_http_server(g_services);
fanctl::SshServer g_ssh_server(g_services);

// 状态同步线程 (10Hz - 将 WiFi 状态同步到控制循环)
K_THREAD_STACK_DEFINE(g_status_sync_stack, fanctl::kStatusSyncStackSize);
struct k_thread g_status_sync_thread;

// 静态存储区 - 避免栈溢出
static fanctl::WifiSnapshot g_wifi_snapshot;
static fanctl::HostControlSnapshot g_host_snapshot;

void StatusSyncThread(void *, void *, void *)
{
	while (true) {
		// 使用静态存储，避免栈上分配大结构
		g_wifi_manager.RefreshRuntimeStatus();
		g_wifi_manager.GetSnapshot(&g_wifi_snapshot);
		
		// 同步 WiFi 状态到控制循环 (通过原子变量)
		g_fan_controller.SetWifiStatus(g_wifi_snapshot.sta_connected, 
						       g_wifi_snapshot.ap_enabled);
		
		// 主机控制心跳检测
		g_host_control.Tick();
		
		// 可选：更新主机控制快照供外部查询
		g_host_control.GetSnapshot(&g_host_snapshot);
		
		k_sleep(K_MSEC(200));  // 5Hz 降低频率减少栈压力
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

	rc = g_fan_controller.InitHardware();
	if (rc != 0) {
		LOG_ERR("fan controller init failed: %d", rc);
		return rc;
	}

	rc = fanctl::g_wifi_manager.Init();
	if (rc != 0) {
		LOG_ERR("wifi manager init failed: %d", rc);
		return rc;
	}

	rc = fanctl::g_host_control.Init();
	if (rc != 0) {
		LOG_ERR("host control init failed: %d", rc);
		return rc;
	}

	fanctl::memory::Init();

	rc = g_ssh_server.Init();
	if (rc != 0) {
		LOG_ERR("ssh server init failed: %d", rc);
		return rc;
	}

	fanctl::shell_commands::Init(g_services);

	// 启动 100Hz 控制循环
	g_fan_controller.StartControlLoop();

	// 启动状态同步线程
	k_thread_create(&g_status_sync_thread, g_status_sync_stack,
			K_THREAD_STACK_SIZEOF(g_status_sync_stack), StatusSyncThread, nullptr, nullptr,
			nullptr, 6, 0, K_NO_WAIT);
	k_thread_name_set(&g_status_sync_thread, "fanctl_sync");

	g_http_server.Start();
	g_ssh_server.Start();

	fanctl::WifiSnapshot wifi = {};
	g_wifi_manager.GetSnapshot(&wifi);

	printk("Fan controller ready (100Hz control loop).\n");
	
	// Show AP or STA info based on what's enabled
	if (wifi.ap_enabled) {
		printk("AP SSID: %s  PSK: %s  IP: %s\n", 
		       wifi.ap_ssid[0] != '\0' ? wifi.ap_ssid : "(starting)", 
		       fanctl::kApPsk, fanctl::kApIpAddr);
	}
	
	if (wifi.sta_connected) {
		printk("STA IP: %s  (connected to: %s)\n", wifi.sta_ip, wifi.saved_ssid);
	} else if (wifi.saved_ssid[0] != '\0') {
		printk("STA: Connecting to %s...\n", wifi.saved_ssid);
	}
	
	// HTTP server listens on all interfaces
	if (wifi.ap_enabled && wifi.sta_connected) {
		printk("HTTP: http://%s/  http://%s/\n", fanctl::kApIpAddr, wifi.sta_ip);
	} else if (wifi.sta_connected) {
		printk("HTTP: http://%s/\n", wifi.sta_ip);
	} else if (wifi.ap_enabled) {
		printk("HTTP: http://%s/\n", fanctl::kApIpAddr);
	}
	
	printk("Shell: fanctl status\n");
	printk("Config: %s\n", fanctl::settings::GetConfigRelativePath());
	printk("Curves: %s  %s  %s  %s\n", fanctl::curves::CurveProfiles::GetAdcToVoltagePath(),
	       fanctl::curves::CurveProfiles::GetVoltageToPercentPath(),
	       fanctl::curves::CurveProfiles::GetPercentToPwmPath(),
	       fanctl::curves::CurveProfiles::GetPercentToRpmPath());
	printk("SSH Config: %s\n", fanctl::settings::GetSshConfigRelativePath());
	printk("Authorized Keys: %s\n", fanctl::settings::GetAuthorizedKeysRelativePath());
	if (g_ssh_server.IsEnabled()) {
		const char *ssh_ip = wifi.sta_connected ? wifi.sta_ip : fanctl::kApIpAddr;
		printk("SSH: ssh root@%s -p %d\n", ssh_ip, g_ssh_server.GetListenPort());
	}

	while (true) {
		k_sleep(K_SECONDS(60));
	}
}
