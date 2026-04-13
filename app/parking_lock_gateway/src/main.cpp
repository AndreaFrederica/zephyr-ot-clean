#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

namespace {
#define RS485_NODE DT_ALIAS(rs485_uart)
#define SCREEN_NODE DT_ALIAS(screen_uart)
#define WIFI_NODE DT_ALIAS(wifi_uart)

constexpr size_t RX_BUF_SIZE = 128;
constexpr int POLL_DELAY_MS = 10;
constexpr int POLL_INTERVAL_MS = 200;
constexpr int NODE_ADDR_MAX = 4;
constexpr int PARTS_MAX = 24;
constexpr int RX_MSGQ_DEPTH = 8;

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

NodeState states[NODE_ADDR_MAX];

struct UartRxContext {
	char lineBuf[RX_BUF_SIZE];
	size_t pos;
	struct k_msgq *msgq;
};

K_MSGQ_DEFINE(rs485_msgq, RX_BUF_SIZE, RX_MSGQ_DEPTH, 4);
K_MSGQ_DEFINE(screen_msgq, RX_BUF_SIZE, RX_MSGQ_DEPTH, 4);
K_MSGQ_DEFINE(wifi_msgq, RX_BUF_SIZE, RX_MSGQ_DEPTH, 4);

UartRxContext rs485Rx = {{0}, 0, &rs485_msgq};
UartRxContext screenRx = {{0}, 0, &screen_msgq};
UartRxContext wifiRx = {{0}, 0, &wifi_msgq};

void uart_send(const device *dev, const char *text)
{
	size_t length = strlen(text);

	for (size_t i = 0; i < length; ++i) {
		uart_poll_out(dev, (uint8_t)text[i]);
	}
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

int split_csv(char *line, char *parts[PARTS_MAX])
{
	int count = 0;
	char *cursor = line;

	for (int i = 0; i < PARTS_MAX; ++i) {
		parts[i] = nullptr;
	}

	while (*cursor != '\0' && count < PARTS_MAX) {
		parts[count++] = cursor;
		while (*cursor != '\0' && *cursor != ',') {
			++cursor;
		}
		if (*cursor == ',') {
			*cursor = '\0';
			++cursor;
		}
	}

	return count;
}

NodeState *findNode(int id)
{
	if (id < 1 || id > NODE_ADDR_MAX) {
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
	}
}

void send_to_cloud_and_screen(const char *payload)
{
	uart_send(screen, payload);
	uart_send(screen, "\r\n");
	uart_send(wifi, payload);
	uart_send(wifi, "\r\n");
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

void process_rs485_line(char *line)
{
	char rawLine[RX_BUF_SIZE];
	strncpy(rawLine, line, sizeof(rawLine) - 1U);
	rawLine[sizeof(rawLine) - 1U] = '\0';

	char *parts[PARTS_MAX];
	int count = split_csv(line, parts);

	if (count < 3 || parts[0] == nullptr) {
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

	send_to_cloud_and_screen(rawLine);
}

void send_poll(int nodeId)
{
	char out[32];

	snprintk(out, sizeof(out), "REQ,%d,GET\r\n", nodeId);
	uart_send(rs485, out);
}

void send_set_lock(int nodeId, int value)
{
	char out[32];

	snprintk(out, sizeof(out), "SET,%d,LOCK,%d\r\n", nodeId, value);
	uart_send(rs485, out);
}

void process_gateway_command(char *line)
{
	char *parts[PARTS_MAX];
	int count = split_csv(line, parts);

	if (count == 3 && strcmp(parts[0], "LOCK") == 0) {
		int nodeId = 0;
		int value = 0;
		if (parse_int(parts[1], &nodeId) && parse_int(parts[2], &value) && (value == 0 || value == 1)) {
			send_set_lock(nodeId, value);
		}
	}

	if (count == 2 && strcmp(parts[0], "GET") == 0) {
		int nodeId = 0;
		if (parse_int(parts[1], &nodeId)) {
			send_poll(nodeId);
		}
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
}

int main()
{
	char rs485Line[RX_BUF_SIZE] = {0};
	char screenLine[RX_BUF_SIZE] = {0};
	char wifiLine[RX_BUF_SIZE] = {0};
	init_states();

	int64_t nextPollAt = k_uptime_get() + POLL_INTERVAL_MS;
	int pollCursor = 1;

	if (!device_is_ready(rs485) || !device_is_ready(screen) || !device_is_ready(wifi)) {
		return -1;
	}

	uart_send(screen, "GATEWAY_READY\r\n");
	uart_send(wifi, "GATEWAY_READY\r\n");

	if (uart_irq_callback_user_data_set(rs485, uart_rx_isr, &rs485Rx) < 0 ||
	    uart_irq_callback_user_data_set(screen, uart_rx_isr, &screenRx) < 0 ||
	    uart_irq_callback_user_data_set(wifi, uart_rx_isr, &wifiRx) < 0) {
		return -1;
	}

	uart_irq_rx_enable(rs485);
	uart_irq_rx_enable(screen);
	uart_irq_rx_enable(wifi);

	while (true) {
		while (k_msgq_get(&rs485_msgq, rs485Line, K_NO_WAIT) == 0) {
			process_rs485_line(rs485Line);
		}

		while (k_msgq_get(&screen_msgq, screenLine, K_NO_WAIT) == 0) {
			process_gateway_command(screenLine);
		}

		while (k_msgq_get(&wifi_msgq, wifiLine, K_NO_WAIT) == 0) {
			process_gateway_command(wifiLine);
		}

		int64_t now = k_uptime_get();
		if (now >= nextPollAt) {
			send_poll(pollCursor);
			pollCursor = (pollCursor % NODE_ADDR_MAX) + 1;
			nextPollAt = now + POLL_INTERVAL_MS;
		}

		for (int i = 0; i < NODE_ADDR_MAX; ++i) {
			if (states[i].online && (k_uptime_get() - states[i].lastSeen > 15000)) {
				char out[32];

				states[i].online = false;
				snprintk(out, sizeof(out), "OFFLINE,%d", states[i].id);
				send_to_cloud_and_screen(out);
			}
		}

		k_msleep(POLL_DELAY_MS);
	}
}
