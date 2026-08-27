/*
 * Left-half LED chain map, measured with the calibration chase
 * (src/led_calibration.c, ./build.sh --calib) on 2026-08-27.
 *
 * The strip snakes from the inner thumb up column 5, down column 4,
 * across the thumbs, and so on out to column 0. Chain index per key
 * position, laid out as the physical left half:
 *
 *     18  17  12  11   4   3
 *     19  16  13  10   5   2
 *     20  15  14   9   6   1
 *              8   7   0
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#define PIANTOR_LEFT_LED_COUNT 21

// Chain order as observed: chain index -> key position.
static const uint8_t piantor_left_chain_to_pos[PIANTOR_LEFT_LED_COUNT] = {
    38, 29, 17, 5, 4, 16, 28, 37, 36, 27, 15, 3, 2, 14, 26, 25, 13, 1, 0, 12, 24,
};

// Chain index for a left-half key position, or -1 for positions on the right half.
static inline int piantor_left_led_for_pos(uint8_t pos) {
    for (int i = 0; i < PIANTOR_LEFT_LED_COUNT; i++) {
        if (piantor_left_chain_to_pos[i] == pos) {
            return i;
        }
    }
    return -1;
}

// The five &bt BT_SEL 0..4 keys on the Keyboard layer sit at positions 1..5.
#define PIANTOR_BT_SEL_LED_0 17
#define PIANTOR_BT_SEL_LED_1 12
#define PIANTOR_BT_SEL_LED_2 11
#define PIANTOR_BT_SEL_LED_3 4
#define PIANTOR_BT_SEL_LED_4 3

// Battery gauges on the Keyboard layer: the two inner left thumbs.
// Position 37 (chain 7) shows this half, position 38 (chain 0) the right half.
#define PIANTOR_BAT_LEFT_LED 7
#define PIANTOR_BAT_RIGHT_LED 0
