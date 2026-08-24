/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB DFU image backends for MCUboot recovery: three alternates
 * (alt 0 = Loader, alt 1 = Sketch, alt 2 = Bootloader/BFM).
 * Auto-resets after the last download completes.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_dfu.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/sys_io.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(dfu_images, CONFIG_USBD_DFU_LOG_LEVEL);

/* USBHS POWER register: SOFTCONN (bit 6) survives NVIC_SystemReset. */
#define USBHS_POWER_ADDR  (DT_REG_ADDR(DT_NODELABEL(zephyr_udc0)) + 0x1001U)
#define USBHS_SOFTCONN    BIT(6)

static void dfu_auto_reset_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	sys_write8(sys_read8(USBHS_POWER_ADDR) & ~USBHS_SOFTCONN,
		   USBHS_POWER_ADDR);
	__DSB();
	k_busy_wait(20000);

	sys_reboot(SYS_REBOOT_COLD);
}
static K_WORK_DELAYABLE_DEFINE(dfu_auto_reset, dfu_auto_reset_handler);

struct dfu_slot {
	const struct flash_area *fa;
	uint32_t offset;
	uint32_t erased_up_to;
	uint8_t fa_id;
};

#define ERASE_BLOCK DT_PROP(DT_INST(0, soc_nv_flash), erase_block_size)
#define WRITE_BLOCK DT_PROP(DT_INST(0, soc_nv_flash), write_block_size)

static int dfu_slot_read(void *const priv,
			 const uint32_t block, const uint16_t size,
			 uint8_t buf[static CONFIG_USBD_DFU_TRANSFER_SIZE])
{
	struct dfu_slot *slot = priv;
	uint32_t to_read;
	int ret;

	if (size == 0) {
		return 0;
	}

	if (block == 0) {
		ret = flash_area_open(slot->fa_id, &slot->fa);
		if (ret) {
			return ret;
		}
		slot->offset = 0;
	}

	if (slot->offset >= slot->fa->fa_size) {
		flash_area_close(slot->fa);
		slot->fa = NULL;
		return 0;
	}

	to_read = slot->fa->fa_size - slot->offset;
	if (to_read > size) {
		to_read = size;
	}

	ret = flash_area_read(slot->fa, slot->offset, buf, to_read);
	if (ret) {
		return ret;
	}

	slot->offset += to_read;
	return (int)to_read;
}

static int dfu_slot_write(void *const priv,
			  const uint32_t block, const uint16_t size,
			  const uint8_t buf[static CONFIG_USBD_DFU_TRANSFER_SIZE])
{
	struct dfu_slot *slot = priv;
	int ret;

	if (block == 0) {
		k_work_cancel_delayable(&dfu_auto_reset);
		ret = flash_area_open(slot->fa_id, &slot->fa);
		if (ret) {
			return ret;
		}
		slot->offset = 0;
		slot->erased_up_to = 0;
	}

	if (size == 0) {
		LOG_INF("DFU download complete: %u bytes to slot %u",
			slot->offset, slot->fa_id);
		flash_area_close(slot->fa);
		slot->fa = NULL;
		return 0;
	}

	if (slot->offset + size > slot->fa->fa_size) {
		LOG_ERR("DFU image too large for slot %u", slot->fa_id);
		return -ENOMEM;
	}

	/* Progressive erase: erase pages as we reach them. */
	uint32_t write_end = slot->offset + size;

	while (slot->erased_up_to < write_end) {
		uint32_t erase_off = ROUND_DOWN(slot->erased_up_to, ERASE_BLOCK);
		uint32_t erase_len = ERASE_BLOCK;

		if (erase_off + erase_len > slot->fa->fa_size) {
			erase_len = slot->fa->fa_size - erase_off;
		}

		ret = flash_area_erase(slot->fa, erase_off, erase_len);
		if (ret) {
			LOG_ERR("Erase failed at offset 0x%x: %d", erase_off, ret);
			return ret;
		}
		slot->erased_up_to = erase_off + erase_len;
	}

	uint16_t write_size = size;

	if (write_size % WRITE_BLOCK != 0) {
		write_size = ROUND_UP(write_size, WRITE_BLOCK);
		/*
		 * The extra bytes in buf[] beyond 'size' may be garbage; fill
		 * them with the erased value. Safe because the DFU framework
		 * allocates buf at CONFIG_USBD_DFU_TRANSFER_SIZE which is
		 * always >= write_size after alignment.
		 */
		uint8_t ev = flash_area_erased_val(slot->fa);

		memset((uint8_t *)buf + size, ev, write_size - size);
	}

	ret = flash_area_write(slot->fa, slot->offset, buf, write_size);
	if (ret) {
		LOG_ERR("Write failed at offset 0x%x: %d", slot->offset, ret);
		return ret;
	}

	slot->offset += size;
	return 0;
}

static bool dfu_slot_next(void *const priv,
			  const enum usb_dfu_state state,
			  const enum usb_dfu_state next)
{
	if (state == DFU_MANIFEST_SYNC && next == DFU_IDLE) {
		LOG_INF("DFU manifestation complete");
		k_work_reschedule(&dfu_auto_reset, K_SECONDS(2));
	}
	return true;
}

/* Image 0 primary: the loader (288 K at 0x0C000000). */
static struct dfu_slot slot0_data = {
	.fa_id = PARTITION_ID(slot0_partition),
};
USBD_DFU_DEFINE_IMG(slot0, "Loader", &slot0_data,
		    dfu_slot_read, dfu_slot_write, dfu_slot_next);

/* Image 1 primary: the sketch (728 K at 0x0C048000). */
static struct dfu_slot slot2_data = {
	.fa_id = PARTITION_ID(slot2_partition),
};
USBD_DFU_DEFINE_IMG(slot2, "Sketch", &slot2_data,
		    dfu_slot_read, dfu_slot_write, dfu_slot_next);

#ifdef HAS_BFM_FLASH
/*
 * Alt 2: direct write to the spare BFM Dual-Boot slot (0x08010000).
 *
 * Replaces the staging mechanism (upload .bfmpkg to slot2, loader installs on
 * next boot) with a single DFU transfer that goes straight to BFM. No header,
 * no CRC wrapper, no sketch clobbering.
 *
 * On manifestation (size == 0): flush, validate the vector table, promote the
 * spare slot by bumping its sequence number, then auto-reset into the new
 * bootloader.
 */
#include "bfm_flash.h"
#include "bfm_boot.h"

static int bfm_dfu_read(void *const priv,
			 const uint32_t block, const uint16_t size,
			 uint8_t buf[static CONFIG_USBD_DFU_TRANSFER_SIZE])
{
	ARG_UNUSED(priv);
	ARG_UNUSED(block);
	ARG_UNUSED(size);
	ARG_UNUSED(buf);
	return 0;
}

static int bfm_dfu_write(void *const priv,
			  const uint32_t block, const uint16_t size,
			  const uint8_t buf[static CONFIG_USBD_DFU_TRANSFER_SIZE])
{
	ARG_UNUSED(priv);
	int ret;

	if (block == 0) {
		k_work_cancel_delayable(&dfu_auto_reset);
		ret = bfm_stream_begin(BFM_SPARE_SLOT, BFM_SLOT_SIZE);
		if (ret) {
			LOG_ERR("BFM stream begin failed: %d", ret);
			return ret;
		}
	}

	if (size == 0) {
		ret = bfm_stream_finish();
		if (ret) {
			LOG_ERR("BFM stream finish failed: %d", ret);
			return ret;
		}
		if (!bfm_boot_spare_image_valid()) {
			LOG_ERR("BFM spare image not bootable");
			return -ENOEXEC;
		}
		ret = bfm_boot_promote_spare();
		if (ret) {
			LOG_ERR("BFM promote failed: %d", ret);
			return ret;
		}
		LOG_INF("BFM update complete: %u bytes, spare promoted",
			bfm_stream_written());
		return 0;
	}

	return bfm_stream_write(buf, size);
}

/* "slot_bfm" sorts after "slot0" and "slot2" (ASCII '_' > '2'), keeping
 * the existing alt assignments: 0=Loader, 1=Sketch, 2=Bootloader.
 */
USBD_DFU_DEFINE_IMG(slot_bfm, "Bootloader", NULL,
		    bfm_dfu_read, bfm_dfu_write, dfu_slot_next);
#endif /* HAS_BFM_FLASH */
