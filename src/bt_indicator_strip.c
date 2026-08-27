/*
 * Bluetooth profile indicator, implemented as an LED strip shim.
 *
 * ZMK's underglow thinks this is the strip. Every frame it renders comes
 * through update_rgb() below and is forwarded to the real WS2812 driver.
 * While the Keyboard layer is active, the five pixels under the BT_SEL keys
 * are overwritten first. Color answers "which profile am I on"; only the
 * active profile gets a bright, warm color:
 *
 *   active profile:  green - host link is up (keystrokes go here)
 *                    amber - bonded, but the host is not reachable
 *                    red   - empty, advertising for a new host
 *   other profiles:  dim blue  - bonded
 *                    dim white - empty
 *
 * Note that several bonded hosts can hold a BLE link at once (BT_MAX_CONN),
 * so "link up" is deliberately not shown for non-active profiles: it would
 * light every awake Mac in the room green.
 *
 * The other pixels keep whatever the underglow effect drew, so the glow
 * keeps running.
 *
 * When underglow is toggled off it stops producing frames and, with
 * CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER, cuts the LED rail. So that the
 * indicator still works then, a slow tick runs only while the layer is held:
 * if underglow is on the tick is a no-op; if it is off, the tick powers the
 * rail, draws a black frame with just the indicator, and pushes it itself.
 * Releasing the layer blanks the strip and cuts the rail again. Outside the
 * layer, and whenever underglow is on, this file adds no timers or wakeups.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT piantor_bt_indicator_strip

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/ext_power.h>
#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/rgb_underglow.h>
#include <zmk/workqueue.h>

#include "led_map.h"

LOG_MODULE_REGISTER(bt_indicator, CONFIG_ZMK_LOG_LEVEL);

struct bt_indicator_cfg {
    const struct device *strip;
    size_t length;
};

// Chain index of the LED under each &bt BT_SEL n key on the Keyboard layer.
static const uint8_t bt_sel_leds[] = {
    PIANTOR_BT_SEL_LED_0, PIANTOR_BT_SEL_LED_1, PIANTOR_BT_SEL_LED_2,
    PIANTOR_BT_SEL_LED_3, PIANTOR_BT_SEL_LED_4,
};

#define PCT(v, pct) ((uint8_t)(((uint32_t)(v) * (pct)) / 100))

/* ---- Rendering ---------------------------------------------------------- */

static struct led_rgb profile_color(uint8_t index, bool active) {
    bool bonded = !zmk_ble_profile_is_open(index);

    if (!active) {
        uint8_t brt = PCT(255, CONFIG_PIANTOR_BT_INDICATOR_BRT_INACTIVE);
        return bonded ? (struct led_rgb){.r = 0, .g = 0, .b = brt}
                      : (struct led_rgb){.r = brt, .g = brt, .b = brt};
    }

    uint8_t brt = PCT(255, CONFIG_PIANTOR_BT_INDICATOR_BRT_ACTIVE);
    if (!bonded) {
        return (struct led_rgb){.r = brt, .g = 0, .b = 0};
    }
    if (zmk_ble_profile_is_connected(index)) {
        return (struct led_rgb){.r = 0, .g = brt, .b = 0};
    }
    return (struct led_rgb){.r = brt, .g = PCT(brt, 45), .b = 0};
}

static void overlay_indicator(struct led_rgb *pixels, size_t num_pixels) {
#if CONFIG_PIANTOR_BT_INDICATOR_OTHERS_PERCENT < 100
    for (size_t i = 0; i < num_pixels; i++) {
        pixels[i].r = PCT(pixels[i].r, CONFIG_PIANTOR_BT_INDICATOR_OTHERS_PERCENT);
        pixels[i].g = PCT(pixels[i].g, CONFIG_PIANTOR_BT_INDICATOR_OTHERS_PERCENT);
        pixels[i].b = PCT(pixels[i].b, CONFIG_PIANTOR_BT_INDICATOR_OTHERS_PERCENT);
    }
#endif

    int active = zmk_ble_active_profile_index();

    for (uint8_t i = 0; i < ARRAY_SIZE(bt_sel_leds) && i < ZMK_BLE_PROFILE_COUNT; i++) {
        uint8_t led = bt_sel_leds[i];
        if (led < num_pixels) {
            pixels[led] = profile_color(i, i == active);
        }
    }
}

static inline bool indicator_layer_active(void) {
    return zmk_keymap_layer_active(CONFIG_PIANTOR_BT_INDICATOR_LAYER);
}

/* ---- Strip shim (underglow on) ------------------------------------------ */

static int bt_indicator_update_rgb(const struct device *dev, struct led_rgb *pixels,
                                   size_t num_pixels) {
    const struct bt_indicator_cfg *cfg = dev->config;

    if (indicator_layer_active()) {
        overlay_indicator(pixels, num_pixels);
    }

    return led_strip_update_rgb(cfg->strip, pixels, num_pixels);
}

static int bt_indicator_update_channels(const struct device *dev, uint8_t *channels,
                                        size_t num_channels) {
    const struct bt_indicator_cfg *cfg = dev->config;
    return led_strip_update_channels(cfg->strip, channels, num_channels);
}

/* ---- Self-driven frames (underglow off) --------------------------------- */

// Single instance; the layer listener has no device handle to work from.
static const struct device *self;

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_ext_power_generic)
static const struct device *const ext_power = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#define HAS_EXT_POWER 1
#else
#define HAS_EXT_POWER 0
#endif

static struct led_rgb own_frame[PIANTOR_LEFT_LED_COUNT];
// True while we hold the LED rail up on underglow's behalf.
static bool rail_raised;

static bool underglow_is_on(void) {
    bool on = false;
    return zmk_rgb_underglow_get_state(&on) == 0 && on;
}

static void raise_rail(void) {
#if HAS_EXT_POWER
    if (rail_raised || !device_is_ready(ext_power) || ext_power_get(ext_power) > 0) {
        return;
    }
    int rc = ext_power_enable(ext_power);
    if (rc == 0) {
        rail_raised = true;
    } else {
        LOG_ERR("Unable to enable EXT_POWER: %d", rc);
    }
#endif
}

static void drop_rail(void) {
#if HAS_EXT_POWER
    if (!rail_raised) {
        return;
    }
    rail_raised = false;
    // If underglow came on while we held the layer, it owns the rail now.
    if (underglow_is_on()) {
        return;
    }
    int rc = ext_power_disable(ext_power);
    if (rc != 0) {
        LOG_ERR("Unable to disable EXT_POWER: %d", rc);
    }
#endif
}

static void push_own_frame(bool blank) {
    const struct bt_indicator_cfg *cfg = self->config;

    memset(own_frame, 0, sizeof(own_frame));
    if (!blank) {
        overlay_indicator(own_frame, cfg->length);
    }

    int err = led_strip_update_rgb(cfg->strip, own_frame, cfg->length);
    if (err < 0) {
        LOG_ERR("Failed to update the LED strip (%d)", err);
    }
}

static void own_tick_work(struct k_work *work) {
    if (!self || !indicator_layer_active()) {
        return;
    }
    if (underglow_is_on()) {
        // Underglow's own frames carry the overlay; nothing to do. If we had
        // raised the rail earlier, underglow has since taken it over.
        rail_raised = false;
        return;
    }
    raise_rail();
    push_own_frame(false);
}

static void layer_exit_work(struct k_work *work) {
    if (!self || underglow_is_on()) {
        return;
    }
    push_own_frame(true);
    drop_rail();
}

K_WORK_DEFINE(own_tick, own_tick_work);
K_WORK_DEFINE(layer_exit, layer_exit_work);

static void own_timer_handler(struct k_timer *timer) {
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &own_tick);
}

K_TIMER_DEFINE(own_timer, own_timer_handler, NULL);

static int bt_indicator_layer_listener(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    if (ev == NULL || ev->layer != CONFIG_PIANTOR_BT_INDICATOR_LAYER) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->state) {
        k_timer_start(&own_timer, K_NO_WAIT, K_MSEC(CONFIG_PIANTOR_BT_INDICATOR_REFRESH_MS));
    } else {
        k_timer_stop(&own_timer);
        k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &layer_exit);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(bt_indicator, bt_indicator_layer_listener);
ZMK_SUBSCRIPTION(bt_indicator, zmk_layer_state_changed);

/* ---- Device ------------------------------------------------------------- */

static int bt_indicator_init(const struct device *dev) {
    const struct bt_indicator_cfg *cfg = dev->config;

    if (!device_is_ready(cfg->strip)) {
        LOG_ERR("Wrapped LED strip %s is not ready", cfg->strip->name);
        return -ENODEV;
    }

    self = dev;
    return 0;
}

static const struct led_strip_driver_api bt_indicator_api = {
    .update_rgb = bt_indicator_update_rgb,
    .update_channels = bt_indicator_update_channels,
};

#define BT_INDICATOR_DEVICE(n)                                                                    \
    BUILD_ASSERT(DT_INST_PROP(n, chain_length) == PIANTOR_LEFT_LED_COUNT,                          \
                 "chain-length does not match the measured LED map");                              \
    static const struct bt_indicator_cfg bt_indicator_cfg_##n = {                                  \
        .strip = DEVICE_DT_GET(DT_INST_PHANDLE(n, led_strip)),                                     \
        .length = DT_INST_PROP(n, chain_length),                                                   \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, bt_indicator_init, NULL, NULL, &bt_indicator_cfg_##n, POST_KERNEL,     \
                          CONFIG_PIANTOR_BT_INDICATOR_INIT_PRIORITY, &bt_indicator_api);

DT_INST_FOREACH_STATUS_OKAY(BT_INDICATOR_DEVICE)
