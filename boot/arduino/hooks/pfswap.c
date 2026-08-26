/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Normalise the PFM panel swap before MCUboot looks at a slot.
 *
 * The mechanism, the register sequence and the reason it is needed all live in
 * the variant's pfswap.h, shared with samples/dfu_boot rather than copied.
 * What is local here is only when to run it, and what to do with the result.
 *
 * Confirmed on hardware 2026-08-17: FCW.SWAP read 0x00000101 (BFSWAP and
 * PFSWAP both set) on a board whose BFM had been promoted once, and MCUboot
 * reported "Failed reading image headers" about a slot0 that had just been
 * written and verified byte-for-byte.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>

#include <pfswap.h>

#include "bootutil/bootutil_log.h"

/* Log into MCUboot's own module, the way its other files do; with
 * CONFIG_LOG=n (the shipped configuration) these calls compile away. */
BOOT_LOG_MODULE_DECLARE(mcuboot);

/*
 * 🔴 Only safe for a bootloader that runs from BFM.
 *
 * Changing PFSWAP requires that nothing is accessing either PFM panel (DS
 * 31.2.17.4). This runs at PRE_KERNEL_1 - before the flash driver initialises
 * at POST_KERNEL and long before boot_go() reads a slot - which guarantees that
 * only for an image that is not itself executing from PFM.
 *
 * The other arrangements in extra/mcuboot/ put MCUboot's body in PFM and are
 * built against this same module, so the check is a build error rather than a
 * comment: relocating the panels out from under the program counter is not
 * something to discover at runtime.
 */
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
