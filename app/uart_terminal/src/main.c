#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define UART_NODE DT_CHOSEN(zephyr_shell_uart)
#define MSG_SIZE 64

K_MSGQ_DEFINE(uart_msgq, MSG_SIZE, 8, 4);

static const struct device *const uart_dev = DEVICE_DT_GET(UART_NODE);
static char rx_buf[MSG_SIZE];
static size_t rx_buf_pos;

static void uart_send(const char *buf)
{
	for (size_t i = 0; i < strlen(buf); ++i) {
		uart_poll_out(uart_dev, buf[i]);
	}
}

static void uart_isr(const struct device *dev, void *user_data)
{
	uint8_t c;

	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	if (!uart_irq_update(uart_dev) || !uart_irq_rx_ready(uart_dev)) {
		return;
	}

	while (uart_fifo_read(uart_dev, &c, 1) == 1) {
		if (c == '\r' || c == '\n') {
			if (rx_buf_pos == 0) {
				continue;
			}

			rx_buf[rx_buf_pos] = '\0';
			k_msgq_put(&uart_msgq, rx_buf, K_NO_WAIT);
			rx_buf_pos = 0;
		} else if (rx_buf_pos < (MSG_SIZE - 1U)) {
			rx_buf[rx_buf_pos++] = (char)c;
		}
	}
}

int main(void)
{
	char msg[MSG_SIZE];
	int ret;

	if (!device_is_ready(uart_dev)) {
		printk("UART device is not ready\r\n");
		return -ENODEV;
	}

	ret = uart_irq_callback_user_data_set(uart_dev, uart_isr, NULL);
	if (ret < 0) {
		printk("Failed to enable UART IRQ callback: %d\r\n", ret);
		return ret;
	}

	uart_irq_rx_enable(uart_dev);

	uart_send("\r\nESP32-S3 N16R8 UART terminal ready.\r\n");
	uart_send("Type a line and press Enter.\r\n");

	while (1) {
		if (k_msgq_get(&uart_msgq, msg, K_FOREVER) == 0) {
			uart_send("Echo: ");
			uart_send(msg);
			uart_send("\r\n");
		}
	}

	return 0;
}
