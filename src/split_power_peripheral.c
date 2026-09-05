/*
 * Peripheral side of the split power-state characteristic (split_power.h).
 *
 * The VBUS line is polled on a slow timer and combined with this half's own
 * gauge reading into a charge state; whenever that changes, the new value is
 * pushed to the central as a notification. The central also reads the
 * characteristic once after subscribing, so the value it sees is right from
 * the moment the link comes up.
 *
 * The full/charging decision is made here, on the gauge's raw reading,
 * because the level ZMK forwards to the central goes through the standard
 * Battery Service, which caps at 100 and drops readings above it.
 *
 * Polling keeps running while idle on purpose: the central may ask (by
 * holding its indicator layer) while this half has not seen a key press for
 * a while, and a register read every second costs nothing next to the BLE
 * connection events already waking the chip. Deep sleep powers the SoC off
 * and takes the timer with it.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/battery.h>

#include "split_power.h"
#include "vbus.h"

LOG_MODULE_REGISTER(split_power, CONFIG_ZMK_LOG_LEVEL);

static uint8_t power_state = PIANTOR_POWER_BATTERY;

static ssize_t read_power_state(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                                uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &power_state, sizeof(power_state));
}

static void power_state_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    LOG_DBG("Power state notifications %s", value == BT_GATT_CCC_NOTIFY ? "enabled" : "disabled");
}

BT_GATT_SERVICE_DEFINE(piantor_power_svc,
                       BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(PIANTOR_POWER_SERVICE_UUID)),
                       BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(PIANTOR_POWER_CHAR_UUID),
                                              BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_READ_ENCRYPT, read_power_state, NULL,
                                              &power_state),
                       BT_GATT_CCC(power_state_ccc_changed,
                                   BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT));

static void poll_work_cb(struct k_work *work) {
    uint8_t now = piantor_power_state_from(piantor_vbus_present(), zmk_battery_state_of_charge());

    if (now == power_state) {
        return;
    }
    power_state = now;
    LOG_DBG("Power state %u", now);

    // attrs[1] is the characteristic declaration; bt_gatt_notify resolves it
    // to the value attribute. Nobody connected is not an error.
    int err = bt_gatt_notify(NULL, &piantor_power_svc.attrs[1], &power_state, sizeof(power_state));
    if (err < 0 && err != -ENOTCONN) {
        LOG_WRN("Failed to notify power state (%d)", err);
    }
}

K_WORK_DEFINE(poll_work, poll_work_cb);

static void poll_timer_cb(struct k_timer *timer) { k_work_submit(&poll_work); }

K_TIMER_DEFINE(poll_timer, poll_timer_cb, NULL);

static int split_power_peripheral_init(void) {
    k_timer_start(&poll_timer, K_NO_WAIT, K_MSEC(CONFIG_PIANTOR_SPLIT_POWER_POLL_MS));
    return 0;
}

SYS_INIT(split_power_peripheral_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
