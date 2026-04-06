/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/fatal.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/arch/cpu.h>

#include <soc.h>
#include <esp_rom_sys.h>
#include <esp_private/system_internal.h>

LOG_MODULE_REGISTER(fanctl_fatal, LOG_LEVEL_ERR);

extern "C" void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);

	/* Log the fatal error */
	LOG_PANIC();
	LOG_ERR("Fatal error %u - initiating emergency reboot", reason);

	/* Flush any pending log output */
	k_sleep(K_MSEC(100));

	/* 
	 * ESP32-S3 specific: Use ROM software reset which is more reliable
	 * in fatal error context than sys_reboot(). 
	 * This function resets the entire system including RTC.
	 */
	LOG_ERR("Triggering ESP ROM software reset...");
	esp_rom_software_reset_system();

	/* 
	 * Fallback: If ROM restart returns (should not happen),
	 * try system reboot API as last resort.
	 */
	sys_reboot(SYS_REBOOT_COLD);

	/* 
	 * Absolute last resort: If we're still here, the system is truly stuck.
	 * Halt and let watchdog (if enabled) eventually reset us.
	 */
	LOG_ERR("All restart attempts failed - halting for watchdog");
	k_fatal_halt(reason);
}
