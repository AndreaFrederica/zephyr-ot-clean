#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void)
{
	printk("Hello World from esp32s3_n16r8 (%s)\r\n", CONFIG_BOARD_TARGET);

	while (1) {
		printk("tick\r\n");
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
