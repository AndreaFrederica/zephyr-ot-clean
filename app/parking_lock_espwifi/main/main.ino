#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <stdarg.h>

#define DEBUG_ENABLED 0
#define DEBUG_BAUD 115200
#define UART_BAUD 115200

#define UART_RX_BUF_SIZE 384
#define UART_TX_BUF_SIZE 384
#define MQTT_PAYLOAD_BUF_SIZE 384
#define WIFI_HOSTNAME "parking-lock-gateway"

#define CONFIG_FILE_PATH "/netcfg.txt"

#define WIFI_RETRY_INTERVAL_MS 5000UL
#define MQTT_RETRY_INTERVAL_MS 5000UL
#define STATUS_REPORT_INTERVAL_MS 30000UL
#define WIFI_CONNECT_STALL_TIMEOUT_MS 15000UL

#define MQTT_TOPIC_UPLINK_RAW "parking_lock/gateway/up/raw"
#define MQTT_TOPIC_COMMAND "parking_lock/cloud/command"
#define MQTT_TOPIC_COMMAND_ACK "parking_lock/gateway/command_ack"

struct NetConfig {
  bool valid;
  char ssid[64];
  char wifi_password[64];
  char mqtt_host[64];
  uint16_t mqtt_port;
  char client_id[64];
  char mqtt_username[64];
  char mqtt_password[64];
};

static NetConfig g_cfg = {};
static WiFiClient g_wifi_client;
static PubSubClient g_mqtt(g_wifi_client);

static char g_uart_rx_line[UART_RX_BUF_SIZE];
static size_t g_uart_rx_len = 0;

static unsigned long g_last_wifi_retry_ms = 0;
static unsigned long g_last_mqtt_retry_ms = 0;
static unsigned long g_last_status_report_ms = 0;
static unsigned long g_last_wifi_begin_ms = 0;

static bool g_mqtt_was_connected = false;
static wl_status_t g_last_wifi_status = WL_IDLE_STATUS;
static uint8_t g_last_wifi_reason = 0;

static void uart_send_line(const char* line);
static void uart_send_fmt(const char* fmt, ...);
static void debug_log(const char* fmt, ...);

static void clear_config(NetConfig* cfg) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->valid = false;
  cfg->mqtt_port = 1883;
}

static bool is_field_valid(const char* s) {
  if (s == nullptr) {
    return false;
  }
  while (*s) {
    if (*s == ',' || *s == '\r' || *s == '\n') {
      return false;
    }
    ++s;
  }
  return true;
}

static bool save_config(const NetConfig* cfg) {
  File f = LittleFS.open(CONFIG_FILE_PATH, "w");
  if (!f) {
    uart_send_line("NETCFG,ERROR,flash_open");
    return false;
  }

  f.printf("%s\n", cfg->ssid);
  f.printf("%s\n", cfg->wifi_password);
  f.printf("%s\n", cfg->mqtt_host);
  f.printf("%u\n", static_cast<unsigned>(cfg->mqtt_port));
  f.printf("%s\n", cfg->client_id);
  f.printf("%s\n", cfg->mqtt_username);
  f.printf("%s\n", cfg->mqtt_password);
  f.close();
  return true;
}

static bool read_line_from_file(File& f, char* out, size_t out_size) {
  if (!f.available()) {
    return false;
  }

  size_t idx = 0;
  while (f.available()) {
    int c = f.read();
    if (c < 0) {
      break;
    }
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      break;
    }
    if (idx + 1 < out_size) {
      out[idx++] = static_cast<char>(c);
    } else {
      out[out_size - 1] = '\0';
      return false;
    }
  }
  out[idx] = '\0';
  return true;
}

static bool load_config(NetConfig* cfg) {
  clear_config(cfg);

  if (!LittleFS.exists(CONFIG_FILE_PATH)) {
    return false;
  }

  File f = LittleFS.open(CONFIG_FILE_PATH, "r");
  if (!f) {
    return false;
  }

  char mqtt_port_str[16] = {0};

  if (!read_line_from_file(f, cfg->ssid, sizeof(cfg->ssid))) return false;
  if (!read_line_from_file(f, cfg->wifi_password, sizeof(cfg->wifi_password))) return false;
  if (!read_line_from_file(f, cfg->mqtt_host, sizeof(cfg->mqtt_host))) return false;
  if (!read_line_from_file(f, mqtt_port_str, sizeof(mqtt_port_str))) return false;
  if (!read_line_from_file(f, cfg->client_id, sizeof(cfg->client_id))) return false;
  if (!read_line_from_file(f, cfg->mqtt_username, sizeof(cfg->mqtt_username))) return false;
  if (!read_line_from_file(f, cfg->mqtt_password, sizeof(cfg->mqtt_password))) return false;

  f.close();

  if (!is_field_valid(cfg->ssid) ||
      !is_field_valid(cfg->wifi_password) ||
      !is_field_valid(cfg->mqtt_host) ||
      !is_field_valid(cfg->client_id) ||
      !is_field_valid(cfg->mqtt_username) ||
      !is_field_valid(cfg->mqtt_password)) {
    clear_config(cfg);
    return false;
  }

  long port = strtol(mqtt_port_str, nullptr, 10);
  if (port <= 0 || port > 65535) {
    clear_config(cfg);
    return false;
  }

  cfg->mqtt_port = static_cast<uint16_t>(port);
  cfg->valid = true;
  return true;
}

static bool erase_config() {
  if (LittleFS.exists(CONFIG_FILE_PATH)) {
    return LittleFS.remove(CONFIG_FILE_PATH);
  }
  return true;
}

static const char* wifi_status_name(wl_status_t st) {
  switch (st) {
    case WL_CONNECTED:
      return "CONNECTED";
    case WL_NO_SSID_AVAIL:
      return "NO_SSID";
    case WL_CONNECT_FAILED:
      return "CONNECT_FAIL";
    case WL_WRONG_PASSWORD:
      return "WRONG_PASSWORD";
    case WL_DISCONNECTED:
      return "DISCONNECTED";
    case WL_IDLE_STATUS:
      return "IDLE";
    default:
      return "UNKNOWN";
  }
}

static const char* wifi_disconnect_reason_name(uint8_t reason) {
  switch (reason) {
    case WIFI_DISCONNECT_REASON_UNSPECIFIED:
      return "UNSPECIFIED";
    case WIFI_DISCONNECT_REASON_AUTH_EXPIRE:
      return "AUTH_EXPIRE";
    case WIFI_DISCONNECT_REASON_AUTH_LEAVE:
      return "AUTH_LEAVE";
    case WIFI_DISCONNECT_REASON_ASSOC_EXPIRE:
      return "ASSOC_EXPIRE";
    case WIFI_DISCONNECT_REASON_ASSOC_TOOMANY:
      return "ASSOC_TOOMANY";
    case WIFI_DISCONNECT_REASON_NOT_AUTHED:
      return "NOT_AUTHED";
    case WIFI_DISCONNECT_REASON_NOT_ASSOCED:
      return "NOT_ASSOCED";
    case WIFI_DISCONNECT_REASON_ASSOC_LEAVE:
      return "ASSOC_LEAVE";
    case WIFI_DISCONNECT_REASON_ASSOC_NOT_AUTHED:
      return "ASSOC_NOT_AUTHED";
    case WIFI_DISCONNECT_REASON_DISASSOC_PWRCAP_BAD:
      return "DISASSOC_PWRCAP_BAD";
    case WIFI_DISCONNECT_REASON_DISASSOC_SUPCHAN_BAD:
      return "DISASSOC_SUPCHAN_BAD";
    case WIFI_DISCONNECT_REASON_IE_INVALID:
      return "IE_INVALID";
    case WIFI_DISCONNECT_REASON_MIC_FAILURE:
      return "MIC_FAILURE";
    case WIFI_DISCONNECT_REASON_4WAY_HANDSHAKE_TIMEOUT:
      return "4WAY_TIMEOUT";
    case WIFI_DISCONNECT_REASON_GROUP_KEY_UPDATE_TIMEOUT:
      return "GROUP_KEY_TIMEOUT";
    case WIFI_DISCONNECT_REASON_IE_IN_4WAY_DIFFERS:
      return "IE_4WAY_DIFFERS";
    case WIFI_DISCONNECT_REASON_GROUP_CIPHER_INVALID:
      return "GROUP_CIPHER_INVALID";
    case WIFI_DISCONNECT_REASON_PAIRWISE_CIPHER_INVALID:
      return "PAIRWISE_CIPHER_INVALID";
    case WIFI_DISCONNECT_REASON_AKMP_INVALID:
      return "AKMP_INVALID";
    case WIFI_DISCONNECT_REASON_UNSUPP_RSN_IE_VERSION:
      return "UNSUPP_RSN_IE_VERSION";
    case WIFI_DISCONNECT_REASON_INVALID_RSN_IE_CAP:
      return "INVALID_RSN_IE_CAP";
    case WIFI_DISCONNECT_REASON_802_1X_AUTH_FAILED:
      return "AUTH_8021X_FAILED";
    case WIFI_DISCONNECT_REASON_CIPHER_SUITE_REJECTED:
      return "CIPHER_REJECTED";
    case WIFI_DISCONNECT_REASON_BEACON_TIMEOUT:
      return "BEACON_TIMEOUT";
    case WIFI_DISCONNECT_REASON_NO_AP_FOUND:
      return "NO_AP_FOUND";
    case WIFI_DISCONNECT_REASON_AUTH_FAIL:
      return "AUTH_FAIL";
    case WIFI_DISCONNECT_REASON_ASSOC_FAIL:
      return "ASSOC_FAIL";
    case WIFI_DISCONNECT_REASON_HANDSHAKE_TIMEOUT:
      return "HANDSHAKE_TIMEOUT";
    default:
      return "UNKNOWN";
  }
}

static const char* mqtt_status_name(bool connected) {
  return connected ? "CONNECTED" : "DISCONNECTED";
}

static const char* wifi_reason_summary(wl_status_t st, uint8_t reason, bool connecting_timed_out) {
  if (st == WL_CONNECTED) {
    return "OK";
  }
  if (st == WL_WRONG_PASSWORD) {
    return "WRONG_PASSWORD";
  }
  if (st == WL_NO_SSID_AVAIL) {
    return "NO_SSID";
  }
  if (st == WL_CONNECT_FAILED) {
    return "CONNECT_FAILED";
  }
  if (reason != 0) {
    return wifi_disconnect_reason_name(reason);
  }
  if (connecting_timed_out) {
    return "CONNECT_TIMEOUT";
  }
  if (st == WL_IDLE_STATUS) {
    return "IDLE";
  }
  if (st == WL_DISCONNECTED) {
    return "DISCONNECTED";
  }
  return "UNKNOWN";
}

static void publish_raw_line(const char* line) {
  if (!g_mqtt.connected()) {
    return;
  }
  g_mqtt.publish(MQTT_TOPIC_UPLINK_RAW, line, false);
}

static void publish_python_ack(const char* command_id, const char* status, const char* detail) {
  if (!g_mqtt.connected()) {
    return;
  }
  if (command_id == nullptr || command_id[0] == '\0') {
    return;
  }

  char line[160];
  snprintf(line, sizeof(line), "ACK,%s,%s,%s", command_id, status, detail);
  g_mqtt.publish(MQTT_TOPIC_COMMAND_ACK, line, false);
}

static void format_link_status(char* out, size_t out_size, const char* trigger) {
  wl_status_t st = WiFi.status();
  bool timed_out = false;
  if (g_last_wifi_begin_ms != 0 && st != WL_CONNECTED) {
    timed_out = (millis() - g_last_wifi_begin_ms) >= WIFI_CONNECT_STALL_TIMEOUT_MS;
  }

  if (st == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    snprintf(out, out_size,
             "LINK,%s,WIFI,%s,%u.%u.%u.%u,REASON,%s,MQTT,%s",
             trigger,
             wifi_status_name(st),
             ip[0], ip[1], ip[2], ip[3],
             wifi_reason_summary(st, g_last_wifi_reason, timed_out),
             mqtt_status_name(g_mqtt.connected()));
  } else {
    snprintf(out, out_size,
             "LINK,%s,WIFI,%s,REASON,%s,RID,%u,MQTT,%s",
             trigger,
             wifi_status_name(st),
             wifi_reason_summary(st, g_last_wifi_reason, timed_out),
             static_cast<unsigned>(g_last_wifi_reason),
             mqtt_status_name(g_mqtt.connected()));
  }
}

static void mqtt_publish_link_status(const char* trigger) {
  if (!g_mqtt.connected()) {
    return;
  }

  char line[192];
  format_link_status(line, sizeof(line), trigger);
  g_mqtt.publish(MQTT_TOPIC_UPLINK_RAW, line, false);
}

static void uart_report_link_status(const char* trigger) {
  char line[192];
  format_link_status(line, sizeof(line), trigger);
  uart_send_line(line);
}

static void report_link_status(const char* trigger) {
  uart_report_link_status(trigger);
  mqtt_publish_link_status(trigger);
}

static void wifi_disconnect_now() {
  WiFi.disconnect(true);
}

static void mqtt_disconnect_now() {
  if (g_mqtt.connected()) {
    g_mqtt.disconnect();
  }
}

static void reconnect_pipeline_reset() {
  mqtt_disconnect_now();
  wifi_disconnect_now();
}

static void report_netcfg_state_loaded() {
  uart_send_line("NETCFG,LOADED");
  publish_raw_line("NETCFG,LOADED");
}

static void report_netcfg_state_empty() {
  uart_send_line("NETCFG,EMPTY");
  publish_raw_line("NETCFG,EMPTY");
}

static void report_netcfg_state_saved() {
  uart_send_line("NETCFG,SAVED");
  publish_raw_line("NETCFG,SAVED");
}

static void report_netcfg_state_cleared() {
  uart_send_line("NETCFG,CLEARED");
  publish_raw_line("NETCFG,CLEARED");
}

static void report_netcfg_error(const char* code) {
  uart_send_fmt("NETCFG,ERROR,%s", code);
  char buf[96];
  snprintf(buf, sizeof(buf), "NETCFG,ERROR,%s", code);
  publish_raw_line(buf);
}

static void report_info_esp(const char* detail) {
  uart_send_fmt("INFO,ESP,%s", detail);
  char buf[128];
  snprintf(buf, sizeof(buf), "INFO,ESP,%s", detail);
  publish_raw_line(buf);
}

static void report_err_esp(const char* detail) {
  uart_send_fmt("ERR,ESP,%s", detail);
  char buf[128];
  snprintf(buf, sizeof(buf), "ERR,ESP,%s", detail);
  publish_raw_line(buf);
}

static void report_wifi_connecting() {
  uart_send_line("WIFI,CONNECTING");
  publish_raw_line("WIFI,CONNECTING");
}

static void report_mqtt_connecting() {
  uart_send_line("MQTT,CONNECTING");
  publish_raw_line("MQTT,CONNECTING");
}

static size_t split_csv_inplace(char* line, char* fields[], size_t max_fields) {
  size_t count = 0;
  char* p = line;

  while (*p != '\0' && count < max_fields) {
    fields[count++] = p;
    char* comma = strchr(p, ',');
    if (comma == nullptr) {
      break;
    }
    *comma = '\0';
    p = comma + 1;
  }

  return count;
}

static bool parse_python_json_command(const char* json_buf, char* cmd_id, size_t cmd_id_size,
                                      char* serial_line, size_t serial_line_size) {
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, json_buf);
  if (err) {
    return false;
  }

  const char* id = doc["command_id"] | "";
  const char* action = doc["action"] | "";
  int node_id = doc["node_id"] | -1;

  if (!is_field_valid(id) || !is_field_valid(action)) {
    return false;
  }
  if (id[0] == '\0' || action[0] == '\0' || node_id < 0) {
    return false;
  }

  snprintf(cmd_id, cmd_id_size, "%s", id);

  if (strcasecmp(action, "LOCK") == 0) {
    if (!doc.containsKey("value")) {
      return false;
    }
    int value = doc["value"].as<int>();
    if (!(value == 0 || value == 1)) {
      return false;
    }
    snprintf(serial_line, serial_line_size, "CMD,%s,LOCK,%d,%d", cmd_id, node_id, value);
    return true;
  }

  if (strcasecmp(action, "GET") == 0) {
    snprintf(serial_line, serial_line_size, "CMD,%s,GET,%d", cmd_id, node_id);
    return true;
  }

  return false;
}

static bool parse_python_text_command(char* payload, char* cmd_id, size_t cmd_id_size,
                                      char* serial_line, size_t serial_line_size) {
  char* fields[5] = {0};
  size_t count = split_csv_inplace(payload, fields, 5);
  if (count < 4) {
    return false;
  }

  if (strcmp(fields[0], "CMD") != 0) {
    return false;
  }
  if (!is_field_valid(fields[1]) || !is_field_valid(fields[2]) || !is_field_valid(fields[3])) {
    return false;
  }

  snprintf(cmd_id, cmd_id_size, "%s", fields[1]);

  if (strcasecmp(fields[2], "LOCK") == 0) {
    if (count != 5 || !is_field_valid(fields[4])) {
      return false;
    }
    long node_id = strtol(fields[3], nullptr, 10);
    long value = strtol(fields[4], nullptr, 10);
    if (node_id < 0 || !(value == 0 || value == 1)) {
      return false;
    }
    snprintf(serial_line, serial_line_size, "CMD,%s,LOCK,%ld,%ld", cmd_id, node_id, value);
    return true;
  }

  if (strcasecmp(fields[2], "GET") == 0) {
    if (count != 4) {
      return false;
    }
    long node_id = strtol(fields[3], nullptr, 10);
    if (node_id < 0) {
      return false;
    }
    snprintf(serial_line, serial_line_size, "CMD,%s,GET,%ld", cmd_id, node_id);
    return true;
  }

  return false;
}

static void mqtt_callback(char* topic, uint8_t* payload, unsigned int length) {
  if (topic == nullptr || payload == nullptr || length == 0) {
    return;
  }

  if (strcmp(topic, MQTT_TOPIC_COMMAND) != 0) {
    return;
  }

  if (length >= MQTT_PAYLOAD_BUF_SIZE) {
    report_err_esp("mqtt_payload_too_long");
    return;
  }

  char rx_buf[MQTT_PAYLOAD_BUF_SIZE];
  memcpy(rx_buf, payload, length);
  rx_buf[length] = '\0';

  char cmd_id[64] = {0};
  char serial_line[128] = {0};
  bool ok = false;

  if (strncmp(rx_buf, "CMD,", 4) == 0) {
    ok = parse_python_text_command(rx_buf, cmd_id, sizeof(cmd_id), serial_line, sizeof(serial_line));
  } else {
    ok = parse_python_json_command(rx_buf, cmd_id, sizeof(cmd_id), serial_line, sizeof(serial_line));
  }

  if (!ok) {
    report_err_esp("mqtt_bad_cmd");
    publish_python_ack(cmd_id, "failed", "bad_command");
    return;
  }

  uart_send_line(serial_line);
  publish_python_ack(cmd_id, "sent", "serial_written");
}

static void mqtt_setup_client() {
  g_mqtt.setClient(g_wifi_client);
  g_mqtt.setServer(g_cfg.mqtt_host, g_cfg.mqtt_port);
  g_mqtt.setCallback(mqtt_callback);
  g_mqtt.setBufferSize(MQTT_PAYLOAD_BUF_SIZE);
}

static void on_wifi_disconnect(const WiFiEventStationModeDisconnected& event) {
  g_last_wifi_reason = event.reason;
  report_link_status("wifi_disconnected");
}

static void on_wifi_got_ip(const WiFiEventStationModeGotIP&) {
  g_last_wifi_reason = 0;
  report_link_status("wifi_got_ip");
}

static void wifi_begin_if_needed() {
  if (!g_cfg.valid) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  unsigned long now = millis();
  if (now - g_last_wifi_retry_ms < WIFI_RETRY_INTERVAL_MS) {
    return;
  }
  g_last_wifi_retry_ms = now;
  g_last_wifi_begin_ms = now;

  report_wifi_connecting();
  WiFi.mode(WIFI_STA);
  WiFi.hostname(WIFI_HOSTNAME);
  WiFi.begin(g_cfg.ssid, g_cfg.wifi_password);
}

static void mqtt_begin_if_needed() {
  if (!g_cfg.valid) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (g_mqtt.connected()) {
    return;
  }

  unsigned long now = millis();
  if (now - g_last_mqtt_retry_ms < MQTT_RETRY_INTERVAL_MS) {
    return;
  }
  g_last_mqtt_retry_ms = now;

  report_mqtt_connecting();

  bool ok = false;
  if (g_cfg.mqtt_username[0] != '\0') {
    ok = g_mqtt.connect(g_cfg.client_id, g_cfg.mqtt_username, g_cfg.mqtt_password);
  } else {
    ok = g_mqtt.connect(g_cfg.client_id);
  }

  if (ok) {
    g_mqtt.subscribe(MQTT_TOPIC_COMMAND);
    publish_raw_line("MQTT,CONNECTED");
    report_link_status("mqtt_up");
  } else {
    publish_raw_line("MQTT,DISCONNECTED,connect_fail");
    report_link_status("mqtt_fail");
  }
}

static void handle_connectivity() {
  wl_status_t current_wifi_status = WiFi.status();
  if (current_wifi_status != g_last_wifi_status) {
    report_link_status("wifi_change");
    g_last_wifi_status = current_wifi_status;
    if (current_wifi_status == WL_CONNECTED) {
      g_last_wifi_begin_ms = 0;
    }
  }

  if (!g_cfg.valid) {
    return;
  }

  wifi_begin_if_needed();

  if (WiFi.status() == WL_CONNECTED) {
    mqtt_begin_if_needed();
    g_mqtt.loop();
  } else if (g_last_wifi_begin_ms != 0 && millis() - g_last_wifi_begin_ms >= WIFI_CONNECT_STALL_TIMEOUT_MS) {
    report_link_status("wifi_timeout");
    g_last_wifi_begin_ms = millis();
  }

  bool mqtt_now_connected = g_mqtt.connected();
  if (g_mqtt_was_connected && !mqtt_now_connected) {
    report_link_status("mqtt_lost");
  }
  g_mqtt_was_connected = mqtt_now_connected;
}

static void process_netcfg_command(char* line) {
  char* fields[8] = {0};
  size_t count = split_csv_inplace(line, fields, 8);

  if (count != 8) {
    report_netcfg_error("bad_field_count");
    return;
  }

  NetConfig cfg;
  clear_config(&cfg);

  if (!is_field_valid(fields[1]) ||
      !is_field_valid(fields[2]) ||
      !is_field_valid(fields[3]) ||
      !is_field_valid(fields[4]) ||
      !is_field_valid(fields[5]) ||
      !is_field_valid(fields[6]) ||
      !is_field_valid(fields[7])) {
    report_netcfg_error("bad_field");
    return;
  }

  if (fields[1][0] == '\0' || fields[3][0] == '\0' || fields[5][0] == '\0') {
    report_netcfg_error("bad_field");
    return;
  }

  long port = strtol(fields[4], nullptr, 10);
  if (port <= 0 || port > 65535) {
    report_netcfg_error("bad_port");
    return;
  }

  snprintf(cfg.ssid, sizeof(cfg.ssid), "%s", fields[1]);
  snprintf(cfg.wifi_password, sizeof(cfg.wifi_password), "%s", fields[2]);
  snprintf(cfg.mqtt_host, sizeof(cfg.mqtt_host), "%s", fields[3]);
  cfg.mqtt_port = static_cast<uint16_t>(port);
  snprintf(cfg.client_id, sizeof(cfg.client_id), "%s", fields[5]);
  snprintf(cfg.mqtt_username, sizeof(cfg.mqtt_username), "%s", fields[6]);
  snprintf(cfg.mqtt_password, sizeof(cfg.mqtt_password), "%s", fields[7]);
  cfg.valid = true;

  if (!save_config(&cfg)) {
    report_netcfg_error("flash_write");
    return;
  }

  g_cfg = cfg;
  mqtt_setup_client();

  report_netcfg_state_saved();
  reconnect_pipeline_reset();
}

static void process_netclr_command() {
  if (!erase_config()) {
    report_netcfg_error("flash_erase");
    return;
  }

  clear_config(&g_cfg);
  mqtt_disconnect_now();
  wifi_disconnect_now();
  g_last_wifi_reason = 0;
  g_last_wifi_begin_ms = 0;
  report_netcfg_state_cleared();
  uart_report_link_status("netclr");
}

static void process_netstat_command() {
  if (!g_cfg.valid) {
    uart_send_line("NETCFG,EMPTY");
    return;
  }

  uart_send_line("NETCFG,LOADED");
  report_link_status("netstat");
}

static void process_uart_line(char* line) {
  if (line == nullptr || line[0] == '\0') {
    return;
  }

  debug_log("UART RX: %s", line);

  if (strcmp(line, "NETCLR") == 0) {
    process_netclr_command();
    return;
  }

  if (strcmp(line, "NETSTAT") == 0) {
    process_netstat_command();
    return;
  }

  if (strcmp(line, "READY,GATEWAY,1") == 0) {
    report_info_esp("gateway_ready_seen");
    return;
  }

  if (strcmp(line, "PONG,GATEWAY") == 0) {
    report_info_esp("pong_gateway");
    return;
  }

  if (strncmp(line, "NETCFG,", 7) == 0) {
    process_netcfg_command(line);
    return;
  }

  if (strncmp(line, "REPORT,", 7) == 0) {
    publish_raw_line(line);
    return;
  }

  if (strncmp(line, "OFFLINE,", 8) == 0) {
    publish_raw_line(line);
    return;
  }

  if (strncmp(line, "ACK,", 4) == 0) {
    if (g_mqtt.connected()) {
      g_mqtt.publish(MQTT_TOPIC_COMMAND_ACK, line, false);
    }
    publish_raw_line(line);
    return;
  }

  report_err_esp("unknown_uart_frame");
}

static void poll_uart() {
  while (Serial.available() > 0) {
    int ch = Serial.read();
    if (ch < 0) {
      break;
    }

    char c = static_cast<char>(ch);

    if (c == '\r') {
      continue;
    }

    if (c == '\n') {
      g_uart_rx_line[g_uart_rx_len] = '\0';
      if (g_uart_rx_len > 0) {
        process_uart_line(g_uart_rx_line);
      }
      g_uart_rx_len = 0;
      continue;
    }

    if (g_uart_rx_len + 1 < sizeof(g_uart_rx_line)) {
      g_uart_rx_line[g_uart_rx_len++] = c;
    } else {
      g_uart_rx_len = 0;
      report_err_esp("uart_line_too_long");
    }
  }
}

static void periodic_tasks() {
  unsigned long now = millis();
  if (now - g_last_status_report_ms >= STATUS_REPORT_INTERVAL_MS) {
    g_last_status_report_ms = now;
    if (g_cfg.valid) {
      report_link_status("periodic");
    }
  }
}

static void uart_send_line(const char* line) {
  if (line == nullptr) {
    return;
  }
  Serial.print(line);
  Serial.print("\r\n");
  debug_log("UART TX: %s", line);
}

static void uart_send_fmt(const char* fmt, ...) {
  char buf[UART_TX_BUF_SIZE];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  uart_send_line(buf);
}

static void debug_log(const char* fmt, ...) {
#if DEBUG_ENABLED
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.println(buf);
#else
  (void)fmt;
#endif
}

void setup() {
  Serial.begin(UART_BAUD);
  delay(200);

  clear_config(&g_cfg);
  g_last_wifi_status = WiFi.status();
  g_last_wifi_reason = 0;

  WiFi.persistent(false);
  WiFi.onStationModeDisconnected(on_wifi_disconnect);
  WiFi.onStationModeGotIP(on_wifi_got_ip);

  if (!LittleFS.begin()) {
    uart_send_line("READY,ESP,1");
    uart_send_line("ERR,ESP,fs_mount_fail");
    return;
  }

  uart_send_line("READY,ESP,1");
  report_info_esp("boot_start");

  if (load_config(&g_cfg)) {
    mqtt_setup_client();
    report_netcfg_state_loaded();
  } else {
    report_netcfg_state_empty();
  }

  uart_report_link_status("boot");
  report_info_esp("boot_complete");
}

void loop() {
  poll_uart();
  handle_connectivity();
  periodic_tasks();
}
