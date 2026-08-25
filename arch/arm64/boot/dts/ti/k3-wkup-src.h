/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Device tree constants for wakeup source values used in the TI K3 IPC IRQ
 * controller.
 *
 * Copyright (C) 2026 Texas Instruments Incorporated - https://www.ti.com/
 */

#ifndef DTS_ARM64_TI_K3_WKUP_SRC_H
#define DTS_ARM64_TI_K3_WKUP_SRC_H

/*
 * Wakeup source bitmask values.
 */
#define AM62L_WKUP_SRC_WKUP_I2C0	0x00001
#define AM62L_WKUP_SRC_WKUP_UART0	0x00002
#define AM62L_WKUP_SRC_WKUP_GPIO0	0x00004
#define AM62L_WKUP_SRC_WKUP_TIMER0	0x00020
#define AM62L_WKUP_SRC_WKUP_TIMER1	0x00040
#define AM62L_WKUP_SRC_WKUP_RTC0	0x00080
#define AM62L_WKUP_SRC_USB0		0x00200
#define AM62L_WKUP_SRC_USB1		0x00400
#define AM62L_WKUP_SRC_MAIN_IO		0x10000
#define AM62L_WKUP_SRC_WKUP_IO		0x20000
#define AM62L_WKUP_SRC_RTC_IO		0x40000

/*
 * TI_K3_WKUP_HWIRQ - construct the wakeup source interrupt specifier
 * @src:  one of the WKUP_SRC_* values above
 * @pin:  8-bit wakeup pin index; 0 for sources with no associated pin
 *
 * For IO daisy chain sources (WKUP_SRC_MAIN_IO, WKUP_SRC_WKUP_IO),
 * the pin index is the pad that triggered the wakeup. For all other sources
 * set pin to 0.
 */
#define TI_K3_WKUP_HWIRQ(src, pin)	(((src) << 8) | ((pin) & 0xff))

#endif /* DTS_ARM64_TI_K3_WKUP_SRC_H */
