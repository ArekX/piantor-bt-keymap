/*
 * Right half's charge state, carried over the split BLE link.
 *
 * The peripheral publishes one byte (an enum piantor_power_state, never
 * UNKNOWN) as a custom GATT characteristic next to ZMK's split service; the
 * central subscribes to it the same way ZMK subscribes to the peripheral's
 * battery level. See split_power_peripheral.c and split_power_central.c.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/bluetooth/uuid.h>

#include "power_state.h"

#define PIANTOR_POWER_UUID(num) BT_UUID_128_ENCODE(num, 0x5a3c, 0x4d8e, 0x9f61, 0x2c4e8a7d1b90)
#define PIANTOR_POWER_SERVICE_UUID PIANTOR_POWER_UUID(0x7b2f0000)
#define PIANTOR_POWER_CHAR_UUID PIANTOR_POWER_UUID(0x7b2f0001)

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
// Last state the right half reported, or PIANTOR_POWER_UNKNOWN while it is
// disconnected, has not answered yet, or runs firmware without the
// characteristic.
enum piantor_power_state piantor_peripheral_power_state(void);
#endif
