# SPDX-License-Identifier: MIT
#
# Sources are added to ZMK's "app" target rather than a separate library so
# they see ZMK's private include dir (<zmk/...>, <drivers/ext_power.h>).

target_sources_ifdef(CONFIG_PIANTOR_BT_INDICATOR app PRIVATE src/bt_indicator_strip.c)
target_sources_ifdef(CONFIG_PIANTOR_LED_CALIBRATION app PRIVATE src/led_calibration.c)
