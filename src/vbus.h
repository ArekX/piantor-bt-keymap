/*
 * USB power (VBUS) detection shared by both halves.
 *
 * ZMK's own zmk_usb_is_powered() only exists on the central: CONFIG_ZMK_USB
 * is not selectable on a split peripheral. The nRF52840 exposes the VBUS
 * comparator directly in POWER.USBREGSTATUS, so read that instead; it works
 * the same on both halves and with or without the USB stack.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>

#include <hal/nrf_power.h>

#if NRF_POWER_HAS_USBREG
static inline bool piantor_vbus_present(void) {
    return nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
}
#else
#error "piantor_vbus_present() needs an nRF SoC with a USB regulator (nRF52840)"
#endif
