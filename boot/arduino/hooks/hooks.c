/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The single mcuboot_status_change() implementation for Arduino boards, and
 * the USB teardown that has to happen before the chainload.
 *
 * This is a standalone Zephyr module injected into MCUboot's build with
 * -DEXTRA_ZEPHYR_MODULES=<this dir> (the pattern MCUboot itself demonstrates in
 * samples/runtime-source/zephyr/hooks), rather than a patch against the vendored
 * mcuboot tree: MCUboot already calls mcuboot_status_change() at every state
 * transition worth acting on, so nothing in its sources has to change and there
 * is nothing to re-apply after a `west update`. Requires
 * CONFIG_MCUBOOT_ACTION_HOOKS=y.
 *
 * MCUboot permits exactly one definition of that callback, which is why the
 * breathing LED (status_led.c) and the USB shutdown below are dispatched from
 * here instead of each owning their own.
 */

#include <zephyr/kernel.h>

#ifdef CONFIG_USB_DEVICE_STACK_NEXT
#include <zephyr/usb/usbd.h>
#include <usb_soft_disconnect.h>
#endif

#include "bootutil/mcuboot_status.h"

#include "hooks.h"

#ifdef CONFIG_USB_DEVICE_STACK_NEXT
/*
 * Hand the incoming image a USB bus it can enumerate on.
 *
 * MCUboot's do_boot() disables USB only under CONFIG_USB_DEVICE_STACK - the
 * legacy stack - so with the next stack nothing lets go of the bus before
 * the chainload.
 *
 * What matters is not the software state (the incoming image zeroes .bss and
 * re-initialises everything anyway) but the D+ pull-up: chainloading does not
 * reset USBHS, so DPRPU stays asserted, the host never sees a detach/attach
 * edge, and the loader comes up running but invisible on USB.
 *
 * usb_soft_disconnect() is the variant's own helper, shared rather than
 * copied so there is one definition of where the pull-up control lives.
 * Deliberately NOT usbd_disable(): per that header it blocks forever if a bus
 * reset is in flight, which would hang the bootloader instead of booting it.
 * Pending UDC interrupts are not a concern either —
 * CONFIG_MCUBOOT_CLEANUP_ARM_CORE=y disables and clears the NVIC immediately
 * after this returns.
 */
static void usb_teardown(void)
{
	usb_soft_disconnect();
}

/*
 * Bring USB up, but only once recovery has actually been entered.
 *
 * Zephyr's CDC_ACM_SERIAL_INITIALIZE_AT_BOOT registers the class and calls
 * usbd_init() from a SYS_INIT; CONFIG_CDC_ACM_SERIAL_ENABLE_AT_BOOT=n stops
 * it also calling usbd_enable(), which is what asserts the D+ pull-up. Doing
 * that here means the bootloader is only visible on USB when there is
 * something for a host to talk to.
 *
 * It matters because MCUboot sits in the double-reset window for
 * CONFIG_BOOT_SERIAL_DOUBLE_RESET_WINDOW_MS on EVERY boot. Enumerating for
 * that window puts a phantom port on the bus: anything the host sent to it
 * during that window (notably a 1200-bps touch, which only the loader acts
 * on) went nowhere. USB is now only asserted in recovery mode.
 *
 * The context belongs to Zephyr's cdc_acm_serial.c and is static there, so
 * it is reached by iterating the section rather than by name.
 */
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
		/*
		 * Reported from main() immediately before do_boot(), which is
		 * the last point at which this image is still running.
		 *
		 * With USB brought up only in recovery this is a no-op on the
		 * normal path — the pull-up was never asserted — and the
		 * chainload is that much cleaner for it. Kept because it makes
		 * the handoff correct if USB is ever enabled here again.
		 */
		usb_teardown();
		break;

	default:
		break;
	}
}
