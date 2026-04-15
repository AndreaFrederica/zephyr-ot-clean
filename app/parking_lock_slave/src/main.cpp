#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>

namespace {

#define RS485_NODE DT_ALIAS(rs485_uart)

constexpr size_t RX_BUF_SIZE = 128;
constexpr int NODE_ADDR_SPACE = 4;
constexpr int PING_SLOT_MS = 8;
constexpr int PARTS_MAX = 8;
constexpr size_t UID_TEXT_MAX = 2 * 8 + 1;

const device *const rs485 = DEVICE_DT_GET(RS485_NODE);
const device *const dht_sensor = DEVICE_DT_GET(DT_NODELABEL(dht0));

const gpio_dt_spec flame_do =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), flame_do_gpios);

const adc_dt_spec flame_adc =
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);
const pwm_dt_spec lock_servo = PWM_DT_SPEC_GET(DT_PATH(zephyr_user));

bool dht_available = false;
bool flame_do_available = false;
bool flame_adc_available = false;
bool lock_servo_available = false;
int lock_state = 0;
int lock_servo_ret = -100;

uint32_t rx_bytes = 0;
uint32_t rx_lines = 0;
uint32_t rx_cmds = 0;
uint32_t rx_overflows = 0;
int node_id = 1;
char node_uid_text[UID_TEXT_MAX] = "NA";
bool has_dht_cache = false;
int32_t dht_temp_cache_centi = 0;
int32_t dht_humi_cache_centi = 0;

char rx_line[RX_BUF_SIZE];
size_t rx_line_pos = 0;

K_MSGQ_DEFINE(line_msgq, RX_BUF_SIZE, 8, 4);

void init_node_identity()
{
	/* 固定 node_id = 1，禁用硬件 UID 生成逻辑 */
	node_id = 1;
	strcpy(node_uid_text, "NA");
	return;

	/*
	uint8_t uid[16];
	ssize_t uid_len = hwinfo_get_device_id(uid, sizeof(uid));
	if (uid_len <= 0) {
		node_id = 1;
		strcpy(node_uid_text, "NA");
		return;
	}

	uint8_t hash = 0;
	for (ssize_t i = 0; i < uid_len; ++i) {
		hash = (uint8_t)((hash * 33U) ^ uid[i]);
	}
	node_id = (hash % NODE_ADDR_SPACE) + 1;

	size_t bytes_to_print = (size_t)uid_len;
	if (bytes_to_print > 8U) {
		bytes_to_print = 8U;
	}

	for (size_t i = 0; i < bytes_to_print; ++i) {
		snprintk(&node_uid_text[i * 2U], 3, "%02X", uid[i]);
	}
	node_uid_text[bytes_to_print * 2U] = '\0';
	*/
}

void uart_send_bytes(const char *data, size_t len)
{
	for (size_t i = 0; i < len; ++i) {
		uart_poll_out(rs485, static_cast<uint8_t>(data[i]));
	}
}

void uart_send_text(const char *text)
{
	uart_send_bytes(text, strlen(text));
}

void uart_send_line(const char *text)
{
	uart_send_text(text);
	uart_send_text("\r\n");
}

bool parse_int(const char *text, int *value)
{
	if (text == nullptr || value == nullptr || *text == '\0') {
		return false;
	}

	char *end = nullptr;
	long parsed = strtol(text, &end, 10);
	if (end == nullptr || *end != '\0') {
		return false;
	}

	*value = static_cast<int>(parsed);
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

int read_flame_adc_mv(int32_t *mv)
{
	if (!flame_adc_available || mv == nullptr) {
		return -ENODEV;
	}

	int16_t raw = 0;
	adc_sequence sequence = {
		.buffer = &raw,
		.buffer_size = sizeof(raw),
	};

	int ret = adc_sequence_init_dt(&flame_adc, &sequence);
	if (ret < 0) {
		return ret;
	}

	ret = adc_read_dt(&flame_adc, &sequence);
	if (ret < 0) {
		return ret;
	}

	*mv = raw;
	return adc_raw_to_millivolts_dt(&flame_adc, mv);
}

int read_dht_values(int32_t *temp_centi, int32_t *humi_centi)
{
	if (!dht_available || temp_centi == nullptr || humi_centi == nullptr) {
		return -ENODEV;
	}

	sensor_value temp = {};
	sensor_value humi = {};

	int ret = sensor_sample_fetch(dht_sensor);
	if (ret < 0) {
		return ret;
	}

	ret = sensor_channel_get(dht_sensor, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	if (ret < 0) {
		return ret;
	}

	ret = sensor_channel_get(dht_sensor, SENSOR_CHAN_HUMIDITY, &humi);
	if (ret < 0) {
		return ret;
	}

	const int64_t temp_micro = (int64_t)temp.val1 * 1000000LL + temp.val2;
	const int64_t humi_micro = (int64_t)humi.val1 * 1000000LL + humi.val2;
	*temp_centi = (int32_t)(temp_micro / 10000LL);
	*humi_centi = (int32_t)(humi_micro / 10000LL);
	return 0;
}

void set_lock(bool locked)
{
	if (!lock_servo_available) {
		lock_servo_ret = -ENODEV;
		return;
	}

	const uint32_t pulse = locked ? PWM_USEC(2000) : PWM_USEC(1000);
	lock_servo_ret = pwm_set_pulse_dt(&lock_servo, pulse);
	if (lock_servo_ret == 0) {
		lock_state = locked ? 1 : 0;
	}
}

void send_text_with_node(const char *head, const char *tail)
{
	char out[96];
	snprintk(out, sizeof(out), "%s,%d,%s\r\n", head, node_id, tail);
	uart_send_text(out);
}

void send_sensor_report(const char *reason)
{
	char out[320];

	int flame_digital = -1;
	int32_t flame_mv = -1;
	int32_t temp_centi = -100000;
	int32_t humi_centi = -100000;
	int flame_ret = -100;
	int dht_ret = -100;

	if (flame_do_available) {
		flame_digital = gpio_pin_get_dt(&flame_do);
	}

	if (flame_adc_available) {
		flame_ret = read_flame_adc_mv(&flame_mv);
	}

	if (dht_available) {
		dht_ret = read_dht_values(&temp_centi, &humi_centi);
		if (dht_ret == 0) {
			dht_temp_cache_centi = temp_centi;
			dht_humi_cache_centi = humi_centi;
			has_dht_cache = true;
		}
	}

	// DHT occasionally fails during/after lock actuation; keep last valid reading
	// to avoid transient -1000.00 spikes in telemetry.
	if (dht_ret != 0 && has_dht_cache) {
		temp_centi = dht_temp_cache_centi;
		humi_centi = dht_humi_cache_centi;
	}

	snprintk(out, sizeof(out),
		 "REPORT,%d,%s,T=%d.%02d,H=%d.%02d,FD=%d,FA=%d,L=%d,DE=%d,AE=%d,SE=%d,RB=%u,RL=%u,RC=%u,RO=%u,UID=%s\r\n",
		 node_id,
		 reason,
		 temp_centi / 100,
		 temp_centi >= 0 ? temp_centi % 100 : -(temp_centi % 100),
		 humi_centi / 100,
		 humi_centi >= 0 ? humi_centi % 100 : -(humi_centi % 100),
		 flame_digital,
		 static_cast<int>(flame_mv),
		 lock_state,
		 dht_ret,
		 flame_ret,
		 lock_servo_ret,
		 rx_bytes,
		 rx_lines,
		 rx_cmds,
		 rx_overflows,
		 node_uid_text);

	uart_send_text(out);
}

void process_req(char *parts[PARTS_MAX], int count)
{
	if (count < 3) {
		return;
	}

	int req_node = -1;
	if (!parse_int(parts[1], &req_node)) {
		return;
	}

	if (req_node != node_id && req_node != 0) {
		return;
	}

	if (strcmp(parts[2], "PING") == 0) {
		if (req_node == 0) {
			k_msleep((node_id - 1) * PING_SLOT_MS);
		}
		send_text_with_node("ACK", "PONG");
		return;
	}

	if (strcmp(parts[2], "GET") == 0) {
		if (req_node == 0) {
			return;
		}
		send_sensor_report("POLL");
		return;
	}
}

void process_set(char *parts[PARTS_MAX], int count)
{
	if (count < 4) {
		return;
	}

	int req_node = -1;
	if (!parse_int(parts[1], &req_node)) {
		return;
	}

	if (req_node != node_id) {
		return;
	}

	if (strcmp(parts[2], "LOCK") != 0) {
		return;
	}

	int lock_value = -1;
	if (!parse_int(parts[3], &lock_value) || (lock_value != 0 && lock_value != 1)) {
		return;
	}

	if (!lock_servo_available) {
		return;
	}

	set_lock(lock_value == 1);
	send_text_with_node("ACK", lock_value == 1 ? "LOCK,1" : "LOCK,0");
	send_sensor_report("SET");
}

void handle_command(char *line)
{
	char *parts[PARTS_MAX];
	int count = split_csv(line, parts);

	if (count <= 0 || parts[0] == nullptr || parts[0][0] == '\0') {
		return;
	}

	rx_cmds++;

	if (strcmp(parts[0], "REQ") == 0) {
		process_req(parts, count);
		return;
	}

	if (strcmp(parts[0], "SET") == 0) {
		process_set(parts, count);
		return;
	}
}

void finalize_rx_line_from_isr()
{
	if (rx_line_pos == 0) {
		return;
	}

	rx_line[rx_line_pos] = '\0';
	char line_copy[RX_BUF_SIZE];
	memcpy(line_copy, rx_line, rx_line_pos + 1);

	if (k_msgq_put(&line_msgq, line_copy, K_NO_WAIT) != 0) {
		rx_overflows++;
	}

	rx_lines++;
	rx_line_pos = 0;
}

void uart_isr(const device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	if (!uart_irq_update(dev)) {
		return;
	}

	while (uart_irq_rx_ready(dev)) {
		uint8_t buf[16];
		int rd = uart_fifo_read(dev, buf, sizeof(buf));
		if (rd <= 0) {
			break;
		}

		for (int i = 0; i < rd; ++i) {
			const uint8_t c = buf[i];
			rx_bytes++;

			if (c == '\r' || c == '\n') {
				finalize_rx_line_from_isr();
				continue;
			}

			if (rx_line_pos < (RX_BUF_SIZE - 1U)) {
				rx_line[rx_line_pos++] = static_cast<char>(c);
			} else {
				rx_overflows++;
				rx_line_pos = 0;
			}
		}
	}
}

int init_devices()
{
	int boot_flags = 0;

	dht_available = device_is_ready(dht_sensor);
	flame_do_available = gpio_is_ready_dt(&flame_do);
	lock_servo_available = pwm_is_ready_dt(&lock_servo);
	flame_adc_available = adc_is_ready_dt(&flame_adc);

	if (!dht_available) {
		boot_flags |= 0x01;
	}
	if (!flame_do_available) {
		boot_flags |= 0x02;
	}
	if (!flame_adc_available) {
		boot_flags |= 0x08;
	}

	if (flame_do_available && gpio_pin_configure_dt(&flame_do, GPIO_INPUT) < 0) {
		flame_do_available = false;
		boot_flags |= 0x20;
	}

	if (!lock_servo_available) {
		boot_flags |= 0x100;
	}

	if (flame_adc_available && adc_channel_setup_dt(&flame_adc) < 0) {
		flame_adc_available = false;
		boot_flags |= 0x80;
	}

	return boot_flags;
}

} // namespace

int main()
{
	if (!device_is_ready(rs485)) {
		return -1;
	}

	init_node_identity();
	uart_send_line("BOOT,1,START");

	const int boot_flags = init_devices();

	k_sleep(K_SECONDS(1));

	char boot[128];
	snprintk(boot, sizeof(boot),
		 "BOOT,%d,READY,FLAGS=%d,DHT=%d,FD=%d,LOCK=%d,ADC=%d,SRV=%d,UID=%s\r\n",
		 node_id,
		 boot_flags,
		 dht_available ? 1 : 0,
		 flame_do_available ? 1 : 0,
		 lock_servo_available ? 1 : 0,
		 flame_adc_available ? 1 : 0,
		 lock_servo_available ? 1 : 0,
		 node_uid_text);
	uart_send_text(boot);

	int ret = uart_irq_callback_user_data_set(rs485, uart_isr, nullptr);
	if (ret < 0) {
		uart_send_line("ERR,1,UART_IRQ_CB");
		return ret;
	}

	uart_irq_rx_enable(rs485);

	char line[RX_BUF_SIZE];

	while (true) {
		while (k_msgq_get(&line_msgq, line, K_NO_WAIT) == 0) {
			handle_command(line);
		}

		k_msleep(1);
	}

	return 0;
}
