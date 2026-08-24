/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * mcuboot_status_change() — LED breathing and USB teardown/bringup.
 * Standalone Zephyr module (-DEXTRA_ZEPHYR_MODULES), no MCUboot patches.
 * Requires CONFIG_MCUBOOT_ACTION_HOOKS=y.
 */

#include <zephyr/kernel.h>

#ifdef CONFIG_USB_DEVICE_STACK_NEXT
#include <zephyr/usb/usbd.h>
#include <usb_soft_disconnect.h>
#endif

#include "bootutil/mcuboot_status.h"

#include "hooks.h"

/* Pin slot addresses so MCUboot overlay / variant overlay drift is a build error. */
#define SLOT_ABS(label) \
	(DT_REG_ADDR(DT_GPARENT(DT_NODELABEL(label))) + DT_REG_ADDR(DT_NODELABEL(label)))

BUILD_ASSERT(SLOT_ABS(slot0_partition) == 0x0C000000,
	     "slot0 moved: keep mcuboot.overlay and the variant's -common.overlay in sync");
BUILD_ASSERT(SLOT_ABS(slot2_partition) == 0x0C048000,
	     "slot2 moved: keep mcuboot.overlay and the variant's -common.overlay in sync");

#ifdef CONFIG_USB_DEVICE_STACK_NEXT
/* Drop D+ pull-up before chainload so the host sees a detach/attach edge.
 * usb_soft_disconnect(), not usbd_disable() — the latter blocks on bus reset. */
static void usb_teardown(void)
{
	usb_soft_disconnect();
}

/* Enable USB only in recovery — avoids a phantom port during the double-reset window. */
static void usb_bringup(void)
{
	STRUCT_SECTION_FOREACH(usbd_context, ctx) {
		(void)usbd_enable(ctx);
	}
}
#else
static inline void usb_teardown(void)
{
}

static inline void usb_bringup(void)
{
}
#endif /* CONFIG_USB_DEVICE_STACK_NEXT */

void mcuboot_status_change(mcuboot_status_type_t status)
{
	switch (status) {
	case MCUBOOT_STATUS_SERIAL_DFU_ENTERED:
	case MCUBOOT_STATUS_USB_DFU_ENTERED:
		/* Before boot_console_init(), which is where MCUboot opens the
		 * CDC-ACM device it is about to serve recovery on. */
		usb_bringup();
		mcuboot_hooks_led_breathe();
		break;

	case MCUBOOT_STATUS_BOOTABLE_IMAGE_FOUND:
		usb_teardown();
		break;

	default:
		break;
	}
}
