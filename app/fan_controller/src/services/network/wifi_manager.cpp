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

// 设置日志级别为 INF，但关键函数使用 LOG_DBG
// 如需查看详细 WiFi 调试日志，可改为 LOG_LEVEL_DBG
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
	  ap_clients_(0), cached_sta_rssi_(0), sta_power_save_disabled_(false), scan_count_(0),
	  scan_complete_(false), scan_status_(0)
{
	ap_ssid_[0] = '\0';
	saved_ssid_[0] = '\0';
	(void)snprintf(cached_sta_state_, sizeof(cached_sta_state_), "%s", "INACTIVE");
	cached_sta_ip_[0] = '\0';
	memset(scan_results_, 0, sizeof(scan_results_));
	k_sem_init(&scan_sem_, 0, 1);
	k_work_init_delayable(&ntp_sync_work_, NtpSyncWorkHandler);
	k_work_init_delayable(&reconnect_work_, ReconnectWorkHandler);
	reconnect_attempts_ = 0;
}

void WifiManager::NtpSyncWorkHandler(struct k_work *work)
{
	struct k_work_delayable *delayable = k_work_delayable_from_work(work);
	WifiManager *self = CONTAINER_OF(delayable, WifiManager, ntp_sync_work_);
	if (self != nullptr) {
		self->SyncTimeViaNtp();
	}
}

void WifiManager::ReconnectWorkHandler(struct k_work *work)
{
	struct k_work_delayable *delayable = k_work_delayable_from_work(work);
	WifiManager *self = CONTAINER_OF(delayable, WifiManager, reconnect_work_);
	if (self == nullptr || self->sta_iface_ == nullptr) {
		return;
	}
	
	k_mutex_lock(&self->mutex_, K_FOREVER);
	// Only reconnect if not already connected and have credentials
	if (!self->sta_connected_ && self->saved_ssid_[0] != '\0') {
		self->reconnect_attempts_++;
		LOG_INF("WiFi STA: Reconnect attempt %d/10 to %s", 
			self->reconnect_attempts_, self->saved_ssid_);
		
		char ssid[WIFI_SSID_MAX_LEN + 1];
		char psk[WIFI_PSK_MAX_LEN + 1];
		(void)strncpy(ssid, self->saved_ssid_, sizeof(ssid) - 1);
		ssid[sizeof(ssid) - 1] = '\0';
		
		// Get PSK from settings
		settings::WifiConfig wifi_config = {};
		if (settings::LoadWifiConfig(&wifi_config) == 0) {
			(void)strncpy(psk, wifi_config.sta_psk, sizeof(psk) - 1);
			psk[sizeof(psk) - 1] = '\0';
		} else {
			psk[0] = '\0';
		}
		k_mutex_unlock(&self->mutex_);
		
		int rc = self->ConnectToNetwork(ssid, psk);
		if (rc != 0) {
			LOG_WRN("WiFi STA: Reconnect failed: %d", rc);
			// Schedule next retry with exponential backoff
			uint32_t delay_sec = (self->reconnect_attempts_ < 5) ? 5 : 30;
			if (self->reconnect_attempts_ < 10) {
				k_work_schedule(&self->reconnect_work_, K_SECONDS(delay_sec));
			} else {
				LOG_ERR("WiFi STA: Max reconnection attempts reached, giving up");
			}
		}
	} else {
		k_mutex_unlock(&self->mutex_);
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
		LOG_ERR("ConnectToNetwork: invalid parameters");
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

	LOG_INF("========================================");
	LOG_INF("WiFi STA: Starting connection attempt");
	LOG_INF("  SSID:     %s", ssid);
	LOG_INF("  Security: %s", psk_len > 0 ? "WPA2-PSK" : "Open");
	LOG_INF("  Channel:  Auto (any)");
	LOG_INF("  Band:     2.4GHz");
	LOG_INF("========================================");

	int rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, sta_iface_, &params, sizeof(params));
	
	if (rc != 0) {
		LOG_ERR("WiFi STA: net_mgmt connect request failed: %d (%s)", 
			rc, rc == -EALREADY ? "Already connecting/connected" : 
			    rc == -EINVAL ? "Invalid parameter" : 
			    rc == -ENODEV ? "Device not ready" : "Unknown error");
	}
	
	return rc;
}

int WifiManager::ReadStatus(struct wifi_iface_status *status)
{
	memset(status, 0, sizeof(*status));

	if (sta_iface_ == nullptr) {
		return -ENODEV;
	}

	return net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, sta_iface_, status, sizeof(*status));
}

int WifiManager::DisableStaPowerSave()
{
	if (sta_iface_ == nullptr) {
		return -ENODEV;
	}

	struct wifi_ps_params params = {};
	params.enabled = WIFI_PS_DISABLED;
	params.type = WIFI_PS_PARAM_STATE;

	int rc = net_mgmt(NET_REQUEST_WIFI_PS, sta_iface_, &params, sizeof(params));
	if (rc == 0) {
		LOG_INF("WiFi STA: power save disabled for low-latency traffic");
	} else {
		LOG_WRN("WiFi STA: failed to disable power save: %d", rc);
	}

	return rc;
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
	bool disable_sta_power_save = false;

	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT: {
		const struct wifi_status *status = static_cast<const struct wifi_status *>(cb->info);
		bool was_connected = sta_connected_;
		sta_connected_ = (status->status == 0);
		
		LOG_INF("========================================");
		if (sta_connected_) {
			LOG_INF("WiFi STA: CONNECTED SUCCESSFULLY");
			LOG_INF("  SSID: %s", saved_ssid_);
			sta_power_save_disabled_ = false;
			// Cancel any pending reconnect and reset counter
			if (reconnect_attempts_ > 0) {
				(void)k_work_cancel_delayable(&reconnect_work_);
				reconnect_attempts_ = 0;
				LOG_INF("WiFi STA: Reconnect successful, reset attempt counter");
			}
			if (!was_connected) {
				schedule_ntp_sync = true;
			}
		} else {
			LOG_ERR("WiFi STA: CONNECTION FAILED");
			LOG_ERR("  Status code: %d", status->status);
			// ESP32 WiFi status codes
			if (status->status == 1) {
				LOG_ERR("  Reason: AUTH_EXPIRE (Authentication expired)");
			} else if (status->status == 2) {
				LOG_ERR("  Reason: AUTH_LEAVE (Authentication left)");
			} else if (status->status == 3) {
				LOG_ERR("  Reason: ASSOC_EXPIRE (Association expired)");
			} else if (status->status == 4) {
				LOG_ERR("  Reason: ASSOC_TOOMANY (Too many associations)");
			} else if (status->status == 5) {
				LOG_ERR("  Reason: NOT_AUTHED (Not authenticated)");
			} else if (status->status == 6) {
				LOG_ERR("  Reason: NOT_ASSOCED (Not associated)");
			} else if (status->status == 7) {
				LOG_ERR("  Reason: ASSOC_LEAVE (Association left)");
			} else if (status->status == 8) {
				LOG_ERR("  Reason: ASSOC_NOT_AUTHED (Association not authenticated)");
			} else if (status->status == 9) {
				LOG_ERR("  Reason: DISASSOC_PWRCAP_BAD (Disassociate - power capability bad)");
			} else if (status->status == 10) {
				LOG_ERR("  Reason: DISASSOC_SUPCHAN_BAD (Disassociate - supported channels bad)");
			} else if (status->status == 11) {
				LOG_ERR("  Reason: IE_INVALID (Invalid IE)");
			} else if (status->status == 12) {
				LOG_ERR("  Reason: MIC_FAILURE (MIC failure)");
			} else if (status->status == 13) {
				LOG_ERR("  Reason: 4WAY_HANDSHAKE_TIMEOUT (4-way handshake timeout)");
				LOG_ERR("  Hint: Check if password is correct");
			} else if (status->status == 14) {
				LOG_ERR("  Reason: GROUP_KEY_UPDATE_TIMEOUT (Group key handshake timeout)");
			} else if (status->status == 15) {
				LOG_ERR("  Reason: IE_IN_4WAY_DIFFERS (IE in 4-way differs)");
			} else if (status->status == 16) {
				LOG_ERR("  Reason: GROUP_CIPHER_INVALID (Group cipher invalid)");
			} else if (status->status == 17) {
				LOG_ERR("  Reason: PAIRWISE_CIPHER_INVALID (Pairwise cipher invalid)");
			} else if (status->status == 18) {
				LOG_ERR("  Reason: AKMP_INVALID (AKMP invalid)");
			} else if (status->status == 19) {
				LOG_ERR("  Reason: UNSUPP_RSN_IE_VERSION (Unsupported RSN IE version)");
			} else if (status->status == 20) {
				LOG_ERR("  Reason: INVALID_RSN_IE_CAP (Invalid RSN IE capability)");
			} else if (status->status == 21) {
				LOG_ERR("  Reason: 802_1X_AUTH_FAILED (802.1X authentication failed)");
			} else if (status->status == 22) {
				LOG_ERR("  Reason: CIPHER_SUITE_REJECTED (Cipher suite rejected)");
			} else if (status->status == 23) {
				LOG_ERR("  Reason: INVALID_PMKID (Invalid PMKID)");
			} else if (status->status == 24) {
				LOG_ERR("  Reason: NO_AP_FOUND (No AP found)");
				LOG_ERR("  Hint: Check if SSID is correct and in range");
			} else if (status->status == 25) {
				LOG_ERR("  Reason: SCAN_FAIL (Scan failed)");
			} else if (status->status == 26) {
				LOG_ERR("  Reason: BEACON_TIMEOUT (Beacon timeout)");
				LOG_ERR("  Hint: AP may be out of range or interference");
			} else if (status->status == 203) {
				LOG_ERR("  Reason: ASSOC_FAIL (Association failed)");
			} else if (status->status == 205) {
				LOG_ERR("  Reason: CONNECTION_FAIL (Connection failed)");
			} else if (status->status == 206) {
				LOG_ERR("  Reason: AP_NOT_FOUND (AP not found)");
				LOG_ERR("  Hint: Check if SSID is correct");
			} else {
				LOG_ERR("  Reason: Unknown error code");
			}
		}
		LOG_INF("========================================");
		break;
	}
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		sta_connected_ = false;
		sta_power_save_disabled_ = false;
		(void)k_work_cancel_delayable(&ntp_sync_work_);
		LOG_INF("WiFi STA: Disconnected from AP");
		// Schedule reconnect if we have saved credentials
		if (saved_ssid_[0] != '\0' && reconnect_attempts_ < 10) {
			LOG_INF("WiFi STA: Will attempt reconnect in 5 seconds...");
			k_work_schedule(&reconnect_work_, K_SECONDS(5));
		}
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
		// 详细日志在 HandleScanResult 中
		break;
	}
	case NET_EVENT_WIFI_SCAN_DONE:
		scan_status_ = cb->info != nullptr
				     ? static_cast<const struct wifi_status *>(cb->info)->status
				     : 0;
		scan_complete_ = true;
		k_sem_give(&scan_sem_);
		LOG_INF("========================================");
		LOG_INF("WiFi Scan: COMPLETED");
		LOG_INF("  Status: %d", scan_status_);
		LOG_INF("  Total networks found: %u", scan_count_);
		LOG_INF("========================================");
		break;
	case NET_EVENT_IPV4_DHCP_BOUND: {
		if (sta_iface_ != nullptr && iface == sta_iface_) {
			if (sta_connected_ && !sta_power_save_disabled_) {
				disable_sta_power_save = true;
			}
			char ip[NET_IPV4_ADDR_LEN] = { 0 };
			struct net_in_addr *ipv4 = net_if_ipv4_get_global_addr(sta_iface_, NET_ADDR_PREFERRED);
			if (ipv4 != nullptr &&
			    net_addr_ntop(AF_INET, ipv4, ip, sizeof(ip)) != nullptr) {
				LOG_INF("========================================");
				LOG_INF("WiFi STA: DHCP Bound (IPv4 ready)");
				LOG_INF("  IP Address: %s", ip);
				
				// 获取网关信息
				struct net_if_ipv4 *ipv4_config = sta_iface_->config.ip.ipv4;
				if (ipv4_config != nullptr && ipv4_config->gw.s_addr != 0) {
					char gw[NET_IPV4_ADDR_LEN];
					if (net_addr_ntop(AF_INET, &ipv4_config->gw, gw, sizeof(gw)) != nullptr) {
						LOG_INF("  Gateway:    %s", gw);
					}
				}
				LOG_INF("========================================");
			}
		}
		break;
	}
	case NET_EVENT_IPV4_ADDR_ADD: {
		if (sta_iface_ != nullptr && iface == sta_iface_) {
			char ip[NET_IPV4_ADDR_LEN] = { 0 };
			struct net_in_addr *ipv4 = net_if_ipv4_get_global_addr(sta_iface_, NET_ADDR_PREFERRED);
			if (ipv4 != nullptr &&
			    net_addr_ntop(AF_INET, ipv4, ip, sizeof(ip)) != nullptr) {
				LOG_INF("WiFi STA: IPv4 address added: %s", ip);
			}
		}
		break;
	}
	default:
		break;
	}

	k_mutex_unlock(&mutex_);

	if (disable_sta_power_save) {
		if (DisableStaPowerSave() == 0) {
			k_mutex_lock(&mutex_, K_FOREVER);
			sta_power_save_disabled_ = true;
			k_mutex_unlock(&mutex_);
		}
	}

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
	net_mgmt_init_event_callback(&wifi_callback_, EventHandler,
				     NET_EVENT_WIFI_CONNECT_RESULT |
					     NET_EVENT_WIFI_DISCONNECT_RESULT |
					     NET_EVENT_WIFI_AP_ENABLE_RESULT |
					     NET_EVENT_WIFI_AP_DISABLE_RESULT |
					     NET_EVENT_WIFI_AP_STA_CONNECTED |
					     NET_EVENT_WIFI_AP_STA_DISCONNECTED |
					     NET_EVENT_WIFI_SCAN_RESULT |
					     NET_EVENT_WIFI_SCAN_DONE);
	net_mgmt_add_event_callback(&wifi_callback_);

	net_mgmt_init_event_callback(&ipv4_callback_, EventHandler,
				     NET_EVENT_IPV4_ADDR_ADD |
					     NET_EVENT_IPV4_DHCP_BOUND);
	net_mgmt_add_event_callback(&ipv4_callback_);

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

	RefreshRuntimeStatus();

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
	sta_power_save_disabled_ = false;
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
	cached_sta_rssi_ = 0;
	cached_sta_ip_[0] = '\0';
	sta_power_save_disabled_ = false;
	(void)snprintf(cached_sta_state_, sizeof(cached_sta_state_), "%s", "INACTIVE");
	k_mutex_unlock(&mutex_);
	settings::ClearWifiCredentials();

	(void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, sta_iface_, nullptr, 0);
	return 0;
}

void WifiManager::RefreshRuntimeStatus()
{
	struct wifi_iface_status status = {};
	int rc = ReadStatus(&status);

	char ip[NET_IPV4_ADDR_LEN] = { 0 };
	if (sta_iface_ != nullptr && sta_connected_) {
		struct net_in_addr *ipv4 = net_if_ipv4_get_global_addr(sta_iface_, NET_ADDR_PREFERRED);
		if (ipv4 != nullptr) {
			(void)net_addr_ntop(AF_INET, ipv4, ip, sizeof(ip));
		}
	}

	k_mutex_lock(&mutex_, K_FOREVER);
	if (rc == 0) {
		(void)snprintf(cached_sta_state_, sizeof(cached_sta_state_), "%s",
			       wifi_state_txt(static_cast<enum wifi_iface_state>(status.state)));
		cached_sta_rssi_ = status.rssi;
	} else {
		if (!sta_connected_) {
			(void)snprintf(cached_sta_state_, sizeof(cached_sta_state_), "%s", "DISCONNECTED");
			cached_sta_rssi_ = 0;
		}
	}

	(void)snprintf(cached_sta_ip_, sizeof(cached_sta_ip_), "%s", ip);
	k_mutex_unlock(&mutex_);
}

void WifiManager::GetSnapshot(WifiSnapshot *snapshot)
{
	if (snapshot == nullptr) {
		return;
	}

	k_mutex_lock(&mutex_, K_FOREVER);
	snapshot->ap_enabled = ap_enabled_;
	snapshot->ap_clients = ap_clients_;
	snapshot->sta_connected = sta_connected_;
	(void)snprintf(snapshot->ap_ssid, sizeof(snapshot->ap_ssid), "%s", ap_ssid_);
	(void)snprintf(snapshot->saved_ssid, sizeof(snapshot->saved_ssid), "%s", saved_ssid_);
	(void)snprintf(snapshot->sta_state, sizeof(snapshot->sta_state), "%s", cached_sta_state_);
	snapshot->sta_rssi = cached_sta_rssi_;
	(void)snprintf(snapshot->sta_ip, sizeof(snapshot->sta_ip), "%s", cached_sta_ip_);
	k_mutex_unlock(&mutex_);
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
	
	char ssid_str[WIFI_SSID_MAX_LEN + 1] = { 0 };
	size_t ssid_len = MIN(static_cast<size_t>(result->ssid_length), sizeof(ssid_str) - 1U);
	memcpy(ssid_str, result->ssid, ssid_len);
	ssid_str[ssid_len] = '\0';
	
	// Check for duplicate
	for (size_t i = 0; i < scan_count_; ++i) {
		if (strncmp(scan_results_[i].ssid, ssid_str, WIFI_SSID_MAX_LEN) == 0) {
			// Update if new result has better signal
			if (result->rssi > scan_results_[i].rssi) {
				LOG_DBG("WiFi Scan: Updated '%s' RSSI %d -> %d (ch:%d)",
					ssid_str, scan_results_[i].rssi, result->rssi, result->channel);
				scan_results_[i].rssi = result->rssi;
				scan_results_[i].channel = result->channel;
				memcpy(scan_results_[i].bssid, result->mac, 6);
			}
			return;
		}
	}
	
	// Log new found network
	const char *security_str = "unknown";
	switch (result->security) {
	case WIFI_SECURITY_TYPE_NONE: security_str = "open"; break;
	case WIFI_SECURITY_TYPE_PSK: security_str = "WPA2-PSK"; break;
	case WIFI_SECURITY_TYPE_PSK_SHA256: security_str = "WPA2-PSK-SHA256"; break;
	case WIFI_SECURITY_TYPE_SAE: security_str = "WPA3-SAE"; break;
	case WIFI_SECURITY_TYPE_WEP: security_str = "WEP"; break;
	case WIFI_SECURITY_TYPE_WPA_PSK: security_str = "WPA-PSK"; break;
	case WIFI_SECURITY_TYPE_EAP: security_str = "EAP"; break;
	default: security_str = "unknown"; break;  // 处理所有其他类型
	}
	
	LOG_INF("WiFi Scan: #%02u | %-32s | RSSI: %4d dBm | Ch: %2d | %s",
		static_cast<unsigned int>(scan_count_ + 1),
		ssid_str,
		result->rssi,
		result->channel,
		security_str);
	
	// Add new result
	WifiScanResult &entry = scan_results_[scan_count_++];
	(void)snprintf(entry.ssid, sizeof(entry.ssid), "%s", ssid_str);
	memcpy(entry.bssid, result->mac, 6);
	entry.rssi = result->rssi;
	entry.channel = result->channel;
	entry.security = result->security;
	entry.valid = true;
}

int WifiManager::StartScan()
{
	if (sta_iface_ == nullptr) {
		LOG_ERR("WiFi Scan: STA interface not available");
		return -ENODEV;
	}

	struct wifi_iface_status status = {};
	int status_rc = ReadStatus(&status);
	
	LOG_INF("========================================");
	LOG_INF("WiFi Scan: Starting...");
	LOG_INF("  Type:    Active");
	LOG_INF("  Active dwell:  100ms");
	LOG_INF("  Passive dwell: 200ms");
	if (status_rc == 0) {
		LOG_INF("  STA state: %s", wifi_state_txt(static_cast<enum wifi_iface_state>(status.state)));
		LOG_INF("  STA RSSI:  %d", status.rssi);
	} else {
		LOG_INF("  STA status query failed: %d", status_rc);
	}
	
	// Clear previous results
	scan_count_ = 0;
	scan_complete_ = false;
	scan_status_ = 0;
	memset(scan_results_, 0, sizeof(scan_results_));
	
	struct wifi_scan_params params = {};
	params.scan_type = WIFI_SCAN_TYPE_ACTIVE;
	params.dwell_time_active = 100;
	params.dwell_time_passive = 200;
	
	int rc = net_mgmt(NET_REQUEST_WIFI_SCAN, sta_iface_, &params, sizeof(params));
	if (rc != 0 && rc != -EALREADY) {
		LOG_ERR("WiFi Scan: Request failed: %d", rc);
		return rc;
	}
	
	LOG_INF("WiFi Scan: Request sent successfully");
	return 0;
}

bool WifiManager::IsScanComplete()
{
	return scan_complete_;
}

int WifiManager::GetScanStatus()
{
	return scan_status_;
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
	scan_status_ = 0;
	memset(scan_results_, 0, sizeof(scan_results_));
	k_mutex_unlock(&mutex_);
}

} // namespace fanctl
