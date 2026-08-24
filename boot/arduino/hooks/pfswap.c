/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Normalise PFM panel swap before MCUboot reads any slot.
 * The mechanism lives in the variant's pfswap.h.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>

#include <pfswap.h>

#include "bootutil/bootutil_log.h"

/* Log into MCUboot's own module, the way its other files do; with
 * CONFIG_LOG=n (the shipped configuration) these calls compile away. */
BOOT_LOG_MODULE_DECLARE(mcuboot);

/* Only safe from BFM — changing PFSWAP while executing from PFM is undefined. */
BUILD_ASSERT(CONFIG_FLASH_BASE_ADDRESS == 0x08000000,
	     "pfswap.c may only be linked into a bootloader running from BFM; "
	     "this image is linked elsewhere and would move the PFM panels "
	     "out from under itself");

static int normalize_pfswap(void)
{
	switch (pfm_normalize_swap()) {
	case PFM_SWAP_NORMALIZED:
		BOOT_LOG_INF("PFSWAP normalized");
		break;
	case PFM_SWAP_LOCKED:
		BOOT_LOG_ERR("PFSWAP set and locked; PFM addresses stay shifted");
		break;
	case PFM_SWAP_FAILED:
		BOOT_LOG_ERR("PFSWAP would not clear");
		break;
	case PFM_SWAP_ALREADY_NORMAL:
		break;
	}

	return 0;
}
SYS_INIT(normalize_pfswap, PRE_KERNEL_1, 0);
