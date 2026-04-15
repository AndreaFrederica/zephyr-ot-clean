#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

namespace {
#define RS485_NODE DT_ALIAS(rs485_uart)
#define SCREEN_NODE DT_ALIAS(screen_uart)
#define WIFI_NODE DT_ALIAS(wifi_uart)

constexpr size_t RX_BUF_SIZE = 192;
constexpr int POLL_DELAY_MS = 10;
constexpr int POLL_INTERVAL_MS = 200;
constexpr int DISCOVERY_INTERVAL_MS = 1000;
constexpr int RS485_TX_INTERVAL_MS = 50;
constexpr uint32_t RS485_IDLE_GUARD_MS = 4;
constexpr uint32_t RS485_IDLE_WAIT_TIMEOUT_MS = 40;
constexpr int NODE_ADDR_MAX = 64;
constexpr size_t NODE_UID_MAX = 2 * 8 + 1;
constexpr int PARTS_MAX = 24;
constexpr int RX_MSGQ_DEPTH = 8;
constexpr int RS485_CMD_MSGQ_DEPTH = 8;
constexpr int NODE_OFFLINE_TIMEOUT_MS = 15000;

const device *const rs485 = DEVICE_DT_GET(RS485_NODE);
const device *const screen = DEVICE_DT_GET(SCREEN_NODE);
const device *const wifi = DEVICE_DT_GET(WIFI_NODE);

struct NodeState {
	int id;
	bool online;
	int64_t lastSeen;
	int32_t tempCenti;
	int32_t humiCenti;
	int flameDigital;
	int flameMv;
	int lockState;
};

struct UartRxContext {
	char lineBuf[RX_BUF_SIZE];
	size_t pos;
	struct k_msgq *msgq;
};

enum class Rs485Action : uint8_t {
	Get = 0,
	Lock = 1,
};

struct Rs485Cmd {
	Rs485Action action;
	int nodeId;
	int value;
	bool needAck;
	char commandId[24];
};

NodeState states[NODE_ADDR_MAX];
char registered_uid[NODE_ADDR_MAX][NODE_UID_MAX];

K_MSGQ_DEFINE(rs485_msgq, RX_BUF_SIZE, RX_MSGQ_DEPTH, 4);
K_MSGQ_DEFINE(screen_msgq, RX_BUF_SIZE, RX_MSGQ_DEPTH, 4);
K_MSGQ_DEFINE(wifi_msgq, RX_BUF_SIZE, RX_MSGQ_DEPTH, 4);
K_MSGQ_DEFINE(rs485_cmd_msgq, sizeof(Rs485Cmd), RS485_CMD_MSGQ_DEPTH, 4);

UartRxContext rs485Rx = {{0}, 0, &rs485_msgq};
UartRxContext screenRx = {{0}, 0, &screen_msgq};
UartRxContext wifiRx = {{0}, 0, &wifi_msgq};
atomic_t rs485_last_activity_ms;

void uart_send(const device *dev, const char *text)
{
	size_t length = strlen(text);

	for (size_t i = 0; i < length; ++i) {
		uart_poll_out(dev, (uint8_t)text[i]);
	}
}

bool rs485_send_when_idle(const char *text)
{
	if (text == nullptr || text[0] == '\0') {
		return false;
	}

	uint32_t deadline = k_uptime_get_32() + RS485_IDLE_WAIT_TIMEOUT_MS;
	while (true) {
		uint32_t now = k_uptime_get_32();
		uint32_t last = (uint32_t)atomic_get(&rs485_last_activity_ms);
		uint32_t idleFor = now - last;

		if (idleFor >= RS485_IDLE_GUARD_MS) {
			uart_send(rs485, text);
			atomic_set(&rs485_last_activity_ms, (atomic_val_t)k_uptime_get_32());
			return true;
		}

		if ((int32_t)(deadline - now) <= 0) {
			return false;
		}

		k_msleep(1);
	}
}

void uart_send_line(const device *dev, const char *text)
{
	uart_send(dev, text);
	uart_send(dev, "\r\n");
}

bool copy_string(char *dst, size_t dstLen, const char *src)
{
	size_t len = strlen(src);
	if (len >= dstLen) {
		return false;
	}

	memcpy(dst, src, len + 1U);
	return true;
}

bool parse_int(const char *text, int *value)
{
	char *end = nullptr;
	long parsed = strtol(text, &end, 10);

	if (end == nullptr || *end != '\0') {
		return false;
	}

	*value = (int)parsed;
	return true;
}

bool starts_with(const char *text, const char *prefix)
{
	return strncmp(text, prefix, strlen(prefix)) == 0;
}

bool is_valid_node_id(int nodeId)
{
	return nodeId >= 1 && nodeId <= NODE_ADDR_MAX;
}

int split_csv(char *line, char *parts[PARTS_MAX])
{
	int count = 0;
	char *cursor = line;

	for (int i = 0; i < PARTS_MAX; ++i) {
		parts[i] = nullptr;
	}

	while (count < PARTS_MAX) {
		parts[count++] = cursor;
		char *comma = strchr(cursor, ',');
		if (comma == nullptr) {
			break;
		}
		*comma = '\0';
		cursor = comma + 1;
	}

	return count;
}

NodeState *findNode(int id)
{
	if (!is_valid_node_id(id)) {
		return nullptr;
	}

	return &states[id - 1];
}

void init_states()
{
	for (int i = 0; i < NODE_ADDR_MAX; ++i) {
		states[i].id = i + 1;
		states[i].online = false;
		states[i].lastSeen = 0;
		states[i].tempCenti = 0;
		states[i].humiCenti = 0;
		states[i].flameDigital = 0;
		states[i].flameMv = 0;
		states[i].lockState = 0;
		registered_uid[i][0] = '\0';
	}
}

bool is_addr_registered(int addr)
{
	if (!is_valid_node_id(addr)) {
		return false;
	}

	return registered_uid[addr - 1][0] != '\0';
}

int find_addr_by_uid(const char *uid)
{
	if (uid == nullptr || uid[0] == '\0') {
		return -1;
	}

	for (int i = 0; i < NODE_ADDR_MAX; ++i) {
		if (registered_uid[i][0] != '\0' && strcmp(registered_uid[i], uid) == 0) {
			return i + 1;
		}
	}

	return -1;
}

int allocate_first_free_addr()
{
	for (int i = 0; i < NODE_ADDR_MAX; ++i) {
		if (registered_uid[i][0] == '\0') {
			return i + 1;
		}
	}

	return -1;
}

bool register_uid_addr(const char *uid, int addr)
{
	if (!is_valid_node_id(addr) || uid == nullptr || uid[0] == '\0') {
		return false;
	}

	if (registered_uid[addr - 1][0] != '\0' && strcmp(registered_uid[addr - 1], uid) != 0) {
		return false;
	}

	return copy_string(registered_uid[addr - 1], sizeof(registered_uid[addr - 1]), uid);
}

int ensure_addr_for_uid(const char *uid)
{
	int existing = find_addr_by_uid(uid);
	if (existing > 0) {
		return existing;
	}

	int addr = allocate_first_free_addr();
	if (addr <= 0) {
		return -1;
	}

	if (!register_uid_addr(uid, addr)) {
		return -1;
	}

	return addr;
}

int find_next_registered_addr(int cursor)
{
	if (cursor < 1 || cursor > NODE_ADDR_MAX) {
		cursor = 1;
	}

	for (int i = 0; i < NODE_ADDR_MAX; ++i) {
		int addr = ((cursor - 1 + i) % NODE_ADDR_MAX) + 1;
		if (is_addr_registered(addr)) {
			return addr;
		}
	}

	return -1;
}

void forward_to_screen(const char *payload)
{
	uart_send_line(screen, payload);
	/* Force terminate any potential incomplete Nextion packet so subsequent
	   screen_set_text commands are parsed correctly. */
	uart_poll_out(screen, 0xFF);
	uart_poll_out(screen, 0xFF);
	uart_poll_out(screen, 0xFF);
}

void screen_send_nextion_cmd(const char *cmd)
{
	uart_send(screen, cmd);
	uart_poll_out(screen, 0xFF);
	uart_poll_out(screen, 0xFF);
	uart_poll_out(screen, 0xFF);
}

void screen_set_number(const char *target, int value)
{
	char out[48];
	snprintk(out, sizeof(out), "%s=%d", target, value);
	screen_send_nextion_cmd(out);
}

void screen_set_text(const char *target, const char *value)
{
	char out[64];
	snprintk(out, sizeof(out), "%s=\"%s\"", target, value);
	screen_send_nextion_cmd(out);
}

void screen_publish_latest_snapshot(const NodeState *state)
{
	if (state == nullptr) {
		return;
	}

	char buf[48];

	/* Temp */
	snprintk(buf, sizeof(buf), "%dC", state->tempCenti / 100);
	screen_set_text("home.T.txt", buf);

	/* Humi */
	snprintk(buf, sizeof(buf), "%d%%", state->humiCenti / 100);
	screen_set_text("home.H.txt", buf);

	/* Flame digital: N (normal) or A (alarm) */
	const char* fireState = (state->flameDigital == 0) ? "A" : "N";
	snprintk(buf, sizeof(buf), "%s", fireState);
	screen_set_text("home.FD.txt", buf);

	/* Flame risk percent: 0% = safe, 100% = high risk */
	int flameRiskPct = 0;
	if (state->flameMv >= 3300) {
		flameRiskPct = 0;
	} else if (state->flameMv <= 0) {
		flameRiskPct = 100;
	} else {
		flameRiskPct = ((3300 - state->flameMv) * 100 + 1650) / 3300;
	}
	if (flameRiskPct < 0) flameRiskPct = 0;
	if (flameRiskPct > 100) flameRiskPct = 100;
	snprintk(buf, sizeof(buf), "%d%%", flameRiskPct);
	screen_set_text("home.FA.txt", buf);

	/* Lock state */
	const char* lockStateText = (state->lockState == 1) ? "LCK"
								: ((state->lockState == 0) ? "ULCK" : "-");
	screen_set_text("home.L.txt", lockStateText);
}

void screen_publish_link_status(const char *line)
{
	/* Support both old single-line format (WIFI,CONNECTED) and ESP32 LINK format
	   (LINK,trigger,WIFI,CONNECTED,192.168.x.x,MQTT,CONNECTED). */
	const char* wifi_pos = strstr(line, "WIFI,");
	if (wifi_pos != nullptr) {
		wifi_pos += 5;
		if (strncmp(wifi_pos, "CONNECTED", 9) == 0) {
			screen_set_text("home.ws.txt", "ON");
			screen_set_text("home.ss.txt", "ON");
			screen_set_text("page0.ws.txt", "ON");
			screen_set_text("page0.ss.txt", "ON");
		} else if (strncmp(wifi_pos, "CONNECTING", 10) == 0) {
			screen_set_text("home.ws.txt", "...");
			screen_set_text("home.ss.txt", "...");
			screen_set_text("page0.ws.txt", "...");
			screen_set_text("page0.ss.txt", "...");
		} else {
			screen_set_text("home.ws.txt", "OFF");
			screen_set_text("home.ss.txt", "OFF");
			screen_set_text("page0.ws.txt", "OFF");
			screen_set_text("page0.ss.txt", "OFF");
		}
	}

	const char* mqtt_pos = strstr(line, "MQTT,");
	if (mqtt_pos != nullptr) {
		mqtt_pos += 5;
		if (strncmp(mqtt_pos, "CONNECTED", 9) == 0) {
			screen_set_text("home.ms.txt", "ON");
			screen_set_text("home.ss.txt", "ON");
			screen_set_text("page0.ms.txt", "ON");
			screen_set_text("page0.ss.txt", "ON");
		} else if (strncmp(mqtt_pos, "CONNECTING", 10) == 0) {
			screen_set_text("home.ms.txt", "...");
			screen_set_text("home.ss.txt", "...");
			screen_set_text("page0.ms.txt", "...");
			screen_set_text("page0.ss.txt", "...");
		} else {
			screen_set_text("home.ms.txt", "OFF");
			screen_set_text("home.ss.txt", "OFF");
			screen_set_text("page0.ms.txt", "OFF");
			screen_set_text("page0.ss.txt", "OFF");
		}
	}
}

void forward_to_wifi(const char *payload)
{
	uart_send_line(wifi, payload);
}

void forward_upstream(const char *payload)
{
	forward_to_screen(payload);
	forward_to_wifi(payload);
}

void send_wifi_ack(const char *commandId, const char *status, const char *detail)
{
	if (commandId == nullptr || commandId[0] == '\0') {
		return;
	}

	char out[96];
	snprintk(out, sizeof(out), "ACK,%s,%s,%s", commandId, status, detail);
	forward_to_wifi(out);
}

void parse_telemetry(NodeState *state, char *parts[PARTS_MAX], int count)
{
	for (int i = 3; i < count; ++i) {
		if (strncmp(parts[i], "T=", 2) == 0) {
			int tInt = 0;
			int tFrac = 0;
			if (sscanf(parts[i] + 2, "%d.%d", &tInt, &tFrac) == 2) {
				state->tempCenti = (tInt * 100) + tFrac;
			}
		} else if (strncmp(parts[i], "H=", 2) == 0) {
			int hInt = 0;
			int hFrac = 0;
			if (sscanf(parts[i] + 2, "%d.%d", &hInt, &hFrac) == 2) {
				state->humiCenti = (hInt * 100) + hFrac;
			}
		} else if (strncmp(parts[i], "FD=", 3) == 0) {
			int value = 0;
			if (parse_int(parts[i] + 3, &value)) {
				state->flameDigital = value;
			}
		} else if (strncmp(parts[i], "FA=", 3) == 0) {
			int value = 0;
			if (parse_int(parts[i] + 3, &value)) {
				state->flameMv = value;
			}
		} else if (strncmp(parts[i], "L=", 2) == 0) {
			int value = 0;
			if (parse_int(parts[i] + 2, &value)) {
				state->lockState = value;
			}
		}
	}
}

void process_reg_frame(char *parts[PARTS_MAX], int count, const char *rawLine);

void process_rs485_line(char *line)
{
	char rawLine[RX_BUF_SIZE];
	if (!copy_string(rawLine, sizeof(rawLine), line)) {
		return;
	}

	char *parts[PARTS_MAX];
	int count = split_csv(line, parts);
	if (count < 2 || parts[0] == nullptr) {
		return;
	}

	if (strcmp(parts[0], "REG") == 0) {
		process_reg_frame(parts, count, rawLine);
		return;
	}

	if (count < 3) {
		return;
	}

	int nodeId = 0;
	if (!parse_int(parts[1], &nodeId)) {
		return;
	}

	auto *state = findNode(nodeId);
	if (state == nullptr) {
		return;
	}

	state->online = true;
	state->lastSeen = k_uptime_get();
	if (strcmp(parts[0], "REPORT") == 0) {
		parse_telemetry(state, parts, count);
	}

	forward_upstream(rawLine);
	screen_publish_latest_snapshot(state);
}

bool send_poll(int nodeId)
{
	char out[32];

	snprintk(out, sizeof(out), "REQ,%d,GET\r\n", nodeId);
	return rs485_send_when_idle(out);
}

bool send_discovery_probe()
{
	return rs485_send_when_idle("REQ,0,DISCOVER\r\n");
}

bool send_set_lock(int nodeId, int value)
{
	char out[32];

	snprintk(out, sizeof(out), "SET,%d,LOCK,%d\r\n", nodeId, value);
	return rs485_send_when_idle(out);
}

bool send_assign_addr(const char *uid, int addr)
{
	if (uid == nullptr || uid[0] == '\0' || !is_valid_node_id(addr)) {
		return false;
	}

	char out[64];
	snprintk(out, sizeof(out), "SET,0,ADDR,%s,%d\r\n", uid, addr);
	return rs485_send_when_idle(out);
}

void process_reg_frame(char *parts[PARTS_MAX], int count, const char *rawLine)
{
	forward_upstream(rawLine);

	if (count >= 3 && strcmp(parts[1], "REQ") == 0 && starts_with(parts[2], "UID=")) {
		const char *uid = parts[2] + 4;
		int addr = ensure_addr_for_uid(uid);
		if (addr <= 0) {
			forward_upstream("REG,NACK,NOADDR");
			return;
		}

		if (send_assign_addr(uid, addr)) {
			char out[64];
			snprintk(out, sizeof(out), "REG,ASSIGN,%d,UID=%s", addr, uid);
			forward_upstream(out);
		}
		return;
	}

	if (count >= 4 && strcmp(parts[1], "ACK") == 0 && starts_with(parts[3], "UID=")) {
		int addr = 0;
		if (!parse_int(parts[2], &addr) || !is_valid_node_id(addr)) {
			return;
		}

		const char *uid = parts[3] + 4;
		if (!register_uid_addr(uid, addr)) {
			return;
		}

		auto *state = findNode(addr);
		if (state != nullptr) {
			state->online = true;
			state->lastSeen = k_uptime_get();
		}
	}
}

bool execute_gateway_command(const char *action, const char *nodeText, const char *valueText, const char *commandId)
{
	int nodeId = 0;
	int value = 0;
	Rs485Cmd cmd = {};

	if (!parse_int(nodeText, &nodeId) || !is_valid_node_id(nodeId)) {
		send_wifi_ack(commandId, "failed", "bad_node");
		return true;
	}

	if (!is_addr_registered(nodeId)) {
		send_wifi_ack(commandId, "failed", "bad_node");
		return true;
	}

	if (strcmp(action, "LOCK") == 0) {
		if (valueText == nullptr || !parse_int(valueText, &value) || (value != 0 && value != 1)) {
			send_wifi_ack(commandId, "failed", "bad_value");
			return true;
		}
		cmd.action = Rs485Action::Lock;
		cmd.nodeId = nodeId;
		cmd.value = value;
		cmd.needAck = (commandId != nullptr && commandId[0] != '\0');
		snprintk(cmd.commandId, sizeof(cmd.commandId), "%s", commandId ? commandId : "");
		if (k_msgq_put(&rs485_cmd_msgq, &cmd, K_NO_WAIT) != 0) {
			send_wifi_ack(commandId, "failed", "queue_full");
			return true;
		}
		return true;
	}

	if (strcmp(action, "GET") == 0) {
		cmd.action = Rs485Action::Get;
		cmd.nodeId = nodeId;
		cmd.value = 0;
		cmd.needAck = (commandId != nullptr && commandId[0] != '\0');
		snprintk(cmd.commandId, sizeof(cmd.commandId), "%s", commandId ? commandId : "");
		if (k_msgq_put(&rs485_cmd_msgq, &cmd, K_NO_WAIT) != 0) {
			send_wifi_ack(commandId, "failed", "queue_full");
			return true;
		}
		return true;
	}

	send_wifi_ack(commandId, "failed", "bad_action");
	return true;
}

void process_screen_line(char *line)
{
	char rawLine[RX_BUF_SIZE];
	if (!copy_string(rawLine, sizeof(rawLine), line)) {
		return;
	}

	char *parts[PARTS_MAX];
	int count = split_csv(line, parts);
	if (count <= 0 || parts[0] == nullptr) {
		return;
	}

	if (count == 3 && strcmp(parts[0], "LOCK") == 0) {
		(void)execute_gateway_command(parts[0], parts[1], parts[2], "");
		return;
	}

	if (count == 2 && strcmp(parts[0], "GET") == 0) {
		(void)execute_gateway_command(parts[0], parts[1], nullptr, "");
		return;
	}

	if (starts_with(rawLine, "NETCFG,") || strcmp(rawLine, "NETCLR") == 0 ||
	    strcmp(rawLine, "NETSTAT") == 0) {
		forward_to_wifi(rawLine);
	}
}

bool is_wifi_status_line(const char *line)
{
	return starts_with(line, "READY,") || starts_with(line, "NETCFG,") || starts_with(line, "WIFI,") ||
	       starts_with(line, "MQTT,") || starts_with(line, "LINK,") || starts_with(line, "INFO,") ||
	       starts_with(line, "ERR,");
}

void process_wifi_line(char *line)
{
	char rawLine[RX_BUF_SIZE];
	if (!copy_string(rawLine, sizeof(rawLine), line)) {
		return;
	}

	char *parts[PARTS_MAX];
	int count = split_csv(line, parts);
	if (count <= 0 || parts[0] == nullptr) {
		return;
	}

	if (count == 5 && strcmp(parts[0], "CMD") == 0 && strcmp(parts[2], "LOCK") == 0) {
		(void)execute_gateway_command(parts[2], parts[3], parts[4], parts[1]);
		return;
	}

	if (count == 4 && strcmp(parts[0], "CMD") == 0 && strcmp(parts[2], "GET") == 0) {
		(void)execute_gateway_command(parts[2], parts[3], nullptr, parts[1]);
		return;
	}

	if (count == 2 && strcmp(parts[0], "PING") == 0 && strcmp(parts[1], "ESP") == 0) {
		forward_to_wifi("PONG,GATEWAY");
		return;
	}

	if (is_wifi_status_line(rawLine)) {
		forward_to_screen(rawLine);
		screen_publish_link_status(rawLine);
	}
}

void finalize_rx_line(UartRxContext *ctx)
{
	if (ctx == nullptr || ctx->pos == 0) {
		return;
	}

	ctx->lineBuf[ctx->pos] = '\0';
	char line[RX_BUF_SIZE];
	memcpy(line, ctx->lineBuf, ctx->pos + 1U);

	(void)k_msgq_put(ctx->msgq, line, K_NO_WAIT);
	ctx->pos = 0;
}

void uart_rx_isr(const device *dev, void *user_data)
{
	auto *ctx = static_cast<UartRxContext *>(user_data);
	if (ctx == nullptr || !uart_irq_update(dev)) {
		return;
	}

	while (uart_irq_rx_ready(dev)) {
		uint8_t fifo[16];
		int rd = uart_fifo_read(dev, fifo, sizeof(fifo));
		if (rd <= 0) {
			break;
		}

		if (dev == rs485) {
			atomic_set(&rs485_last_activity_ms, (atomic_val_t)k_uptime_get_32());
		}

		for (int i = 0; i < rd; ++i) {
			uint8_t c = fifo[i];

			if (c == '\r' || c == '\n') {
				finalize_rx_line(ctx);
				continue;
			}

			if (ctx->pos < (RX_BUF_SIZE - 1U)) {
				ctx->lineBuf[ctx->pos++] = (char)c;
			} else {
				ctx->pos = 0;
			}
		}
	}
}
} // namespace

int main()
{
	char rs485Line[RX_BUF_SIZE] = {0};
	char screenLine[RX_BUF_SIZE] = {0};
	char wifiLine[RX_BUF_SIZE] = {0};
	init_states();
	atomic_set(&rs485_last_activity_ms, (atomic_val_t)k_uptime_get_32());

	int64_t nextPollAt = k_uptime_get() + POLL_INTERVAL_MS;
	int64_t nextDiscoveryAt = k_uptime_get() + DISCOVERY_INTERVAL_MS;
	int64_t nextRs485TxAt = k_uptime_get();
	int pollCursor = 1;

	if (!device_is_ready(rs485) || !device_is_ready(screen) || !device_is_ready(wifi)) {
		return -1;
	}

	if (uart_irq_callback_user_data_set(rs485, uart_rx_isr, &rs485Rx) < 0 ||
	    uart_irq_callback_user_data_set(screen, uart_rx_isr, &screenRx) < 0 ||
	    uart_irq_callback_user_data_set(wifi, uart_rx_isr, &wifiRx) < 0) {
		return -1;
	}

	uart_irq_rx_enable(rs485);
	uart_irq_rx_enable(screen);
	uart_irq_rx_enable(wifi);

	forward_to_screen("GATEWAY_READY");
	forward_to_wifi("READY,GATEWAY,1");

	while (true) {
		while (k_msgq_get(&rs485_msgq, rs485Line, K_NO_WAIT) == 0) {
			process_rs485_line(rs485Line);
		}

		while (k_msgq_get(&screen_msgq, screenLine, K_NO_WAIT) == 0) {
			process_screen_line(screenLine);
		}

		while (k_msgq_get(&wifi_msgq, wifiLine, K_NO_WAIT) == 0) {
			process_wifi_line(wifiLine);
		}

		int64_t now = k_uptime_get();
		if (now >= nextRs485TxAt) {
			Rs485Cmd cmd = {};
			if (k_msgq_get(&rs485_cmd_msgq, &cmd, K_NO_WAIT) == 0) {
				bool sent = false;
				if (cmd.action == Rs485Action::Lock) {
					sent = send_set_lock(cmd.nodeId, cmd.value);
				} else {
					sent = send_poll(cmd.nodeId);
				}

				if (cmd.needAck && cmd.commandId[0] != '\0' && sent) {
					send_wifi_ack(cmd.commandId, "sent", "rs485_written");
				} else if (cmd.needAck && cmd.commandId[0] != '\0') {
					send_wifi_ack(cmd.commandId, "failed", "bus_busy");
				}

				nextRs485TxAt = now + RS485_TX_INTERVAL_MS;
				nextPollAt = now + POLL_INTERVAL_MS;
				nextDiscoveryAt = now + DISCOVERY_INTERVAL_MS;
			} else if (now >= nextDiscoveryAt) {
				if (send_discovery_probe()) {
					nextDiscoveryAt = now + DISCOVERY_INTERVAL_MS;
				}
				nextRs485TxAt = now + RS485_TX_INTERVAL_MS;
			} else if (now >= nextPollAt) {
				int addr = find_next_registered_addr(pollCursor);
				if (addr > 0 && send_poll(addr)) {
					pollCursor = (addr % NODE_ADDR_MAX) + 1;
				}
				nextPollAt = now + POLL_INTERVAL_MS;
				nextRs485TxAt = now + RS485_TX_INTERVAL_MS;
			}
		}

		for (int i = 0; i < NODE_ADDR_MAX; ++i) {
			if (states[i].online && (now - states[i].lastSeen > NODE_OFFLINE_TIMEOUT_MS)) {
				char out[32];

				states[i].online = false;
				snprintk(out, sizeof(out), "OFFLINE,%d", states[i].id);
				forward_upstream(out);
			}
		}

		k_msleep(POLL_DELAY_MS);
	}
}
