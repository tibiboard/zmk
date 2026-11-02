/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <sys/types.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/ring_buffer.h>

#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/studio/rpc.h>

#include "uuid.h"

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

static bool handling_rx = false;

static K_SEM_DEFINE(indicate_sem, 1, 1);
static atomic_t notify_size;

static void refresh_notify_size(void);

static void rpc_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    ARG_UNUSED(attr);

    bool notif_enabled = (value == BT_GATT_CCC_INDICATE);

    LOG_INF("RPC Notifications %s", notif_enabled ? "enabled" : "disabled");

    // The client subscribing is a good moment to re-sample the negotiated ATT MTU: MTU
    // exchange typically completes after the connection events that previously refreshed
    // notify_size, so without this the cached value could stay at the un-negotiated minimum.
    refresh_notify_size();

#if CONFIG_ZMK_STUDIO_TRANSPORT_BLE_PREF_LATENCY < CONFIG_BT_PERIPHERAL_PREF_LATENCY
    struct bt_conn *conn = zmk_ble_active_profile_conn();
    if (conn) {
        uint8_t latency = notif_enabled ? CONFIG_ZMK_STUDIO_TRANSPORT_BLE_PREF_LATENCY
                                        : CONFIG_BT_PERIPHERAL_PREF_LATENCY;

        int ret = bt_conn_le_param_update(
            conn,
            BT_LE_CONN_PARAM(CONFIG_BT_PERIPHERAL_PREF_MIN_INT, CONFIG_BT_PERIPHERAL_PREF_MAX_INT,
                             latency, CONFIG_BT_PERIPHERAL_PREF_TIMEOUT));
        if (ret < 0) {
            LOG_WRN("Failed to request lower latency while studio is active (%d)", ret);
        }

        bt_conn_unref(conn);
    }
#endif
}

static ssize_t read_rpc_resp(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                             uint16_t len, uint16_t offset) {

    LOG_DBG("Read response for length %d at offset %d", len, offset);
    return 0;
}

static ssize_t write_rpc_req(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                             uint16_t len, uint16_t offset, uint8_t flags) {
    if (!handling_rx) {
        return len;
    }

    uint32_t copied = 0;
    struct ring_buf *rpc_buf = zmk_rpc_get_rx_buf();
    while (copied < len) {
        uint8_t *buffer;
        uint32_t claim_len = ring_buf_put_claim(rpc_buf, &buffer, len - copied);

        if (claim_len > 0) {
            memcpy(buffer, ((uint8_t *)buf) + copied, claim_len);
            copied += claim_len;
        } else {
            // RX ring full: the RPC thread hasn't drained earlier request bytes yet (e.g. it
            // is still streaming out a large response and a pipelined client sent another
            // request). This callback runs on the cooperative-priority BT RX thread, so a
            // hot spin here would block every lower-priority thread — including the RPC
            // thread that must drain this very buffer — wedging the whole system. Make sure
            // the RPC thread is woken for what we've queued so far, then sleep to let it run.
            zmk_rpc_rx_notify();
            k_sleep(K_MSEC(1));
        }

        ring_buf_put_finish(rpc_buf, claim_len);
    }

    zmk_rpc_rx_notify();

    return len;
}

BT_GATT_SERVICE_DEFINE(
    rpc_interface, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(ZMK_STUDIO_BT_SERVICE_UUID)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ZMK_STUDIO_BT_RPC_CHRC_UUID),
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_READ | BT_GATT_CHRC_INDICATE,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT, read_rpc_resp,
                           write_rpc_req, NULL),
    BT_GATT_CCC(rpc_ccc_cfg_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT));

static uint16_t get_notify_size_for_conn(struct bt_conn *conn) {
    // An ATT Handle Value Indication carries at most (ATT MTU - 3) bytes of value. The LL
    // data length (conn_info.le.data_len->tx_max_len) used previously is unrelated to that
    // limit and caused two real problems:
    //  - it capped every indication at ~27 bytes even when the central had negotiated a large
    //    ATT MTU (e.g. 247), making multi-KB responses (keymap, macro/combo lists) take tens
    //    of seconds to minutes over BLE, and
    //  - on split centrals it can be 69 (> both the 64-byte TX ring and the indicate buffer),
    //    which as gatt_tx_notify's submit threshold meant the drain work item was never
    //    submitted while rpc_tx_buffer_write spun forever on a full ring: a hard RPC wedge.
    uint16_t mtu = (conn != NULL) ? bt_gatt_get_mtu(conn) : 0;

    if (mtu < 23) {
        mtu = 23; // ATT spec minimum, also covers a 0 return before ATT is fully up.
    }

    return mtu - 3;
}

static void refresh_notify_size(void) {
    struct bt_conn *conn = zmk_ble_active_profile_conn();

    uint16_t ns = get_notify_size_for_conn(conn);
    if (conn) {
        bt_conn_unref(conn);
    }

    atomic_set(&notify_size, ns);
}

static int gatt_start_rx() {
    refresh_notify_size();
    handling_rx = true;
    return 0;
}

static int gatt_stop_rx(void) {
    handling_rx = false;
    return 0;
}

// Sized for a typical maximum negotiated ATT MTU of 247 (247 - 3 = 244 value bytes per
// indication); notif_rpc_tx_cb clamps each indication to MIN(ATT MTU - 3, sizeof(buffer)),
// so smaller negotiated MTUs still work, just in smaller chunks.
static uint8_t indicate_buffer[244];

static void indicate_cb(struct bt_conn *conn, struct bt_gatt_indicate_params *params, uint8_t err);

static struct bt_gatt_indicate_params rpc_indicate_params = {
    .attr = &rpc_interface.attrs[1],
    .data = indicate_buffer,
    .func = indicate_cb,
};

static void notif_rpc_tx_cb(struct k_work *work) {
    struct bt_conn *conn = zmk_ble_active_profile_conn();
    struct ring_buf *tx_buf = zmk_rpc_get_tx_buf();

    if (!conn) {
        LOG_WRN("No active connection for queued data, dropping");
        ring_buf_reset(tx_buf);
        return;
    }

    uint16_t notify_size = MIN(get_notify_size_for_conn(conn), sizeof(indicate_buffer));

    if (ring_buf_size_get(tx_buf) > 0) {
        int ret = k_sem_take(&indicate_sem, K_NO_WAIT);
        if (ret < 0) {
            /* tibiboard (2026-10-17 FW52): 本家の不具合。ここで conn を離さずに抜けていたので、
             * 前の返事を送っている最中に次の返事が来るたびに接続の参照が1つ残った。相手が切れた後も
             * その接続が「抜け殻」(切れている状態)で残り、同じ相手がつなぎ直すと bt_conn_lookup_addr_le が
             * 抜け殻の方を先に返す → 「今のプロファイルとつながっていない」と判断して文字を送らなかった。
             * 電源を入れ直すまで直らない。FW51 の記録(10/17 3:22 iPad)で、同じ相手の「つながっている」と
             * 「切れている」が並んでいたのが証拠 */
            bt_conn_unref(conn);
            return;
        }

        uint16_t added = 0;
        while (added < notify_size && ring_buf_size_get(tx_buf) > 0) {
            uint8_t *buf;
            int len = ring_buf_get_claim(tx_buf, &buf, notify_size - added);

            memcpy(indicate_buffer + added, buf, len);

            added += len;
            ring_buf_get_finish(tx_buf, len);
        }

        rpc_indicate_params.len = added;

        int err = bt_gatt_indicate(conn, &rpc_indicate_params);
        if (err < 0) {
            LOG_WRN("Failed to notify the response %d", err);
            k_sem_give(&indicate_sem);
        }
    }

    bt_conn_unref(conn);
}

static K_WORK_DEFINE(notify_tx_work, notif_rpc_tx_cb);

struct gatt_write_state {
    size_t pending_notify;
};

static void indicate_cb(struct bt_conn *conn, struct bt_gatt_indicate_params *params, uint8_t err) {
    k_sem_give(&indicate_sem);
    k_work_submit(&notify_tx_work);
}

static void gatt_tx_notify(struct ring_buf *tx_buf, size_t added, bool msg_done, void *user_data) {
    struct gatt_write_state *state = (struct gatt_write_state *)user_data;

    state->pending_notify += added;

    size_t threshold = MIN((size_t)atomic_get(&notify_size), sizeof(indicate_buffer));

    // Never let the submit threshold reach the TX ring's capacity: rpc_tx_buffer_write spins
    // waiting for ring space and only this submit drains it, so a threshold the ring can't
    // hold would deadlock the RPC thread. Half the capacity keeps the pipeline moving.
    threshold = MIN(threshold, ring_buf_capacity_get(tx_buf) / 2);

    if (msg_done || state->pending_notify > threshold) {
        k_work_submit(&notify_tx_work);
        state->pending_notify = 0;
    }
}

static struct gatt_write_state tx_state = {};

static void *gatt_tx_user_data(void) {
    memset(&tx_state, 0, sizeof(tx_state));

    return &tx_state;
}

ZMK_RPC_TRANSPORT(gatt, ZMK_TRANSPORT_BLE, gatt_start_rx, gatt_stop_rx, gatt_tx_user_data,
                  gatt_tx_notify);

static int gatt_rpc_listener(const zmk_event_t *eh) {
    refresh_notify_size();

#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOCK_ON_DISCONNECT)
    struct bt_conn *conn = zmk_ble_active_profile_conn();

    if (!conn) {
        zmk_studio_core_lock();
    } else {
        bt_conn_unref(conn);
    }
#endif

    return 0;
}

ZMK_LISTENER(gatt_rpc_listener, gatt_rpc_listener);
ZMK_SUBSCRIPTION(gatt_rpc_listener, zmk_ble_active_profile_changed);
