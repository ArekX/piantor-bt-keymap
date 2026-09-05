/*
 * Charge state of one half, as shown on its battery LED and as sent by the
 * right half over the split link (split_power.h).
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

enum piantor_power_state {
    PIANTOR_POWER_UNKNOWN = -1, // right half not connected / not answered yet
    PIANTOR_POWER_BATTERY = 0,  // no USB power
    PIANTOR_POWER_CHARGING = 1, // USB power, level below the full threshold
    PIANTOR_POWER_FULL = 2,     // USB power, level at or above it
};

// Each half decides for itself from its own VBUS line and its own gauge
// reading, so the level never has to travel anywhere to make this call.
static inline enum piantor_power_state piantor_power_state_from(bool vbus, int soc) {
    if (!vbus) {
        return PIANTOR_POWER_BATTERY;
    }
    return soc >= CONFIG_PIANTOR_BATTERY_FULL_PERCENT ? PIANTOR_POWER_FULL
                                                      : PIANTOR_POWER_CHARGING;
}
