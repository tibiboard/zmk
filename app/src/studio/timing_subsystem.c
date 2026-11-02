/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * chibiboardカスタム: タッピングターム(長押し判定時間)のランタイム調整RPC。
 * v1.1「誤爆カウンタ+自動提案」のファーム側。
 * - 上書き値は behavior_hold_tap.c の tibiboard_tapping_term_override に反映
 * - きわどい判定カウンタ(±50msで決着した打鍵)を読み出せる
 * - 上書き値はNVSに保存して電源断でも維持(vibesと同じパターン)
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <zmk/studio/rpc.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* behavior_hold_tap.c のグローバル(定義はあちら) */
extern int32_t tibiboard_tapping_term_override;
extern uint32_t tibiboard_ht_default_term_ms;
extern uint32_t tibiboard_ht_total_taps, tibiboard_ht_near_taps;
extern uint32_t tibiboard_ht_total_holds, tibiboard_ht_near_holds;

#define TIMING_SETTINGS_KEY "timing/term"
#define TIMING_MIN_MS 100
#define TIMING_MAX_MS 500

static void save_timing(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    settings_save_one(TIMING_SETTINGS_KEY, &tibiboard_tapping_term_override,
                      sizeof(tibiboard_tapping_term_override));
#endif
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int timing_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                               void *cb_arg) {
    if (strcmp(name, "term") == 0) {
        int32_t val = 0;
        if (len != sizeof(val)) {
            return -EINVAL;
        }
        int rc = read_cb(cb_arg, &val, sizeof(val));
        if (rc >= 0) {
            /* 範囲外の保存値は無視(化けたNVS対策 — vibes v10の範囲検証と同じ思想) */
            if (val == 0 || (val >= TIMING_MIN_MS && val <= TIMING_MAX_MS)) {
                tibiboard_tapping_term_override = val;
            }
            return 0;
        }
        return rc;
    }
    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(timing, "timing", NULL, timing_settings_set, NULL, NULL);
#endif

ZMK_RPC_SUBSYSTEM(timing)

#define TIMING_RESPONSE(type, ...) ZMK_RPC_RESPONSE(timing, type, __VA_ARGS__)

static zmk_timing_Timing current_timing(void) {
    uint32_t def = tibiboard_ht_default_term_ms;
    bool ov = tibiboard_tapping_term_override > 0;
    return (zmk_timing_Timing){
        .tapping_term_ms = ov ? (uint32_t)tibiboard_tapping_term_override : def,
        .default_term_ms = def,
        .overridden = ov,
        .total_taps = tibiboard_ht_total_taps,
        .near_taps = tibiboard_ht_near_taps,
        .total_holds = tibiboard_ht_total_holds,
        .near_holds = tibiboard_ht_near_holds,
    };
}

static zmk_studio_Response get_timing(const zmk_studio_Request *req) {
    return TIMING_RESPONSE(get_timing, current_timing());
}

static zmk_studio_Response set_timing(const zmk_studio_Request *req) {
    uint32_t ms = req->subsystem.timing.request_type.set_timing.tapping_term_ms;
    if (ms == 0) {
        tibiboard_tapping_term_override = 0; /* 既定に戻す */
    } else {
        tibiboard_tapping_term_override = (int32_t)CLAMP(ms, TIMING_MIN_MS, TIMING_MAX_MS);
    }
    save_timing();
    return TIMING_RESPONSE(set_timing, ((zmk_timing_SetTimingResponse){
                                           .ok = true,
                                           .has_timing = true,
                                           .timing = current_timing(),
                                       }));
}

/* ==== 誤爆アナリティクス(2026-07-23作者要望「どのキーと、どの組み合わせで」) ====
 *
 * behavior_hold_tap.c のweakフック(tibiboard_misfire_note_decision)の実体。
 * - キー位置別カウンタ: タップ/ホールド/すれすれ/巻き込みを位置ごとに集計
 * - ペア履歴: 誤爆(すれすれ・巻き込み)が起きた瞬間の「直前キー→当該キー」をリング保存
 * - 全てRAMのみ(フラッシュに書かない=ゴミ蓄積ゼロ。長期集計はアプリ側の仕事)
 * - このファイルはcentral(左)のStudioビルドでのみコンパイルされる。右は
 *   hold_tap側のweak空実装が使われるので安全。 */

#define MF_POSITIONS 64
#define MF_PAIR_RING 64
#define MF_NEAR_MS 50
#define MF_PREV_WINDOW_MS 2000 /* これより昔の「直前キー」はペアとみなさない */

static uint16_t mf_taps[MF_POSITIONS];
static uint16_t mf_near_taps[MF_POSITIONS];
static uint16_t mf_holds[MF_POSITIONS];
static uint16_t mf_near_holds[MF_POSITIONS];
static uint16_t mf_interrupts[MF_POSITIONS];

struct mf_pair {
    uint16_t prev;
    uint16_t pos;
    uint16_t dur_ms;
    uint16_t interval_ms; /* 直前キー押下→このキー押下の間隔。ロール判定用。0xFFFF=不明 */
    uint8_t kind; /* 1=あわや長押し 2=あわやタップ 3=巻き込み */
};
static struct mf_pair mf_ring[MF_PAIR_RING];
static uint8_t mf_ring_head; /* 次に書く場所 */
static uint8_t mf_ring_count;

/* 直前に押されたキーの追跡(ペア分析用)。自分自身の押下も記録されるので、
 * 「自分以外で最後に押されたキー」を2段持ちで取れるようにする。 */
static uint16_t mf_last_pos = 0xFFFF, mf_prev_pos = 0xFFFF;
static int64_t mf_last_time, mf_prev_time;

#include <zmk/events/position_state_changed.h>

static int mf_press_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev && ev->state) { /* 押した時だけ */
        mf_prev_pos = mf_last_pos;
        mf_prev_time = mf_last_time;
        mf_last_pos = ev->position;
        mf_last_time = ev->timestamp;
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(tibiboard_misfire, mf_press_listener);
ZMK_SUBSCRIPTION(tibiboard_misfire, zmk_position_state_changed);

static void mf_push_pair(uint16_t position, uint8_t kind, uint32_t dur_ms, int64_t now) {
    /* 「このキー自身の押下」を飛ばして、その前に押していたキーを探す */
    uint16_t prev = 0xFFFF;
    int64_t prev_press_time = 0;
    if (mf_last_pos != position && mf_last_pos != 0xFFFF &&
        (now - mf_last_time) <= MF_PREV_WINDOW_MS) {
        prev = mf_last_pos;
        prev_press_time = mf_last_time;
    } else if (mf_prev_pos != position && mf_prev_pos != 0xFFFF &&
               (now - mf_prev_time) <= MF_PREV_WINDOW_MS) {
        prev = mf_prev_pos;
        prev_press_time = mf_prev_time;
    }
    /* このキー自身の押下時刻 ≒ 今(離した/判定した時刻) - 押していた長さ。
     * ロール判定の核心データ: 直前キー押下→このキー押下の間隔 */
    uint16_t interval = 0xFFFF;
    if (prev != 0xFFFF) {
        int64_t my_press_time = now - (int64_t)dur_ms;
        int64_t iv = my_press_time - prev_press_time;
        interval = (uint16_t)CLAMP(iv, 0, 65534);
    }
    mf_ring[mf_ring_head] = (struct mf_pair){
        .prev = prev,
        .pos = position,
        .dur_ms = (uint16_t)MIN(dur_ms, 65535u),
        .interval_ms = interval,
        .kind = kind,
    };
    mf_ring_head = (mf_ring_head + 1) % MF_PAIR_RING;
    if (mf_ring_count < MF_PAIR_RING) {
        mf_ring_count++;
    }
}

void tibiboard_misfire_note_decision(uint32_t position, uint8_t status, uint32_t dur_ms,
                                     uint32_t term_ms) {
    if (position >= MF_POSITIONS) {
        return;
    }
    int64_t now = k_uptime_get();
    switch (status) {
    case 0: /* タップ判定 */
        if (mf_taps[position] < 65535) mf_taps[position]++;
        if (dur_ms + MF_NEAR_MS >= term_ms) {
            if (mf_near_taps[position] < 65535) mf_near_taps[position]++;
            mf_push_pair(position, 1, dur_ms, now);
        }
        break;
    case 1: /* 時間経過ホールド */
        if (mf_holds[position] < 65535) mf_holds[position]++;
        if (dur_ms <= term_ms + MF_NEAR_MS) {
            if (mf_near_holds[position] < 65535) mf_near_holds[position]++;
            mf_push_pair(position, 2, dur_ms, now);
        }
        break;
    case 2: /* 巻き込みホールド(早打ちで勝手にmod化した可能性) */
        if (mf_holds[position] < 65535) mf_holds[position]++;
        if (mf_interrupts[position] < 65535) mf_interrupts[position]++;
        mf_push_pair(position, 3, dur_ms, now);
        break;
    }
}

static zmk_studio_Response get_key_stats(const zmk_studio_Request *req) {
    uint32_t pos = req->subsystem.timing.request_type.get_key_stats;
    zmk_timing_KeyStats ks = zmk_timing_KeyStats_init_zero;
    ks.position = pos;
    if (pos < MF_POSITIONS) {
        ks.taps = mf_taps[pos];
        ks.near_taps = mf_near_taps[pos];
        ks.holds = mf_holds[pos];
        ks.near_holds = mf_near_holds[pos];
        ks.interrupts = mf_interrupts[pos];
    }
    return TIMING_RESPONSE(key_stats, ks);
}

static zmk_studio_Response get_pair_event(const zmk_studio_Request *req) {
    uint32_t index = req->subsystem.timing.request_type.get_pair_event; /* 0=最新 */
    zmk_timing_PairEvent pe = zmk_timing_PairEvent_init_zero;
    if (index < mf_ring_count) {
        uint8_t slot = (uint8_t)((mf_ring_head + MF_PAIR_RING - 1 - index) % MF_PAIR_RING);
        pe.valid = true;
        pe.prev_position = mf_ring[slot].prev;
        pe.position = mf_ring[slot].pos;
        pe.duration_ms = mf_ring[slot].dur_ms;
        pe.kind = mf_ring[slot].kind;
        pe.interval_ms = mf_ring[slot].interval_ms;
    }
    return TIMING_RESPONSE(pair_event, pe);
}

static zmk_studio_Response reset_stats(const zmk_studio_Request *req) {
    tibiboard_ht_total_taps = 0;
    tibiboard_ht_near_taps = 0;
    tibiboard_ht_total_holds = 0;
    tibiboard_ht_near_holds = 0;
    memset(mf_taps, 0, sizeof(mf_taps));
    memset(mf_near_taps, 0, sizeof(mf_near_taps));
    memset(mf_holds, 0, sizeof(mf_holds));
    memset(mf_near_holds, 0, sizeof(mf_near_holds));
    memset(mf_interrupts, 0, sizeof(mf_interrupts));
    mf_ring_head = 0;
    mf_ring_count = 0;
    return TIMING_RESPONSE(reset_stats, ((zmk_timing_ResetStatsResponse){.ok = true}));
}

ZMK_RPC_SUBSYSTEM_HANDLER(timing, get_timing, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(timing, set_timing, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(timing, reset_stats, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(timing, get_key_stats, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(timing, get_pair_event, ZMK_STUDIO_RPC_HANDLER_SECURED);

/* core resetSettings(初期設定に戻す)で上書きも解除 */
static int timing_settings_reset(void) {
    tibiboard_tapping_term_override = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    settings_delete(TIMING_SETTINGS_KEY);
#endif
    return 0;
}

ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(timing, timing_settings_reset);
