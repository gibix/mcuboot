/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Install a sketch-only upload into the primary slot.
 *
 * Uploading a sketch normally means sending the whole loader+sketch bundle
 * (~280 KB of loader for ~20 KB of sketch), because the sketch is not its own
 * flash region: it is a window *inside* slot0's single MCUboot image, and
 * rewriting the middle of a hashed image invalidates it. This installs just the
 * sketch instead, and keeps CONFIG_BOOT_VALIDATE_SLOT0=y.
 *
 * Why this runs in MCUboot rather than in the loader
 * --------------------------------------------------
 * Two reasons, and the first is hardware.
 *
 * PFM is two panels, and the datasheet is explicit that read-while-write only
 * works ACROSS panels (DS 31.3.11.1: "it is possible to execute from one Flash
 * panel while programming another"). slot0 is one panel and slot1 is the other,
 * so a loader running from slot0 cannot rewrite the sketch window - it is in
 * the panel it is executing from. The arbiter stalls the CPU rather than
 * faulting (DS 31.3.5.2), so it would half-work, which is worse. MCUboot runs
 * from BFM and writes PFM freely.
 *
 * Second, MCUboot is where the image is validated, so installing here means the
 * very next thing that happens is boot_go() checking the result.
 *
 * What the host sends, and why the board does no hashing
 * -----------------------------------------------------
 * The host already has the exact loader binary it built the bundle from, so it
 * can compute the finished image - header, hash and all - without the board's
 * help. It sends only the parts that differ from what is already in slot0:
 *
 *     struct sketch_stage_hdr      magic / body_len / crc32 / pad
 *     uint8_t page0[4096]          slot0's first page, with the image header's
 *                                  ih_img_size updated for the new sketch
 *     uint8_t body[body_len]       the sketch, immediately followed by the
 *                                  image's TLV block (SHA256)
 *
 * The TLV directly follows the sketch in this layout - the bundle is
 * [header | loader | 0xFF pad to SKETCH_OFF | sketch | TLV] - so the sketch and
 * the TLV are one contiguous write at SKETCH_OFF. That is the whole reason this
 * file needs no SHA-256 and no read-modify-write of flash: two erase/write
 * regions and nothing else.
 *
 * The image header lives in the same 4 KB erase page as the first of the
 * loader's code, which is why the host sends the whole page rather than 32
 * bytes.
 *
 * When it goes wrong
 * ------------------
 * Every failure ends with slot0 failing its hash check, which drops the board
 * into serial recovery - where a full bundle upload fixes it. That is the
 * safety net that lets this code stay small: it does not have to prove the
 * result is good, boot_go() does that a few hundred microseconds later.
 *
 * The one failure worth catching early is a delta built against a DIFFERENT
 * loader than the one on the board - after a core update, say. Installing it
 * would produce an image whose hash cannot match, and the board would drop into
 * recovery for what is really a host-side mistake. So the package carries a
 * CRC of the loader region it assumes, and a mismatch refuses the install and
 * leaves slot0 exactly as it was: the board keeps booting the sketch it had.
 * The host notices because the image hash it reads back afterwards is not the
 * one it expected, and can fall back to a full bundle upload.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include "hooks.h"

#define STAGE_MAGIC 0x50554B53U /* "SKUP" in flash order */

struct sketch_stage_hdr {
	uint32_t magic;
	uint32_t body_len;
	uint32_t crc;        /* over page0 || body */
	uint32_t loader_crc; /* over the slot0 loader region this delta assumes */
};

/*
 * Where the sketch window starts inside slot0. Must match build.ota.offset in
 * boards.txt and the user_sketch partition in the variant's -common.overlay;
 * the host tool pads the loader out to exactly this before appending.
 */
#define SKETCH_OFF 0x48000U

#define PAGE_SZ 4096U

/* Upper bound on body_len: capped by what fits in the 128K staging area
 * after the 16-byte header and the 4K page0 copy. */
#define BODY_MAX (0x20000U - 16U - PAGE_SZ)

/*
 * The loader region this delta leaves untouched: everything between the first
 * page (which the package replaces wholesale) and the sketch window.
 */
#define LOADER_CRC_OFF PAGE_SZ
#define LOADER_CRC_LEN (SKETCH_OFF - PAGE_SZ)

/*
 * 🔴 __aligned is NOT decoration. Row programming hands the flash controller
 * this buffer's address in FCW_SRCADDR and lets the controller fetch from SRAM
 * itself, and per DS 31 (SRCADDR) "the bottom four bits, SRCADDR[3:0], are
 * ignored" - an unaligned buffer is silently read from the address below it.
 * Nothing reports an error; the data simply lands shifted.
 *
 * Cost of finding that out: this buffer linked at 0x2000344b, and every write
 * went into flash three bytes late, which presented as a corrupt slot0 that
 * MCUboot then refused to boot. The same __aligned(8) appears on bfm_stage and
 * stream_row in bfm/bfm_flash.c for the same reason.
 */
static uint8_t copy_buf[PAGE_SZ] __aligned(32);

/*
 * Stream `len` bytes out of the staging area, CRC-ing them and optionally
 * writing them into slot0 at `dst`. Two passes over the same data: once to
 * check the CRC before anything is erased, once to install it.
 */
static int stage_stream(const struct flash_area *src, size_t src_off, size_t len,
			const struct flash_area *dst, size_t dst_off, uint32_t *crc)
{
	size_t done = 0;

	while (done < len) {
		size_t n = MIN(sizeof(copy_buf), len - done);
		int rc = flash_area_read(src, src_off + done, copy_buf, n);

		if (rc) {
			return rc;
		}
		if (crc) {
			*crc = crc32_ieee_update(*crc, copy_buf, n);
		}
		if (dst) {
			/* Pad a short tail to the write-block size with the
			 * erased value, so the write never reads past the
			 * buffer and the result is what a full page would be. */
			size_t w = ROUND_UP(n, 8);

			if (w > n) {
				memset(copy_buf + n, 0xFF, w - n);
			}
			rc = flash_area_write(dst, dst_off + done, copy_buf, w);
			if (rc) {
				return rc;
			}
		}
		done += n;
	}

	return 0;
}

static int stage_invalidate(const struct flash_area *fa)
{
	/* Erasing the first page is enough: the magic is the first word, and
	 * nothing reads the rest without it. */
	return flash_area_erase(fa, 0, PAGE_SZ);
}

void mcuboot_hooks_install_staged_sketch(void)
{
	const struct flash_area *stage = NULL;
	const struct flash_area *slot0 = NULL;
	struct sketch_stage_hdr hdr;
	size_t body_off = sizeof(hdr) + PAGE_SZ;
	uint32_t crc = 0;
	int rc;

	if (flash_area_open(FIXED_PARTITION_ID(slot1_partition), &stage)) {
		return;
	}

	if (flash_area_read(stage, 0, &hdr, sizeof(hdr)) || hdr.magic != STAGE_MAGIC) {
		goto out;
	}

	if (hdr.body_len == 0U || hdr.body_len > BODY_MAX) {
		/* Not something this bootloader can install; drop it rather
		 * than retrying it on every boot forever. */
		(void)stage_invalidate(stage);
		goto out;
	}

	/* Check the whole package before erasing anything in slot0. */
	rc = stage_stream(stage, sizeof(hdr), PAGE_SZ + hdr.body_len, NULL, 0, &crc);
	if (rc || crc != hdr.crc) {
		(void)stage_invalidate(stage);
		goto out;
	}

	if (flash_area_open(FIXED_PARTITION_ID(slot0_partition), &slot0)) {
		goto out;
	}

	/*
	 * Refuse a delta built against a different loader. Leaves slot0 alone,
	 * so the board goes on booting what it has; the staging area is dropped
	 * so this is not retried on every boot.
	 */
	crc = 0;
	rc = stage_stream(slot0, LOADER_CRC_OFF, LOADER_CRC_LEN, NULL, 0, &crc);
	if (rc || crc != hdr.loader_crc) {
		(void)stage_invalidate(stage);
		goto out;
	}

	/*
	 * Body first, page 0 last. Both orders leave a slot0 that fails its
	 * hash if power is lost between them - recovery is the answer either
	 * way - but this order means the image header only starts claiming the
	 * new size once the bytes it describes are already there.
	 */
	rc = flash_area_erase(slot0, SKETCH_OFF, ROUND_UP(hdr.body_len, PAGE_SZ));
	if (rc) {
		goto out;
	}
	rc = stage_stream(stage, body_off, hdr.body_len, slot0, SKETCH_OFF, NULL);
	if (rc) {
		goto out;
	}

	rc = flash_area_erase(slot0, 0, PAGE_SZ);
	if (rc) {
		goto out;
	}
	rc = stage_stream(stage, sizeof(hdr), PAGE_SZ, slot0, 0, NULL);
	if (rc) {
		goto out;
	}

	(void)stage_invalidate(stage);

out:
	if (slot0) {
		flash_area_close(slot0);
	}
	if (stage) {
		flash_area_close(stage);
	}
}
