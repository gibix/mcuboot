/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal interface between the pieces of this module. MCUboot allows exactly
 * one mcuboot_status_change() implementation, so it lives in hooks.c alone and
 * everything else exposes a plain function for it to call.
 */

#ifndef ARDUINO_MCUBOOT_HOOKS_H_
#define ARDUINO_MCUBOOT_HOOKS_H_

/*
 * Start the breathing LED. Idempotent - the recovery entrance may be reported
 * more than once. No stop counterpart: entering recovery is terminal, the only
 * way out is a reset.
 */
void mcuboot_hooks_led_breathe(void);

/*
 * Install a sketch-only upload staged in slot1, if there is one. Called once
 * per boot from MCUBOOT_STATUS_STARTUP, which MCUboot reports before the
 * double-reset window and well before boot_go(). Silent and idempotent: with
 * nothing staged it reads one word and returns.
 */
void mcuboot_hooks_install_staged_sketch(void);

#endif /* ARDUINO_MCUBOOT_HOOKS_H_ */
