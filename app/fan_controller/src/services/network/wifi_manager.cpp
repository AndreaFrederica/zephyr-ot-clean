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
#include <zephyr/net/net_event.h>
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

bool TrySyncNtpAddress(const struct net_in_addr *addr, int port, struct sntp_time *out_time)
{
	if (addr == nullptr || out_time == nullptr) {
		return false;
	}

	struct sntp_ctx ctx;
	struct sockaddr_in sock_addr = {};
	sock_addr.sin_family = AF_INET;
	sock_addr.sin_port = htons(port);
	sock_addr.sin_addr = *addr;

	int rc = sntp_init(&ctx, (struct sockaddr *)&sock_addr, sizeof(sock_addr));
	if (rc < 0) {
		return false;
	}

	rc = sntp_query(&ctx, 10000, out_time);
	sntp_close(&ctx);

	return rc >= 0;
}

} // namespace

WifiManager::WifiManager()
	: ap_iface_(nullptr), sta_iface_(nullptr), ap_enabled_(false), sta_connected_(false),
	  ap_clients_(0), scan_count_(0), scan_complete_(false)
{
	ap_ssid_[0] = '\0';
	saved_ssid_[0] = '\0';
	memset(scan_results_, 0, sizeof(scan_results_));
	k_sem_init(&scan_sem_, 0, 1);
	k_work_init_delayable(&ntp_sync_work_, NtpSyncWorkHandler);
}

void WifiManager::NtpSyncWorkHandler(struct k_work *work)
{
	struct k_work_delayable *delayable = k_work_delayable_from_work(work);
	WifiManager *self = CONTAINER_OF(delayable, WifiManager, ntp_sync_work_);
	if (self != nullptr) {
		self->SyncTimeViaNtp();
	}
}

void WifiManager::BuildApSsid(const settings::WifiConfig *wifi_config)
{
	const struct net_linkaddr *link = net_if_get_link_addr(ap_iface_);
	const char *prefix = wifi_config ? wifi_config->ap_ssid_prefix : "fanctl";
	bool use_mac_suffix = wifi_config ? wifi_config->ap_ssid_use_mac_suffix : true;
	const char *custom_ssid = wifi_config ? wifi_config->ap_ssid_custom : nullptr;

	// 如果设置了自定义 SSID，直接使用
	if (custom_ssid != nullptr && custom_ssid[0] != '\0') {
		(void)snprintf(ap_ssid_, sizeof(ap_ssid_), "%s", custom_ssid);
		return;
	}

	// 使用 MAC 后缀
	if (use_mac_suffix && link != nullptr && link->len >= 6U) {
		(void)snprintf(ap_ssid_, sizeof(ap_ssid_), "%s-%02x%02x%02x", prefix,
			       link->addr[3], link->addr[4], link->addr[5]);
	} else {
		(void)snprintf(ap_ssid_, sizeof(ap_ssid_), "%s-setup", prefix);
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
	if (g_instance != nullptr) {
		g_instance->HandleEvent(cb, mgmt_event, iface);
	}
}

bool WifiManager::TrySyncNtp(const char *server, int port, struct sntp_time *out_time)
{
	if (server == nullptr || server[0] == '\0' || out_time == nullptr) {
		return false;
	}

	char port_string[8];
	(void)snprintf(port_string, sizeof(port_string), "%d", port);

	struct zsock_addrinfo hints = {};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;

	struct zsock_addrinfo *results = nullptr;
	int rc = zsock_getaddrinfo(server, port_string, &hints, &results);
	if (rc != 0 || results == nullptr) {
		LOG_DBG("DNS resolve failed for %s: %d", server, rc);
		return false;
	}

	bool success = false;
	for (struct zsock_addrinfo *entry = results; entry != nullptr; entry = entry->ai_next) {
		if (entry->ai_family != AF_INET || entry->ai_addrlen < sizeof(struct sockaddr_in)) {
			continue;
		}

		const struct sockaddr_in *addr = reinterpret_cast<const struct sockaddr_in *>(entry->ai_addr);
		if (TrySyncNtpAddress(&addr->sin_addr, port, out_time)) {
			success = true;
			break;
		}
	}

	zsock_freeaddrinfo(results);
	return success;
}

void WifiManager::SyncTimeViaNtp()
{
	settings::NtpConfig ntp_config = {};
	if (settings::LoadNtpConfig(&ntp_config) != 0) {
		LOG_WRN("Failed to load NTP config");
		return;
	}

	if (!ntp_config.enabled) {
		LOG_INF("NTP sync disabled");
		return;
	}

	// 先检查网络是否真正就绪（ping 网关）
	if (!sta_connected_) {
		LOG_WRN("Skip NTP: WiFi not connected");
		return;
	}

	LOG_INF("NTP sync starting...");

	struct sntp_time sntp_time = {};
	bool synced = false;

	if (ntp_config.use_dhcp_server && sta_iface_ != nullptr) {
#ifdef CONFIG_NET_DHCPV4_OPTION_NTP_SERVER
		if (!net_ipv4_is_addr_unspecified(&sta_iface_->config.dhcpv4.ntp_addr)) {
			char dhcp_ntp[NET_IPV4_ADDR_LEN];
			if (net_addr_ntop(AF_INET, &sta_iface_->config.dhcpv4.ntp_addr, dhcp_ntp,
					  sizeof(dhcp_ntp)) == nullptr) {
				(void)snprintf(dhcp_ntp, sizeof(dhcp_ntp), "<invalid>");
			}

			LOG_INF("Trying DHCP NTP: %s:%d", dhcp_ntp, ntp_config.port);
			if (TrySyncNtpAddress(&sta_iface_->config.dhcpv4.ntp_addr, ntp_config.port,
					      &sntp_time)) {
				synced = true;
				LOG_INF("NTP sync success from DHCP server: %s", dhcp_ntp);
			}
		} else {
			LOG_INF("DHCP did not provide an NTP server");
		}
#else
		LOG_INF("DHCP NTP server option is not enabled in this build");
#endif
	}

	if (!synced) {
		for (size_t i = 0; i < ntp_config.server_count; ++i) {
			LOG_INF("Trying configured NTP %u/%u: %s:%d",
				static_cast<unsigned int>(i + 1),
				static_cast<unsigned int>(ntp_config.server_count),
				ntp_config.servers[i], ntp_config.port);
			if (TrySyncNtp(ntp_config.servers[i], ntp_config.port, &sntp_time)) {
				synced = true;
				LOG_INF("NTP sync success from configured server: %s", ntp_config.servers[i]);
				break;
			}
			k_sleep(K_MSEC(100));
		}
	}

	if (!synced) {
		LOG_WRN("NTP sync failed: all servers unreachable");
		return;
	}

	struct timespec tspec = {};
	tspec.tv_sec = static_cast<time_t>(sntp_time.seconds);
	tspec.tv_nsec = static_cast<long>(((uint64_t)sntp_time.fraction * 1000000000ULL) >> 32);

	if (clock_settime(CLOCK_REALTIME, &tspec) != 0) {
		LOG_WRN("Failed to set system time: %d", errno);
	}

	time_t now = tspec.tv_sec;

	// 转换为可读格式
	struct tm *timeinfo = gmtime(&now);
	if (timeinfo != nullptr) {
		LOG_INF("NTP sync OK: %04d-%02d-%02d %02d:%02d:%02d UTC (unix: %lld)",
			timeinfo->tm_year + 1900,
			timeinfo->tm_mon + 1,
			timeinfo->tm_mday,
			timeinfo->tm_hour,
			timeinfo->tm_min,
			timeinfo->tm_sec,
			static_cast<long long>(now));
	} else {
		LOG_INF("NTP sync OK: unix timestamp=%lld", static_cast<long long>(now));
	}
}

void WifiManager::HandleEvent(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			      struct net_if *iface)
{
	k_mutex_lock(&mutex_, K_FOREVER);
	bool schedule_ntp_sync = false;

	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT: {
		const struct wifi_status *status = static_cast<const struct wifi_status *>(cb->info);
		bool was_connected = sta_connected_;
		sta_connected_ = (status->status == 0);
		if (sta_connected_) {
			LOG_INF("WiFi connected to: %s", saved_ssid_);
			if (!was_connected) {
				schedule_ntp_sync = true;
			}
		} else {
			LOG_WRN("WiFi connection failed: status=%d", status->status);
		}
		break;
	}
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		sta_connected_ = false;
		(void)k_work_cancel_delayable(&ntp_sync_work_);
		LOG_INF("WiFi disconnected");
		break;
	case NET_EVENT_WIFI_AP_ENABLE_RESULT:
		ap_enabled_ = true;
		break;
	case NET_EVENT_WIFI_AP_DISABLE_RESULT:
		ap_enabled_ = false;
		break;
	case NET_EVENT_WIFI_AP_STA_CONNECTED:
		++ap_clients_;
		LOG_INF("AP client connected, total: %d", ap_clients_);
		break;
	case NET_EVENT_WIFI_AP_STA_DISCONNECTED:
		if (ap_clients_ > 0) {
			--ap_clients_;
		}
		LOG_INF("AP client disconnected, total: %d", ap_clients_);
		break;
	case NET_EVENT_WIFI_SCAN_RESULT: {
		const struct wifi_scan_result *result =
			static_cast<const struct wifi_scan_result *>(cb->info);
		HandleScanResult(const_cast<struct wifi_scan_result *>(result));
		break;
	}
	case NET_EVENT_WIFI_SCAN_DONE:
		scan_complete_ = true;
		k_sem_give(&scan_sem_);
		LOG_INF("WiFi scan completed, found %u networks", scan_count_);
		break;
	case NET_EVENT_IPV4_DHCP_BOUND:
	case NET_EVENT_IPV4_ADDR_ADD: {
		if (sta_iface_ != nullptr && iface == sta_iface_) {
			char ip[NET_IPV4_ADDR_LEN] = { 0 };
			struct net_in_addr *ipv4 = net_if_ipv4_get_global_addr(sta_iface_, NET_ADDR_PREFERRED);
			if (ipv4 != nullptr &&
			    net_addr_ntop(AF_INET, ipv4, ip, sizeof(ip)) != nullptr) {
				LOG_INF("STA IPv4 ready: %s", ip);
			}
		}
		break;
	}
	default:
		break;
	}

	k_mutex_unlock(&mutex_);

	if (schedule_ntp_sync) {
		(void)k_work_reschedule(&ntp_sync_work_, K_SECONDS(5));
	}
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
					     NET_EVENT_WIFI_AP_STA_DISCONNECTED |
					     NET_EVENT_WIFI_SCAN_RESULT |
					     NET_EVENT_WIFI_SCAN_DONE |
					     NET_EVENT_IPV4_ADDR_ADD |
					     NET_EVENT_IPV4_DHCP_BOUND);
	net_mgmt_add_event_callback(&callback_);

	// Load WiFi config
	settings::WifiConfig wifi_config = {};
	if (settings::LoadWifiConfig(&wifi_config) != 0) {
		settings::FillWifiDefaults(&wifi_config);
	}

	// Note: DHCP hostname setting requires CONFIG_NET_HOSTNAME_ENABLE
	// For now, we skip this as it's not critical for functionality
	if (wifi_config.dhcp_hostname[0] != '\0') {
		LOG_INF("DHCP hostname would be: %s (requires CONFIG_NET_HOSTNAME_ENABLE)",
			wifi_config.dhcp_hostname);
	}

	LOG_INF("WiFi init: AP mode %s, saved SSID: %s",
		wifi_config.ap_enabled ? "enabled" : "disabled",
		wifi_config.sta_ssid[0] != '\0' ? wifi_config.sta_ssid : "(none)");

	// Enable AP mode if configured (default: enabled)
	if (wifi_config.ap_enabled) {
		// 使用配置中的 AP SSID 设置
		BuildApSsid(&wifi_config);
		int rc = EnableAp();
		if (rc != 0) {
			LOG_WRN("AP enable failed: %d", rc);
			// Continue anyway, STA might still work
		} else {
			LOG_INF("AP enabled: SSID=%s, IP=%s", ap_ssid_, kApIpAddr);
		}
	}

	// Connect to saved WiFi if credentials exist
	if (wifi_config.sta_ssid[0] != '\0') {
		k_mutex_lock(&mutex_, K_FOREVER);
		(void)snprintf(saved_ssid_, sizeof(saved_ssid_), "%s", wifi_config.sta_ssid);
		k_mutex_unlock(&mutex_);

		LOG_INF("Connecting to saved WiFi: %s", wifi_config.sta_ssid);
		int rc = ConnectToNetwork(wifi_config.sta_ssid, wifi_config.sta_psk);
		if (rc != 0) {
			LOG_WRN("WiFi connect failed: %d", rc);
		}
		// Connection result will be logged in HandleEvent
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
	sta_connected_ = false;
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
	snapshot->sta_ip[0] = '\0';
	if (sta_iface_ != nullptr) {
		struct net_in_addr *ipv4 = net_if_ipv4_get_global_addr(sta_iface_, NET_ADDR_PREFERRED);
		if (ipv4 != nullptr &&
		    net_addr_ntop(AF_INET, ipv4, snapshot->sta_ip, sizeof(snapshot->sta_ip)) == nullptr) {
			snapshot->sta_ip[0] = '\0';
		}
	}
	snapshot->sta_rssi = status.rssi;
}

void WifiManager::HandleScanResult(struct wifi_scan_result *result)
{
	if (result == nullptr || scan_count_ >= ARRAY_SIZE(scan_results_)) {
		return;
	}
	
	// Skip empty SSIDs
	if (result->ssid_length == 0 || result->ssid[0] == '\0') {
		return;
	}
	
	// Check for duplicate
	for (size_t i = 0; i < scan_count_; ++i) {
		if (strncmp(scan_results_[i].ssid, reinterpret_cast<const char*>(result->ssid), WIFI_SSID_MAX_LEN) == 0) {
			// Update if new result has better signal
			if (result->rssi > scan_results_[i].rssi) {
				scan_results_[i].rssi = result->rssi;
				scan_results_[i].channel = result->channel;
				memcpy(scan_results_[i].bssid, result->mac, 6);
			}
			return;
		}
	}
	
	// Add new result
	WifiScanResult &entry = scan_results_[scan_count_++];
	(void)snprintf(entry.ssid, sizeof(entry.ssid), "%s", reinterpret_cast<const char*>(result->ssid));
	memcpy(entry.bssid, result->mac, 6);
	entry.rssi = result->rssi;
	entry.channel = result->channel;
	entry.security = result->security;
	entry.valid = true;
}

int WifiManager::StartScan()
{
	if (sta_iface_ == nullptr) {
		return -ENODEV;
	}
	
	// Clear previous results
	scan_count_ = 0;
	scan_complete_ = false;
	memset(scan_results_, 0, sizeof(scan_results_));
	
	struct wifi_scan_params params = {};
	params.scan_type = WIFI_SCAN_TYPE_ACTIVE;
	params.dwell_time_active = 100;
	params.dwell_time_passive = 200;
	
	int rc = net_mgmt(NET_REQUEST_WIFI_SCAN, sta_iface_, &params, sizeof(params));
	if (rc != 0 && rc != -EALREADY) {
		LOG_WRN("Scan request failed: %d", rc);
		return rc;
	}
	
	return 0;
}

bool WifiManager::IsScanComplete()
{
	return scan_complete_;
}

void WifiManager::GetScanResults(WifiScanResult *results, size_t max_count, size_t *out_count)
{
	if (results == nullptr || out_count == nullptr) {
		return;
	}
	
	k_mutex_lock(&mutex_, K_FOREVER);
	size_t count = (scan_count_ < max_count) ? scan_count_ : max_count;
	for (size_t i = 0; i < count; ++i) {
		results[i] = scan_results_[i];
	}
	*out_count = count;
	k_mutex_unlock(&mutex_);
}

void WifiManager::ClearScanResults()
{
	k_mutex_lock(&mutex_, K_FOREVER);
	scan_count_ = 0;
	scan_complete_ = false;
	memset(scan_results_, 0, sizeof(scan_results_));
	k_mutex_unlock(&mutex_);
}

} // namespace fanctl
