/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wifi_manager.hpp"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <time.h>

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/sntp.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/wifi.h>
#include <zephyr/sys/util.h>

#include "settings_store.hpp"

LOG_MODULE_REGISTER(wifi_manager, LOG_LEVEL_INF);

namespace fanctl {

namespace {

WifiManager *g_instance = nullptr;

} // namespace

WifiManager::WifiManager()
	: ap_iface_(nullptr), sta_iface_(nullptr), ap_enabled_(false), sta_connected_(false),
	  ap_clients_(0)
{
	ap_ssid_[0] = '\0';
	saved_ssid_[0] = '\0';
}

void WifiManager::BuildApSsid()
{
	const struct net_linkaddr *link = net_if_get_link_addr(ap_iface_);

	if (link != nullptr && link->len >= 6U) {
		(void)snprintf(ap_ssid_, sizeof(ap_ssid_), "fanctl-%02x%02x%02x", link->addr[3],
			       link->addr[4], link->addr[5]);
	} else {
		(void)snprintf(ap_ssid_, sizeof(ap_ssid_), "fanctl-setup");
	}
}

int WifiManager::EnableDhcpServer()
{
	static struct net_in_addr addr;
	static struct net_in_addr netmask;

	if (net_addr_pton(AF_INET, kApIpAddr, &addr) != 0) {
		return -EINVAL;
	}

	if (net_addr_pton(AF_INET, kApNetmask, &netmask) != 0) {
		return -EINVAL;
	}

	net_if_ipv4_set_gw(ap_iface_, &addr);
	(void)net_if_ipv4_addr_add(ap_iface_, &addr, NET_ADDR_MANUAL, 0);
	(void)net_if_ipv4_set_netmask_by_addr(ap_iface_, &addr, &netmask);

	addr.s4_addr[3] += 10U;
	return net_dhcpv4_server_start(ap_iface_, &addr);
}

int WifiManager::ConnectToNetwork(const char *ssid, const char *psk)
{
	if (sta_iface_ == nullptr || ssid == nullptr || ssid[0] == '\0') {
		return -EINVAL;
	}

	size_t psk_len = (psk != nullptr) ? strlen(psk) : 0U;
	struct wifi_connect_req_params params = {};

	params.ssid = reinterpret_cast<const uint8_t *>(ssid);
	params.ssid_length = strlen(ssid);
	params.psk = reinterpret_cast<const uint8_t *>((psk_len > 0U) ? psk : "");
	params.psk_length = psk_len;
	params.security = (psk_len > 0U) ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE;
	params.channel = WIFI_CHANNEL_ANY;
	params.band = WIFI_FREQ_BAND_2_4_GHZ;
	params.mfp = WIFI_MFP_OPTIONAL;

	return net_mgmt(NET_REQUEST_WIFI_CONNECT, sta_iface_, &params, sizeof(params));
}

int WifiManager::ReadStatus(struct wifi_iface_status *status)
{
	memset(status, 0, sizeof(*status));

	if (sta_iface_ == nullptr) {
		return -ENODEV;
	}

	return net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, sta_iface_, status, sizeof(*status));
}

void WifiManager::EventHandler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			       struct net_if *iface)
{
	ARG_UNUSED(iface);

	if (g_instance != nullptr) {
		g_instance->HandleEvent(cb, mgmt_event);
	}
}

void WifiManager::SyncTimeViaNtp()
{
	settings::NtpConfig ntp_config = {};
	if (settings::LoadNtpConfig(&ntp_config) != 0) {
		LOG_WRN("Failed to load NTP config");
		return;
	}

	if (!ntp_config.enabled) {
		LOG_DBG("NTP sync disabled");
		return;
	}

	struct sntp_ctx ctx;
	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(ntp_config.port);

	if (net_addr_pton(AF_INET, ntp_config.server, &addr.sin_addr) != 0) {
		LOG_WRN("Invalid NTP server address: %s", ntp_config.server);
		return;
	}

	int rc = sntp_init(&ctx, (struct sockaddr *)&addr, sizeof(addr));
	if (rc < 0) {
		LOG_WRN("SNTP init failed: %d", rc);
		return;
	}

	struct sntp_time sntp_time = {};
	rc = sntp_query(&ctx, 5000, &sntp_time);
	sntp_close(&ctx);

	if (rc < 0) {
		LOG_WRN("SNTP query failed: %d", rc);
		return;
	}

	// 转换为 Unix 时间戳 (SNTP 时间从 1900-01-01 开始，Unix 时间从 1970-01-01 开始)
	time_t now = (time_t)(sntp_time.seconds - 2208988800U);

	LOG_INF("Time synced via NTP: %lld", static_cast<long long>(now));
}

void WifiManager::HandleEvent(struct net_mgmt_event_callback *cb, uint64_t mgmt_event)
{
	k_mutex_lock(&mutex_, K_FOREVER);

	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT: {
		const struct wifi_status *status = static_cast<const struct wifi_status *>(cb->info);
		bool was_connected = sta_connected_;
		sta_connected_ = (status->status == 0);
		// 如果刚刚连接成功，延迟触发 NTP 同步
		if (sta_connected_ && !was_connected) {
			k_mutex_unlock(&mutex_);
			k_sleep(K_SECONDS(2));  // 等待 DHCP 完成
			SyncTimeViaNtp();
			k_mutex_lock(&mutex_, K_FOREVER);
		}
		break;
	}
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		sta_connected_ = false;
		break;
	case NET_EVENT_WIFI_AP_ENABLE_RESULT:
		ap_enabled_ = true;
		break;
	case NET_EVENT_WIFI_AP_DISABLE_RESULT:
		ap_enabled_ = false;
		break;
	case NET_EVENT_WIFI_AP_STA_CONNECTED:
		++ap_clients_;
		break;
	case NET_EVENT_WIFI_AP_STA_DISCONNECTED:
		if (ap_clients_ > 0) {
			--ap_clients_;
		}
		break;
	default:
		break;
	}

	k_mutex_unlock(&mutex_);
}

int WifiManager::Init()
{
	k_mutex_init(&mutex_);

	ap_iface_ = net_if_get_wifi_sap();
	sta_iface_ = net_if_get_wifi_sta();
	if (ap_iface_ == nullptr || sta_iface_ == nullptr) {
		return -ENODEV;
	}

	g_instance = this;
	net_mgmt_init_event_callback(&callback_, EventHandler,
				     NET_EVENT_WIFI_CONNECT_RESULT |
					     NET_EVENT_WIFI_DISCONNECT_RESULT |
					     NET_EVENT_WIFI_AP_ENABLE_RESULT |
					     NET_EVENT_WIFI_AP_DISABLE_RESULT |
					     NET_EVENT_WIFI_AP_STA_CONNECTED |
					     NET_EVENT_WIFI_AP_STA_DISCONNECTED);
	net_mgmt_add_event_callback(&callback_);

	int rc = EnableAp();
	if (rc != 0) {
		return rc;
	}

	char json_ssid[WIFI_SSID_MAX_LEN + 1];
	char json_psk[WIFI_PSK_MAX_LEN + 1];
	if (settings::LoadWifiCredentials(json_ssid, sizeof(json_ssid), json_psk, sizeof(json_psk)) == 0 &&
	    json_ssid[0] != '\0') {
		k_mutex_lock(&mutex_, K_FOREVER);
		(void)snprintf(saved_ssid_, sizeof(saved_ssid_), "%s", json_ssid);
		k_mutex_unlock(&mutex_);

		rc = ConnectToNetwork(json_ssid, json_psk);
		if (rc != 0) {
			LOG_WRN("saved wifi connect failed: %d", rc);
		}
	}

	return 0;
}

int WifiManager::EnableAp()
{
	if (ap_iface_ == nullptr) {
		return -ENODEV;
	}

	k_mutex_lock(&mutex_, K_FOREVER);
	bool already_enabled = ap_enabled_;
	k_mutex_unlock(&mutex_);
	if (already_enabled) {
		return 0;
	}

	BuildApSsid();
	(void)EnableDhcpServer();

	struct wifi_connect_req_params params = {};
	params.ssid = reinterpret_cast<const uint8_t *>(ap_ssid_);
	params.ssid_length = strlen(ap_ssid_);
	params.psk = reinterpret_cast<const uint8_t *>(kApPsk);
	params.psk_length = strlen(kApPsk);
	params.security = WIFI_SECURITY_TYPE_PSK;
	params.channel = WIFI_CHANNEL_ANY;
	params.band = WIFI_FREQ_BAND_2_4_GHZ;

	int rc = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, ap_iface_, &params, sizeof(params));

	if (rc == 0) {
		k_mutex_lock(&mutex_, K_FOREVER);
		ap_enabled_ = true;
		k_mutex_unlock(&mutex_);
	}

	return rc == -EALREADY ? 0 : rc;
}

int WifiManager::DisableAp()
{
	if (ap_iface_ == nullptr) {
		return -ENODEV;
	}

	int rc = net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, ap_iface_, nullptr, 0);
	if (rc == 0 || rc == -EALREADY) {
		k_mutex_lock(&mutex_, K_FOREVER);
		ap_enabled_ = false;
		k_mutex_unlock(&mutex_);
		return 0;
	}

	return rc;
}

int WifiManager::SaveAndConnect(const char *ssid, const char *psk)
{
	if (ssid == nullptr || ssid[0] == '\0') {
		return -EINVAL;
	}

	size_t psk_len = (psk != nullptr) ? strlen(psk) : 0U;

	k_mutex_lock(&mutex_, K_FOREVER);
	(void)snprintf(saved_ssid_, sizeof(saved_ssid_), "%s", ssid);
	k_mutex_unlock(&mutex_);
	settings::SaveWifiCredentials(ssid, psk_len > 0U ? psk : "");

	(void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, sta_iface_, nullptr, 0);
	return ConnectToNetwork(ssid, psk);
}

int WifiManager::ClearCredentials()
{
	k_mutex_lock(&mutex_, K_FOREVER);
	saved_ssid_[0] = '\0';
	sta_connected_ = false;
	k_mutex_unlock(&mutex_);
	settings::ClearWifiCredentials();

	(void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, sta_iface_, nullptr, 0);
	return 0;
}

void WifiManager::GetSnapshot(WifiSnapshot *snapshot)
{
	if (snapshot == nullptr) {
		return;
	}

	struct wifi_iface_status status = {};
	(void)ReadStatus(&status);

	k_mutex_lock(&mutex_, K_FOREVER);
	snapshot->ap_enabled = ap_enabled_;
	snapshot->ap_clients = ap_clients_;
	snapshot->sta_connected = sta_connected_;
	(void)snprintf(snapshot->ap_ssid, sizeof(snapshot->ap_ssid), "%s", ap_ssid_);
	(void)snprintf(snapshot->saved_ssid, sizeof(snapshot->saved_ssid), "%s", saved_ssid_);
	k_mutex_unlock(&mutex_);

	(void)snprintf(snapshot->sta_state, sizeof(snapshot->sta_state), "%s",
		       wifi_state_txt(static_cast<enum wifi_iface_state>(status.state)));
	snapshot->sta_rssi = status.rssi;
}

} // namespace fanctl
