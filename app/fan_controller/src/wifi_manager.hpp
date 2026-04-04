/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_WIFI_MANAGER_HPP_
#define FAN_CONTROLLER_WIFI_MANAGER_HPP_

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>

#include "common.hpp"

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

private:
	static void EventHandler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
				 struct net_if *iface);
	void HandleEvent(struct net_mgmt_event_callback *cb, uint64_t mgmt_event);
	void BuildApSsid();
	int ConnectToNetwork(const char *ssid, const char *psk);
	int EnableDhcpServer();
	int ReadStatus(struct wifi_iface_status *status);
	void SyncTimeViaNtp();

	struct net_if *ap_iface_;
	struct net_if *sta_iface_;
	struct net_mgmt_event_callback callback_;
	struct k_mutex mutex_;
	bool ap_enabled_;
	bool sta_connected_;
	int ap_clients_;
	char ap_ssid_[WIFI_SSID_MAX_LEN + 1];
	char saved_ssid_[WIFI_SSID_MAX_LEN + 1];
};

} // namespace fanctl

#endif
