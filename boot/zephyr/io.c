/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 * Copyright (c) 2020 Arm Limited
 * Copyright (c) 2021-2023 Nordic Semiconductor ASA
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <zephyr/usb/usb_device.h>
#include <soc.h>
#include <zephyr/linker/linker-defs.h>

#include "target.h"
#include "bootutil/bootutil_log.h"

BOOT_LOG_MODULE_DECLARE(mcuboot);

#if defined(CONFIG_BOOT_SERIAL_PIN_RESET) || defined(CONFIG_BOOT_FIRMWARE_LOADER_PIN_RESET)
#include <zephyr/drivers/hwinfo.h>
#endif

#if defined(CONFIG_BOOT_SERIAL_BOOT_MODE) || defined(CONFIG_BOOT_FIRMWARE_LOADER_BOOT_MODE)
#include <zephyr/retention/bootmode.h>
#endif

/* Validate serial recovery configuration */
#ifdef CONFIG_MCUBOOT_SERIAL
#if !defined(CONFIG_BOOT_SERIAL_ENTRANCE_GPIO) && \
    !defined(CONFIG_BOOT_SERIAL_WAIT_FOR_DFU) && \
    !defined(CONFIG_BOOT_SERIAL_BOOT_MODE) && \
    !defined(CONFIG_BOOT_SERIAL_NO_APPLICATION) && \
    !defined(CONFIG_BOOT_SERIAL_PIN_RESET) && \
    !defined(CONFIG_BOOT_SERIAL_DOUBLE_RESET)
#error "Serial recovery selected without an entrance mode set"
#endif
#endif

/* Validate firmware loader configuration */
#ifdef CONFIG_BOOT_FIRMWARE_LOADER
#if !defined(CONFIG_BOOT_FIRMWARE_LOADER_ENTRANCE_GPIO) && \
    !defined(CONFIG_BOOT_FIRMWARE_LOADER_BOOT_MODE) && \
    !defined(CONFIG_BOOT_FIRMWARE_LOADER_NO_APPLICATION) && \
    !defined(CONFIG_BOOT_FIRMWARE_LOADER_PIN_RESET)
#error "Firmware loader selected without an entrance mode set"
#endif
#endif

#ifdef CONFIG_MCUBOOT_INDICATION_LED

/*
 * The led0 devicetree alias is optional. If present, we'll use it
 * to turn on the LED whenever the button is pressed.
 */
#if DT_NODE_EXISTS(DT_ALIAS(mcuboot_led0))
#define LED0_NODE DT_ALIAS(mcuboot_led0)
#endif

#if DT_NODE_HAS_STATUS(LED0_NODE, okay) && DT_NODE_HAS_PROP(LED0_NODE, gpios)
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
#else
/* A build error here means your board isn't set up to drive an LED. */
#error "Unsupported board: led0 devicetree alias is not defined"
#endif

void io_led_init(void)
{
    if (!device_is_ready(led0.port)) {
        BOOT_LOG_ERR("Didn't find LED device referred by the LED0_NODE\n");
        return;
    }

    gpio_pin_configure_dt(&led0, GPIO_OUTPUT);
    gpio_pin_set_dt(&led0, 0);
}

void io_led_set(int value)
{
    gpio_pin_set_dt(&led0, value);
}
#endif /* CONFIG_MCUBOOT_INDICATION_LED */

#if defined(CONFIG_BOOT_SERIAL_ENTRANCE_GPIO) || defined(CONFIG_BOOT_USB_DFU_GPIO) || \
    defined(CONFIG_BOOT_FIRMWARE_LOADER_ENTRANCE_GPIO)

#if defined(CONFIG_MCUBOOT_SERIAL)
#define BUTTON_0_DETECT_DELAY CONFIG_BOOT_SERIAL_DETECT_DELAY
#elif defined(CONFIG_BOOT_FIRMWARE_LOADER)
#define BUTTON_0_DETECT_DELAY CONFIG_BOOT_FIRMWARE_LOADER_DETECT_DELAY
#else
#define BUTTON_0_DETECT_DELAY CONFIG_BOOT_USB_DFU_DETECT_DELAY
#endif

#define BUTTON_0_NODE DT_ALIAS(mcuboot_button0)

#if DT_NODE_EXISTS(BUTTON_0_NODE) && DT_NODE_HAS_PROP(BUTTON_0_NODE, gpios)
static const struct gpio_dt_spec button0 = GPIO_DT_SPEC_GET(BUTTON_0_NODE, gpios);
#else
#error "Serial recovery/USB DFU button must be declared in device tree as 'mcuboot_button0'"
#endif

bool io_detect_pin(void)
{
    int rc;
    int pin_active;

    if (!device_is_ready(button0.port)) {
        BOOT_LOG_DBG("GPIO device is not ready.");
        return false;
    }

    rc = gpio_pin_configure_dt(&button0, GPIO_INPUT);
    if (rc != 0) {
        BOOT_LOG_DBG("Failed to initialize boot detect pin.");
        return false;
    }

    rc = gpio_pin_get_dt(&button0);
    pin_active = rc;

    if (rc < 0) {
        BOOT_LOG_DBG("Failed to read boot detect pin.");
        return false;
    }


    if (pin_active) {
        if (BUTTON_0_DETECT_DELAY > 0) {
#ifdef CONFIG_MULTITHREADING
            k_sleep(K_MSEC(50));
#else
            k_busy_wait(50000);
#endif

            /* Get the uptime for debounce purposes. */
            int64_t timestamp = k_uptime_get();

            for(;;) {
                rc = gpio_pin_get_dt(&button0);
                pin_active = rc;
                if (rc < 0) {
                    BOOT_LOG_DBG("Failed to read boot detect pin.");
                    return false;
                }

                /* Get delta from when this started */
                uint32_t delta = k_uptime_get() -  timestamp;

                /* If not pressed OR if pressed > debounce period, stop. */
                if (delta >= BUTTON_0_DETECT_DELAY || !pin_active) {
                    break;
                }

                /* Delay 1 ms */
#ifdef CONFIG_MULTITHREADING
                k_sleep(K_MSEC(1));
#else
                k_busy_wait(1000);
#endif
            }
        }
    }

    return (bool)pin_active;
}
#endif

#if defined(CONFIG_BOOT_SERIAL_PIN_RESET) || defined(CONFIG_BOOT_FIRMWARE_LOADER_PIN_RESET)
bool io_detect_pin_reset(void)
{
    uint32_t reset_cause;
    int rc;

    rc = hwinfo_get_reset_cause(&reset_cause);

    if (rc == 0 && (reset_cause & RESET_PIN)) {
        (void)hwinfo_clear_reset_cause();
        return true;
    }

    return false;
}
#endif

#if defined(CONFIG_BOOT_SERIAL_BOOT_MODE) || defined(CONFIG_BOOT_FIRMWARE_LOADER_BOOT_MODE)
bool io_detect_boot_mode(void)
{
    int32_t boot_mode;

    boot_mode = bootmode_check(BOOT_MODE_TYPE_BOOTLOADER);

    if (boot_mode == 1) {
        /* Boot mode to stay in bootloader, clear status and enter serial
         * recovery mode
         */
        bootmode_clear();

        return true;
    }

    return false;
}
#endif

#ifdef CONFIG_BOOT_SERIAL_DOUBLE_RESET
/*
 * Arduino-local entrance method (not upstream MCUboot): a double physical
 * reset within a short window enters serial recovery, with no GPIO/button
 * and no cooperating application required. Ported from the noinit-RAM
 * magic-cookie technique previously used by an earlier bootloader for the
 * same board family - see ArduinoCore-zephyr/extra/mcuboot/patches/.
 */
static bool double_reset_latched;

#define DOUBLE_RESET_MAGIC 0x44524655U /* random cookie value, kept for compatibility */

/*
 * Where the cookie lives.
 *
 * If the devicetree declares a "recovery_cookie" reserved-memory region, use
 * that rather than a private __noinit word. That is what makes the gesture
 * reachable from software: a running application arms the very same word and
 * resets, so a host-side 1200-bps touch is indistinguishable from a physical
 * double-tap and an ordinary upload needs no gesture at all. Both sides derive
 * the address from this node - see ArduinoCore-zephyr's
 * variants/.../recovery_cookie.h and the matching node in
 * extra/mcuboot/nano_chandler_bfm.overlay - so neither can drift. The region
 * has to be carved out of the RAM the linker knows about, or .bss/.noinit will
 * be laid over it.
 *
 * Without such a node this falls back to a private __noinit word, which still
 * detects a physical double-tap; only the software-triggered entry is lost.
 * Either way the storage must sit outside .bss and .data so it survives a warm
 * reset but not a power cycle, which is exactly the lifetime wanted.
 */
#if DT_NODE_EXISTS(DT_NODELABEL(recovery_cookie))
#define DOUBLE_RESET_COOKIE                                                    \
    (*(volatile uint32_t *)DT_REG_ADDR(DT_NODELABEL(recovery_cookie)))
#else
static uint32_t double_reset_cookie __noinit;
#define DOUBLE_RESET_COOKIE double_reset_cookie
#endif

/*
 * Everything between the reset itself and the moment this cookie gets armed
 * is a dead zone: a second physical reset landing in it finds an unarmed
 * cookie and goes undetected, so the user has to deliberately wait before
 * tapping again. Keeping that zone as short as possible is the whole game,
 * so the arm/check runs from PRE_KERNEL_1 priority 0 - the earliest hook the
 * kernel offers, right after the C runtime is up (BSS zeroed, .data copied;
 * __noinit is excluded from both, which is what lets the cookie survive) and
 * well before console/UART/driver init.
 *
 * Doing this from main() instead - as an earlier revision did - leaves tens
 * of milliseconds of dead zone: console and UART bring-up, plus every boot
 * log line, since CONFIG_LOG_MODE_MINIMAL makes each one a *blocking* printk
 * (~87us/byte at 115200 baud). That is exactly why a fast double-tap failed
 * while a deliberately delayed one worked.
 */
static int double_reset_arm(void)
{
    double_reset_latched = (DOUBLE_RESET_COOKIE == DOUBLE_RESET_MAGIC);

    /* Arm for the next boot (or disarm, if this *is* the second tap). The
     * window is held open, and the cookie finally cleared, back in
     * io_detect_double_reset() once the kernel can actually sleep. */
    DOUBLE_RESET_COOKIE = double_reset_latched ? 0U : DOUBLE_RESET_MAGIC;

    return 0;
}
SYS_INIT(double_reset_arm, PRE_KERNEL_1, 0);

bool io_detect_double_reset(void)
{
    BOOT_LOG_DBG("Double reset latched at PRE_KERNEL_1: %d",
                 (int)double_reset_latched);

    if (!double_reset_latched) {
        /* Hold the window open so a second tap is still observed by the
         * *next* boot as "cookie already armed", then disarm. */
        k_msleep(CONFIG_BOOT_SERIAL_DOUBLE_RESET_WINDOW_MS);
        DOUBLE_RESET_COOKIE = 0U;
    }

    return double_reset_latched;
}
#endif /* CONFIG_BOOT_SERIAL_DOUBLE_RESET */
