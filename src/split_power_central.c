/*
 * Central side of the split power-state characteristic (split_power.h).
 *
 * Every BLE link the central opens itself (role CENTRAL) is a peripheral
 * half; host links are role PERIPHERAL and ignored. When such a link comes
 * up, the characteristic is looked up by UUID, subscribed to for
 * notifications, and read once for the current value. This runs alongside
 * ZMK's own discovery on the same link; the ATT layer queues requests, so
 * the two do not interfere.
 *
 * Only one peripheral is tracked, which is all this keyboard has.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "split_power.h"

LOG_MODULE_REGISTER(split_power, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS == 1,
             "split_power_central.c tracks a single peripheral");

static const struct bt_uuid_128 power_char_uuid = BT_UUID_INIT_128(PIANTOR_POWER_CHAR_UUID);

// The right half's link, with a reference held while it is up.
static struct bt_conn *peer;
static enum piantor_power_state peer_state = PIANTOR_POWER_UNKNOWN;

static struct bt_gatt_discover_params discover_params;
static struct bt_gatt_discover_params ccc_discover_params;
static struct bt_gatt_subscribe_params subscribe_params;
static struct bt_gatt_read_params read_params;

enum piantor_power_state piantor_peripheral_power_state(void) { return peer_state; }

static void set_state(const void *data, uint16_t length) {
    if (data == NULL || length < 1) {
        return;
    }
    uint8_t raw = ((const uint8_t *)data)[0];
    if (raw > PIANTOR_POWER_FULL) {
        LOG_WRN("Unknown peripheral power state %u", raw);
        return;
    }
    peer_state = (enum piantor_power_state)raw;
    LOG_DBG("Peripheral power state %u", raw);
}

static uint8_t power_notify_func(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                                const void *data, uint16_t length) {
    if (data == NULL) {
        LOG_DBG("Power state unsubscribed");
        params->value_handle = 0;
        return BT_GATT_ITER_STOP;
    }
    set_state(data, length);
    return BT_GATT_ITER_CONTINUE;
}

static uint8_t power_read_func(struct bt_conn *conn, uint8_t err, struct bt_gatt_read_params *params,
                              const void *data, uint16_t length) {
    if (err != 0) {
        LOG_WRN("Failed to read peripheral power state (ATT error %u)", err);
        return BT_GATT_ITER_STOP;
    }
    set_state(data, length);
    return BT_GATT_ITER_STOP;
}

static uint8_t power_discover_func(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  struct bt_gatt_discover_params *params) {
    if (attr == NULL) {
        // Reached the end of the handle range without a match: the right
        // half runs firmware without the characteristic. Stay at unknown.
        LOG_INF("Peripheral has no power-state characteristic");
        memset(params, 0, sizeof(*params));
        return BT_GATT_ITER_STOP;
    }

    uint16_t value_handle = bt_gatt_attr_value_handle(attr);
    LOG_DBG("Found power-state characteristic, value handle %u", value_handle);

    subscribe_params.value_handle = value_handle;
    subscribe_params.ccc_handle = 0; // let the host discover the CCC
    subscribe_params.disc_params = &ccc_discover_params;
    subscribe_params.end_handle = 0xffff;
    subscribe_params.notify = power_notify_func;
    subscribe_params.value = BT_GATT_CCC_NOTIFY;
    atomic_set(subscribe_params.flags, BT_GATT_SUBSCRIBE_FLAG_NO_RESUB);

    int err = bt_gatt_subscribe(conn, &subscribe_params);
    if (err != 0 && err != -EALREADY) {
        LOG_WRN("Failed to subscribe to peripheral power state (%d)", err);
    }

    read_params.func = power_read_func;
    read_params.handle_count = 1;
    read_params.single.handle = value_handle;
    read_params.single.offset = 0;

    err = bt_gatt_read(conn, &read_params);
    if (err != 0) {
        LOG_WRN("Failed to read peripheral power state (%d)", err);
    }

    return BT_GATT_ITER_STOP;
}

static void power_connected(struct bt_conn *conn, uint8_t err) {
    struct bt_conn_info info;

    if (err != 0 || bt_conn_get_info(conn, &info) != 0 || info.role != BT_CONN_ROLE_CENTRAL) {
        return;
    }
    if (peer != NULL) {
        LOG_WRN("Second central-role link; only the first peripheral is tracked");
        return;
    }

    peer = bt_conn_ref(conn);
    peer_state = PIANTOR_POWER_UNKNOWN;

    discover_params.uuid = &power_char_uuid.uuid;
    discover_params.func = power_discover_func;
    discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

    int rc = bt_gatt_discover(conn, &discover_params);
    if (rc != 0) {
        LOG_WRN("Failed to start power-state characteristic discovery (%d)", rc);
    }
}

static void power_disconnected(struct bt_conn *conn, uint8_t reason) {
    if (conn != peer) {
        return;
    }
    bt_conn_unref(peer);
    peer = NULL;
    peer_state = PIANTOR_POWER_UNKNOWN;
}

BT_CONN_CB_DEFINE(piantor_power_conn_cb) = {
    .connected = power_connected,
    .disconnected = power_disconnected,
};
