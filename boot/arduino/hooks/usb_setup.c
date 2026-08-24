/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB device setup for MCUboot recovery.
 *
 * Replaces cdc_acm_serial.c (CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT=n)
 * so that BOTH the CDC-ACM serial class AND the DFU download class are
 * registered before usbd_init(). cdc_acm_serial.c only registers CDC-ACM,
 * so the DFU interfaces never appear on the bus.
 *
 * usbd_enable() is NOT called here — hooks.c calls it when recovery is
 * entered (MCUBOOT_STATUS_SERIAL_DFU_ENTERED), keeping the port off the
 * bus during the normal boot path.
 */

#include <zephyr/init.h>
#include <zephyr/usb/usbd.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(usb_setup, CONFIG_USBD_LOG_LEVEL);

#define MCUBOOT_USB_MAX_POWER 125  /* 250 mA in 2 mA units */

USBD_DEVICE_DEFINE(mcuboot_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   CONFIG_USB_DEVICE_VID, CONFIG_USB_DEVICE_PID);

USBD_DESC_LANG_DEFINE(mcuboot_lang);
USBD_DESC_MANUFACTURER_DEFINE(mcuboot_mfr, CONFIG_USB_DEVICE_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(mcuboot_product, CONFIG_USB_DEVICE_PRODUCT);
IF_ENABLED(CONFIG_HWINFO, (USBD_DESC_SERIAL_NUMBER_DEFINE(mcuboot_sn)));

USBD_DESC_CONFIG_DEFINE(mcuboot_fs_desc, "FS Configuration");
USBD_DESC_CONFIG_DEFINE(mcuboot_hs_desc, "HS Configuration");

USBD_CONFIGURATION_DEFINE(mcuboot_fs_config, 0,
			  MCUBOOT_USB_MAX_POWER, &mcuboot_fs_desc);

USBD_CONFIGURATION_DEFINE(mcuboot_hs_config, 0,
			  MCUBOOT_USB_MAX_POWER, &mcuboot_hs_desc);

static const char *const class_blocklist[] = {"dfu_runtime", NULL};

static int register_classes(struct usbd_context *const ctx,
			    const enum usbd_speed speed)
{
	struct usbd_config_node *cfg;
	int err;

	cfg = (speed == USBD_SPEED_HS) ? &mcuboot_hs_config : &mcuboot_fs_config;

	err = usbd_add_configuration(ctx, speed, cfg);
	if (err) {
		return err;
	}

	err = usbd_register_all_classes(ctx, speed, 1, class_blocklist);
	if (err) {
		LOG_ERR("Failed to register classes at speed %d: %d", speed, err);
	}

	return err;
}

static int mcuboot_usb_init(void)
{
	int err;

	err = usbd_add_descriptor(&mcuboot_usbd, &mcuboot_lang);
	if (err) {
		goto fail;
	}

	err = usbd_add_descriptor(&mcuboot_usbd, &mcuboot_mfr);
	if (err) {
		goto fail;
	}

	err = usbd_add_descriptor(&mcuboot_usbd, &mcuboot_product);
	if (err) {
		goto fail;
	}

	IF_ENABLED(CONFIG_HWINFO, (
		err = usbd_add_descriptor(&mcuboot_usbd, &mcuboot_sn);
		if (err) {
			goto fail;
		}
	));

	if (USBD_SUPPORTS_HIGH_SPEED &&
	    usbd_caps_speed(&mcuboot_usbd) == USBD_SPEED_HS) {
		err = register_classes(&mcuboot_usbd, USBD_SPEED_HS);
		if (err) {
			goto fail;
		}
	}

	err = register_classes(&mcuboot_usbd, USBD_SPEED_FS);
	if (err) {
		goto fail;
	}

	err = usbd_device_set_code_triple(&mcuboot_usbd, USBD_SPEED_FS,
					  USB_BCC_MISCELLANEOUS, 0x02, 0x01);
	if (err) {
		goto fail;
	}

	if (USBD_SUPPORTS_HIGH_SPEED) {
		err = usbd_device_set_code_triple(&mcuboot_usbd, USBD_SPEED_HS,
						  USB_BCC_MISCELLANEOUS, 0x02, 0x01);
		if (err) {
			goto fail;
		}
	}

	err = usbd_init(&mcuboot_usbd);
	if (err) {
		goto fail;
	}

	return 0;

fail:
	LOG_ERR("USB init failed: %d", err);
	return err;
}

SYS_INIT(mcuboot_usb_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
