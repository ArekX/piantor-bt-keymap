/*
 * LED chain calibration chase.
 *
 * Lights one LED at a time, walking the chain from index 0 to the end and
 * wrapping. Flash it once, note which key lights up at each step, and you
 * have the chain-index -> key-position table that the DTS does not provide
 * (it only says chain-length = 21). Not meant for daily use.
 *
 * The keymap is untouched, so the keyboard keeps typing while this runs.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/ext_power.h>
#include <zmk/workqueue.h>

LOG_MODULE_REGISTER(piantor_led_calib, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
#error "LED calibration owns the strip; build with CONFIG_ZMK_RGB_UNDERGLOW=n"
#endif

#if !DT_HAS_CHOSEN(zmk_underglow)
#error "A zmk,underglow chosen node is required for LED calibration"
#endif

#define STRIP_NODE DT_CHOSEN(zmk_underglow)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_NODE, chain_length)

#define BRT CONFIG_PIANTOR_LED_CALIBRATION_BRIGHTNESS

static const struct device *const led_strip = DEVICE_DT_GET(STRIP_NODE);

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_ext_power_generic)
#define HAS_EXT_POWER 1
static const struct device *const ext_power = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#else
#define HAS_EXT_POWER 0
#endif

static struct led_rgb pixels[STRIP_NUM_PIXELS];
static int current = 0;

// The LED rail sits behind EXT_POWER. It is on by default at boot, but the
// normal firmware saves the underglow-off state to flash and settings load
// (which runs *after* our init) may cut it again. Re-check on every step;
// ext_power_get() is just a status read, and ext_power_enable() only writes
// flash when it actually flips the state.
static void ensure_ext_power(void) {
#if HAS_EXT_POWER
    if (!device_is_ready(ext_power) || ext_power_get(ext_power) > 0) {
        return;
    }
    int rc = ext_power_enable(ext_power);
    if (rc != 0) {
        LOG_ERR("Unable to enable EXT_POWER: %d", rc);
    }
#endif
}

static void calib_step(struct k_work *work) {
    ensure_ext_power();

    memset(pixels, 0, sizeof(pixels));
    pixels[current] = (struct led_rgb){.r = BRT, .g = BRT, .b = BRT};

    int err = led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
    if (err < 0) {
        LOG_ERR("Failed to update the LED strip (%d)", err);
    } else {
        LOG_INF("LED calibration: chain index %d of 0..%d", current, STRIP_NUM_PIXELS - 1);
    }

    current = (current + 1) % STRIP_NUM_PIXELS;
}

K_WORK_DEFINE(calib_work, calib_step);

static void calib_tick(struct k_timer *timer) {
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &calib_work);
}

K_TIMER_DEFINE(calib_timer, calib_tick, NULL);

static int calib_init(void) {
    if (!device_is_ready(led_strip)) {
        LOG_ERR("LED strip device is not ready");
        return -ENODEV;
    }

    // First frame after a second so power and settings have settled.
    k_timer_start(&calib_timer, K_SECONDS(1), K_MSEC(CONFIG_PIANTOR_LED_CALIBRATION_STEP_MS));
    return 0;
}

SYS_INIT(calib_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
