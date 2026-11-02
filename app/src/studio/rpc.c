/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "msg_framing.h"

#include <pb_encode.h>
#include <pb_decode.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/debug/thread_analyzer.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/studio/core.h>
#include <zmk/studio/rpc.h>

ZMK_EVENT_IMPL(zmk_studio_rpc_notification);

static struct zmk_rpc_subsystem *find_subsystem_for_choice(uint8_t choice) {
    STRUCT_SECTION_FOREACH(zmk_rpc_subsystem, sub) {
        if (sub->subsystem_choice == choice) {
            return sub;
        }
    }

    return NULL;
}

zmk_studio_Response zmk_rpc_subsystem_delegate_to_subs(const struct zmk_rpc_subsystem *subsys,
                                                       const zmk_studio_Request *req,
                                                       uint8_t which_req) {
    LOG_DBG("Got subsystem func for %d", subsys->subsystem_choice);

    for (int i = subsys->handlers_start_index; i <= subsys->handlers_end_index; i++) {
        struct zmk_rpc_subsystem_handler *sub_handler;
        STRUCT_SECTION_GET(zmk_rpc_subsystem_handler, i, &sub_handler);
        if (sub_handler->request_choice == which_req) {
            if (sub_handler->security == ZMK_STUDIO_RPC_HANDLER_SECURED &&
                zmk_studio_core_get_lock_state() != ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED) {
                return ZMK_RPC_RESPONSE(meta, simple_error,
                                        zmk_meta_ErrorConditions_UNLOCK_REQUIRED);
            }
            return sub_handler->func(req);
        }
    }
    LOG_ERR("No handler func found for %d", which_req);
    return ZMK_RPC_RESPONSE(meta, simple_error, zmk_meta_ErrorConditions_RPC_NOT_FOUND);
}

static zmk_studio_Response handle_request(const zmk_studio_Request *req) {
    zmk_studio_core_reschedule_lock_timeout();
    struct zmk_rpc_subsystem *sub = find_subsystem_for_choice(req->which_subsystem);

    zmk_studio_Response resp;
    if (!sub) {
        LOG_WRN("No subsystem found for choice %d", req->which_subsystem);
        resp = ZMK_RPC_RESPONSE(meta, simple_error, zmk_meta_ErrorConditions_RPC_NOT_FOUND);
    } else {
        resp = sub->func(sub, req);
    }

    // This must run for BOTH branches above: previously the "no subsystem found" early-return
    // skipped setting request_id entirely, leaving it at its zero-initialized default. Any
    // client sending a request for a subsystem/behavior the firmware doesn't understand (e.g.
    // this app's new "combos" tab probing a device that predates combo support) got back an
    // error response with request_id 0 instead of the real request's ID, which broke that
    // client's request/response matching for every future call on the connection.
    resp.type.request_response.request_id = req->request_id;

    return resp;
}

RING_BUF_DECLARE(rpc_rx_buf, CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE);

static K_SEM_DEFINE(rpc_rx_sem, 0, 1);

static enum studio_framing_state rpc_framing_state;

static K_MUTEX_DEFINE(rpc_transport_mutex);
static struct zmk_rpc_transport *selected_transport;

struct ring_buf *zmk_rpc_get_rx_buf(void) { return &rpc_rx_buf; }

void zmk_rpc_rx_notify(void) { k_sem_give(&rpc_rx_sem); }

static bool rpc_read_cb(pb_istream_t *stream, uint8_t *buf, size_t count) {
    uint32_t write_offset = 0;

    do {
        uint8_t *buffer;
        uint32_t len = ring_buf_get_claim(&rpc_rx_buf, &buffer, count - write_offset);

        if (len > 0) {
            for (int i = 0; i < len; i++) {
                if (studio_framing_process_byte(&rpc_framing_state, buffer[i])) {
                    buf[write_offset++] = buffer[i];
                }
            }
        } else {
            k_sem_take(&rpc_rx_sem, K_FOREVER);
        }

        ring_buf_get_finish(&rpc_rx_buf, len);
    } while (write_offset < count && rpc_framing_state != FRAMING_STATE_EOF);

    if (rpc_framing_state == FRAMING_STATE_EOF) {
        stream->bytes_left = 0;
        return false;
    } else {
        return true;
    }
}

static pb_istream_t pb_istream_for_rx_ring_buf() {
    pb_istream_t stream = {&rpc_read_cb, NULL, SIZE_MAX};
    return stream;
}

RING_BUF_DECLARE(rpc_tx_buf, CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE);

struct ring_buf *zmk_rpc_get_tx_buf(void) { return &rpc_tx_buf; }

static bool rpc_tx_buffer_write(pb_ostream_t *stream, const uint8_t *buf, size_t count) {
    void *user_data = stream->state;
    size_t written = 0;
    int stalled_ms = 0;

    bool escape_byte_already_written = false;
    do {
        uint32_t write_idx = 0;

        uint8_t *write_buf;
        uint32_t claim_len = ring_buf_put_claim(&rpc_tx_buf, &write_buf, count - written);

        if (claim_len == 0) {
            /* tibiboard30 fix: 上流は満杯時に無限ビジースピンしてスレッドを凍結させて
             * いた(実害: USB給電のみ+クライアント未接続で通知が流れると、システム
             * ワークキューごと固まり振動OFFが実行されずモーターが回りっぱなし)。
             * 少し待っても排水されなければこのメッセージは諦める(受信側のフレーミングは
             * 次のSOFで回復する)。 */
            if (++stalled_ms > 200) {
                return false;
            }
            k_sleep(K_MSEC(1));
            continue;
        }
        stalled_ms = 0;

        int escapes_written = 0;
        for (int i = 0; i < claim_len && write_idx < claim_len; i++) {
            uint8_t b = buf[written + i];
            switch (b) {
            case FRAMING_EOF:
            case FRAMING_ESC:
            case FRAMING_SOF:
                // Care to be taken. We may need to write the escape byte,
                // but that's the last available spot for this claim, so we track
                // if the escape has already been written in the previous iteration
                // of our loop.
                if (!escape_byte_already_written) {
                    escapes_written++;
                    write_buf[write_idx++] = FRAMING_ESC;
                    escape_byte_already_written = true;
                    if (write_idx >= claim_len) {
                        LOG_WRN("Skipping on, no room to write escape and real byte");
                        continue;
                    }
                }
            default:
                write_buf[write_idx++] = b;
                escape_byte_already_written = false;
                break;
            }
        }

        ring_buf_put_finish(&rpc_tx_buf, write_idx);

        written += (write_idx - escapes_written);

        selected_transport->tx_notify(&rpc_tx_buf, write_idx, false, user_data);
    } while (written < count);

    return true;
}

static pb_ostream_t pb_ostream_for_tx_buf(void *user_data) {
    pb_ostream_t stream = {&rpc_tx_buffer_write, (void *)user_data, SIZE_MAX, 0};
    return stream;
}

/* tibiboard30 fix: SOF/EOFマーカーの書き込みも満杯を考慮する。上流はring_buf_putの
 * 戻り値を見ておらず、TXリング満杯時にマーカーだけ黙って落ちてフレーム構造が崩れ、
 * 受信側のデコーダが次のメッセージまで道連れにする余地があった。ペイロード書き込み
 * (rpc_tx_buffer_write)と同じ「最大200ms待って諦める」方針で書き込む。 */
static bool rpc_put_framing_byte(uint8_t framing_byte) {
    int stalled_ms = 0;
    while (ring_buf_put(&rpc_tx_buf, &framing_byte, 1) != 1) {
        if (++stalled_ms > 200) {
            LOG_WRN("TX ring full, dropping framing byte 0x%02x", framing_byte);
            return false;
        }
        k_sleep(K_MSEC(1));
    }
    return true;
}

static int send_response(const zmk_studio_Response *resp) {
    int ret = 0;

    k_mutex_lock(&rpc_transport_mutex, K_FOREVER);

    if (!selected_transport) {
        goto exit;
    }

    void *user_data = selected_transport->tx_user_data ? selected_transport->tx_user_data() : NULL;

    pb_ostream_t stream = pb_ostream_for_tx_buf(user_data);

    if (!rpc_put_framing_byte(FRAMING_SOF)) {
        /* 何も書けていないので、このメッセージを丸ごと諦めれば無害 */
        ret = -EAGAIN;
        goto exit;
    }

    selected_transport->tx_notify(&rpc_tx_buf, 1, false, user_data);

    /* Now we are ready to encode the message! */
    bool status = pb_encode(&stream, &zmk_studio_Response_msg, resp);

    if (!status) {
#if !IS_ENABLED(CONFIG_NANOPB_NO_ERRMSG)
        LOG_ERR("Failed to encode the message %s", stream.errmsg);
#endif // !IS_ENABLED(CONFIG_NANOPB_NO_ERRMSG)
        // Fall through (rather than returning here) on purpose: the SOF byte — and possibly a
        // partial payload — is already queued in the TX ring, so bailing out without an EOF
        // would leave the client's framing decoder stuck mid-frame, where the NEXT response's
        // SOF looks like a protocol error and kills the whole client connection. Worse, the
        // early return skipped k_mutex_unlock, so any other thread sending (e.g. the event
        // thread raising a notification) would then block on rpc_transport_mutex forever.
        ret = -EINVAL;
    }

    rpc_put_framing_byte(FRAMING_EOF);

    selected_transport->tx_notify(&rpc_tx_buf, 1, true, user_data);

exit:
    k_mutex_unlock(&rpc_transport_mutex);
    return ret;
}

static void rpc_main(void) {
    for (;;) {
        pb_istream_t stream = pb_istream_for_rx_ring_buf();
        zmk_studio_Request req = zmk_studio_Request_init_zero;
#if IS_ENABLED(CONFIG_THREAD_ANALYZER)
        thread_analyzer_print(0);
#endif // IS_ENABLED(CONFIG_THREAD_ANALYZER)
        bool status = pb_decode(&stream, &zmk_studio_Request_msg, &req);

        rpc_framing_state = FRAMING_STATE_IDLE;

        if (status) {
            zmk_studio_Response resp = handle_request(&req);

            int err = send_response(&resp);
#if IS_ENABLED(CONFIG_THREAD_ANALYZER)
            thread_analyzer_print(0);
#endif // IS_ENABLED(CONFIG_THREAD_ANALYZER)
            if (err < 0) {
                LOG_ERR("Failed to send the RPC response %d", err);
            }
        } else {
            LOG_DBG("Decode failed");
        }
    }
}

K_THREAD_DEFINE(studio_rpc_thread, CONFIG_ZMK_STUDIO_RPC_THREAD_STACK_SIZE, rpc_main, NULL, NULL,
                NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

static void refresh_selected_transport(void) {
    enum zmk_transport transport = zmk_endpoint_get_selected().transport;

    k_mutex_lock(&rpc_transport_mutex, K_FOREVER);

    if (selected_transport && selected_transport->transport == transport) {
        goto exit_refresh;
    }

    if (selected_transport) {
        if (selected_transport->rx_stop) {
            selected_transport->rx_stop();
        }
        selected_transport = NULL;
#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOCK_ON_DISCONNECT)
        zmk_studio_core_lock();
#endif
    }

    STRUCT_SECTION_FOREACH(zmk_rpc_transport, t) {
        if (t->transport == transport) {
            selected_transport = t;
            if (selected_transport->rx_start) {
                selected_transport->rx_start();
            }
            break;
        }
    }

    if (!selected_transport) {
        LOG_WRN("Failed to select a transport!");
    }

exit_refresh:
    k_mutex_unlock(&rpc_transport_mutex);
}

static int zmk_rpc_init(void) {
    int prev_choice = -1;
    struct zmk_rpc_subsystem *prev_sub = NULL;
    int i = 0;

    STRUCT_SECTION_FOREACH(zmk_rpc_subsystem_handler, handler) {
        struct zmk_rpc_subsystem *sub = find_subsystem_for_choice(handler->subsystem_choice);

        __ASSERT(sub != NULL, "RPC Handler for unknown subsystem choice %d",
                 handler->subsystem_choice);

        if (prev_choice < 0) {
            sub->handlers_start_index = i;
        } else if ((prev_choice != handler->subsystem_choice) && prev_sub) {
            prev_sub->handlers_end_index = i - 1;
            sub->handlers_start_index = i;
        }

        prev_choice = handler->subsystem_choice;
        prev_sub = sub;
        i++;
    }

    if (prev_sub) {
        prev_sub->handlers_end_index = i - 1;
    }

    refresh_selected_transport();

    return 0;
}

SYS_INIT(zmk_rpc_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* tibiboard30 fix: 通知専用の送信スレッド。イベント発生スレッド(キー入力処理等)からは
 * 待ち行列に積むだけにして、実際のエンコード/送信(詰まると待たされる)はここで行う。
 * これで送信の渋滞が入力・振動などに波及しない。行列が満杯なら新しい通知は捨てる。 */
K_MSGQ_DEFINE(rpc_notify_msgq, sizeof(zmk_studio_Notification), 8, 4);

static void rpc_notify_main(void *_a, void *_b, void *_c) {
    zmk_studio_Notification n;
    while (true) {
        k_msgq_get(&rpc_notify_msgq, &n, K_FOREVER);

        zmk_studio_Response resp = zmk_studio_Response_init_zero;
        resp.which_type = zmk_studio_Response_notification_tag;
        resp.type.notification = n;
        send_response(&resp);
    }
}

K_THREAD_DEFINE(studio_rpc_notify_thread, 3072, rpc_notify_main, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

static int studio_rpc_listener_cb(const zmk_event_t *eh) {
    struct zmk_endpoint_changed *ep_changed = as_zmk_endpoint_changed(eh);
    if (ep_changed) {
        refresh_selected_transport();
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct zmk_studio_rpc_notification *rpc_notify = as_zmk_studio_rpc_notification(eh);
    if (rpc_notify) {
        /* tibiboard30 fix: 通知をこのスレッド(イベントを起こしたスレッド=キー入力の
         * 処理スレッド等)で直接送信しない。送信が詰まる(BLEが遅い/クライアント未接続で
         * TXリングが排水されない等)と入力処理ごと固まるため。専用スレッドの待ち行列に
         * 積むだけにして即座に戻る。満杯なら捨てる(通知は損失許容)。 */
        if (ring_buf_space_get(&rpc_tx_buf) < CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE / 4) {
            return ZMK_EV_EVENT_BUBBLE;
        }
        k_msgq_put(&rpc_notify_msgq, &rpc_notify->notification, K_NO_WAIT);
        return ZMK_EV_EVENT_BUBBLE;
    }

    zmk_studio_Notification n = zmk_studio_Notification_init_zero;
    STRUCT_SECTION_FOREACH(zmk_rpc_event_mapper, mapper) {
        int ret = mapper->func(eh, &n);
        if (ret >= 0) {
            raise_zmk_studio_rpc_notification(
                (struct zmk_studio_rpc_notification){.notification = n});
            break;
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(studio_rpc, studio_rpc_listener_cb);
ZMK_SUBSCRIPTION(studio_rpc, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(studio_rpc, zmk_studio_rpc_notification);
