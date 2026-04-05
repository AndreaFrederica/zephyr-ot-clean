/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_WIFI_MANAGER_HPP_
#define FAN_CONTROLLER_WIFI_MANAGER_HPP_

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>

#include "core/common.hpp"
#include "settings_store.hpp"

// Forward declaration from zephyr/net/sntp.h
struct sntp_time;

namespace fanctl {

class WifiManager {
public:
	WifiManager();

	int Init();
	int EnableAp();
	int DisableAp();
	int SaveAndConnect(const char *ssid, const char *psk);
	int ClearCredentials();
	void GetSnapshot(WifiSnapshot *snapshot);
	void RefreshRuntimeStatus();
	
	// Scan API
	int StartScan();
	bool IsScanComplete();
	int GetScanStatus();
	void GetScanResults(WifiScanResult *results, size_t max_count, size_t *out_count);
	void ClearScanResults();

private:
	static void NtpSyncWorkHandler(struct k_work *work);
	static void ReconnectWorkHandler(struct k_work *work);
	bool TrySyncNtp(const char *server, int port, struct sntp_time *out_time);
	static void EventHandler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
				 struct net_if *iface);
	void HandleEvent(struct net_mgmt_event_callback *cb, uint64_t mgmt_event, struct net_if *iface);
	void HandleScanResult(struct wifi_scan_result *result);
	void BuildApSsid(const settings::WifiConfig *wifi_config = nullptr);
	int ConnectToNetwork(const char *ssid, const char *psk);
	int EnableDhcpServer();
	int DisableStaPowerSave();
	int ReadStatus(struct wifi_iface_status *status);
	void SyncTimeViaNtp();

	struct net_if *ap_iface_;
	struct net_if *sta_iface_;
	struct net_mgmt_event_callback wifi_callback_;
	struct net_mgmt_event_callback ipv4_callback_;
	struct k_mutex mutex_;
	bool ap_enabled_;
	bool sta_connected_;
	int ap_clients_;
	char ap_ssid_[WIFI_SSID_MAX_LEN + 1];
	char saved_ssid_[WIFI_SSID_MAX_LEN + 1];
	char cached_sta_state_[24];
	char cached_sta_ip_[NET_IPV4_ADDR_LEN];
	int cached_sta_rssi_;
	bool sta_power_save_disabled_;
	
	// Scan state
	WifiScanResult scan_results_[16];
	size_t scan_count_;
	bool scan_complete_;
	int scan_status_;
	struct k_sem scan_sem_;
	struct k_work_delayable ntp_sync_work_;
	struct k_work_delayable reconnect_work_;
	int reconnect_attempts_;
};

} // namespace fanctl

#endif
