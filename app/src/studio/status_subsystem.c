/*
 * Copyright (c) 2026 tibiboard30 custom
 *
 * SPDX-License-Identifier: MIT
 *
 * Studio "status" subsystem: battery level + live layer/key-event notifications.
 * Powers the app's header battery gauge, live layer highlighting, "pressed key
 * lights up on screen", and typing statistics. See status.proto.
 */

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <zmk/studio/rpc.h>
#include <zmk/keymap.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
#include <zmk/split/central.h>
#include <zmk/events/battery_state_changed.h> /* zmk_peripheral_battery_state_changed */
#endif

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
#include <zmk/usb.h>
#include <zmk/events/usb_conn_state_changed.h>
#endif

ZMK_RPC_SUBSYSTEM(status)

#define STATUS_RESPONSE(type, ...) ZMK_RPC_RESPONSE(status, type, __VA_ARGS__)

static bool usb_powered(void) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    return zmk_usb_is_powered();
#else
    return false;
#endif
}


/* ファームウェアのビルド番号(vN)。リリースのたびに+1する。アプリのヘッダーに表示され、
 * UF2書き込みが本当に成功したかの確認手段になる(Windowsのコピーエラーが毎回出るため —
 * 作者要望 2026-07-15)。 */
#define TIBIBOARD_FW_BUILD 53 /* v2.9.20 売る版(記録なし): 自作振動の列の取り合いを鍵で守る・単発のためすで列を止める・列の待ちを鳴り終わりから数える・回数の間隔を0.15秒に戻す / v2.9.19 試験用(記録入り): Studio の Bluetooth の返事で接続の参照を離さずに抜けていた本家の不具合を直す(抜け殻が残って、つなぎ直した相手に文字が届かなかった)・作っている途中の自作振動を保存せずに本体で1回鳴らす(set_sequence index 0xFFFF)・Output Selection を外す / v2.9.18 試験用(記録入り): Bluetooth の記録を全部(合図・ペアリング・今のプロファイルの相手か・接続の条件・受け取りの登録・登録の保存と数・接続の一覧・512件・繰り返しは1行)・2M PHY を使わない(EXPERIMENTAL_CONN)・たからもの/自作の振動を1打ずつ鳴らす(内蔵シーケンサーをやめる)・すぐのレイヤータップ(hlq)・Caps Word/Key Repeat/Output Selection を残す / v2.9.17 試験用(記録入り): 電池の残量の答えは残量の回数(25%ごとに1〜4回)を必ず使う・番号→レイヤーはロック付きで切り替える(&tog/&to で入ったレイヤーも外す)・紐付けで入ったレイヤーにいる時に紐付けの無い番号へ移ったらレイヤー0 に戻す / v2.9.16 試験用: 接続・暗号化・文字の送信結果・送り先・番号→レイヤーを振動の記録(128件)へ(DIAG_TRACE・売る版では外す) / v2.9.15 電源を入れた時の合図を3つ(電源オン・左右接続・BT接続をつながった順に1.5秒ずつ)・左右接続を🔔の表(出来事6)で選べる・BT切り替え/BT接続/左右接続が出荷時から入る・古い方式の割り当て(8枠・全キー一律)では鳴らさない・自作振動の「ためす」を本体の列で・デフォルトに戻すでコンボ/レイヤー名もその場で戻す・購読の確認を切る(パソコンだけ打てない件) / v2.9.14 設定していない振動を鳴らさない(切断・BTボタンの固定カチッ・タイマーの固定コツン)・電源オンの自動調整は最初の1回だけ・設定を読んだ後に消音を決め直す・初期設定に戻すで振動まわりも全部消す・F24の残量も電池の札で / v2.9.13 振動の整理(同じ瞬間は最初の1つ・トグルのレイヤーから戻る時も鳴る・回数の間隔0.10秒・BTボタンの合図は1つ・長い振動は1回・左右/未登録の切断は鳴らさず、どこが切れたか記録) / v2.9.12 マクロの待ち時間の手(などの制御の手)に固定番号を付け、アプリから書ける・電源を入れ直しても残る / v2.9.11オリジナル振動(8コマ・DRV2605Lの内蔵シーケンサーで一括再生)・振動の強さ100%固定・較正前に0x16/0x17とDRIVE_TIME(A=19/B=24)を書いて読み戻す・タイマーの既定振動も全部OFF/電池停止に従う・多段リズムを再生しない・自己診断に起動時CONTROL1と時刻別LRA_PERIOD / v2.9.10 FW41のスタンバイGOポーリングを取り消し(毎回のI2Cを元の700ms一発へ) / v2.9.9 振動が死んでもキー入力を巻き添えにしない(専用ワークキュー+到達不能ガード) / v2.5.8 振動の再調整・起動時比較・故障回数 / v2.9.8 振動異常時のDRVレジスター/電源/USB診断 / v2.9.7 本体側の30キー累計・1時間保存 / v2.9.6 ポモドーロは回ごとの設定だけを見る / v2.9.5 電池設定の読み込み修正・出来事のキーをログに記録 / v2.9.4 つづき振動の枠 32→56 / v2.9.3 電池のお知らせを ON/OFF できる / v2.9.2 マクロに待ち時間の手(先頭に待つ手・残り31手) / v2.9.1 キーごとの振動も「最後の入力が勝つ」に統一 / v2.9.0 ポモドーロの長い休憩を廃止・回ごとの合図とキー・マクロ・お宝枠32・LEDを元に */

/* --- 診断: 起動カウンタ (2026-07-17, 振動暴走の原因確定用) ---
 * __noinit セクションはリセットで初期化されない。マジックナンバーが化けていなければ
 * 「前回の値が残っている=リセット後の再起動」としてカウントを増やす。
 * 完全な電源断(コールドブート)ではRAMが消えてマジック不一致→1から数え直し。
 * → 暴走中にカウンタが膨れていれば「リセットループ」の動かぬ証拠になる。 */
#define BOOTCOUNT_MAGIC 0x7B005EF7
__noinit static uint32_t bootcount_magic;
__noinit static uint32_t bootcount_value;

static int bootcount_init(void) {
    if (bootcount_magic == BOOTCOUNT_MAGIC) {
        bootcount_value++;
    } else {
        bootcount_magic = BOOTCOUNT_MAGIC;
        bootcount_value = 1;
    }
    return 0;
}
SYS_INIT(bootcount_init, PRE_KERNEL_1, 0);

/* vibe_handler.c(centralのみ)が実体を持つ。周辺機ビルドではweak定義(常に0)。 */
__attribute__((weak)) uint32_t vibe_trigger_count = 0;

/* --- ヒートマップ(2026-07-27): キー別打鍵数・コンボ別発動数 ---
 * heatmap_counter.c(centralのみ)が実体を持つ。周辺機ビルドではweakで空を返す。 */
__attribute__((weak)) size_t tibiboard_heatmap_key_bytes(uint8_t *out, size_t max_len) {
    return 0;
}
__attribute__((weak)) size_t tibiboard_heatmap_combo_bytes(uint8_t *out, size_t max_len) {
    return 0;
}
__attribute__((weak)) int tibiboard_heatmap_reset(void) { return -ENOTSUP; }

static zmk_studio_Response get_status(const zmk_studio_Request *req) {
    zmk_status_GetStatusResponse resp = zmk_status_GetStatusResponse_init_zero;
    resp.firmware_build = TIBIBOARD_FW_BUILD;
    resp.boot_count = bootcount_value;
    resp.uptime_seconds = (uint32_t)(k_uptime_get() / 1000);
    resp.vibe_trigger_count = vibe_trigger_count;

    resp.key_counts.size =
        tibiboard_heatmap_key_bytes(resp.key_counts.bytes, sizeof(resp.key_counts.bytes));
    resp.combo_counts.size =
        tibiboard_heatmap_combo_bytes(resp.combo_counts.bytes, sizeof(resp.combo_counts.bytes));

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    resp.battery_percent = zmk_battery_state_of_charge();
    resp.battery_present = true;
#endif
    resp.battery_charging = usb_powered();
    resp.highest_active_layer = zmk_keymap_highest_layer_active();

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    uint8_t peripheral_level = 0;
    if (zmk_split_central_get_peripheral_battery_level(0, &peripheral_level) == 0) {
        resp.peripheral_battery_percent = peripheral_level;
        /* centralの保持値は未受信/リセット直後だと0。本当に0%なら右手側は動いていない
         * ので、0は「未取得」としてpresent=falseにする(以前は一度でも受信したら
         * present=trueにしていて、保持値が0に戻ると「右 0%」と誤表示された —
         * 作者報告 2026-07-14) */
        resp.peripheral_battery_present = peripheral_level > 0;
    }
#endif

    return STATUS_RESPONSE(get_status, resp);
}

ZMK_RPC_SUBSYSTEM_HANDLER(status, get_status, ZMK_STUDIO_RPC_HANDLER_SECURED);

static zmk_studio_Response reset_key_counts(const zmk_studio_Request *req) {
    ARG_UNUSED(req);
    return STATUS_RESPONSE(key_counts_reset, tibiboard_heatmap_reset() == 0);
}

ZMK_RPC_SUBSYSTEM_HANDLER(status, reset_key_counts, ZMK_STUDIO_RPC_HANDLER_SECURED);

static int status_event_mapper(const zmk_event_t *eh, zmk_studio_Notification *n) {
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    struct zmk_battery_state_changed *bat_ev = as_zmk_battery_state_changed(eh);
    if (bat_ev) {
        zmk_status_BatteryChanged bc = zmk_status_BatteryChanged_init_zero;
        bc.battery_percent = bat_ev->state_of_charge;
        bc.battery_charging = usb_powered();
        *n = ZMK_RPC_NOTIFICATION(status, battery, bc);
        return 0;
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    struct zmk_peripheral_battery_state_changed *pbat_ev =
        as_zmk_peripheral_battery_state_changed(eh);
    if (pbat_ev) {
        zmk_status_PeripheralBatteryChanged pc = zmk_status_PeripheralBatteryChanged_init_zero;
        pc.source = pbat_ev->source;
        pc.battery_percent = pbat_ev->state_of_charge;
        *n = ZMK_RPC_NOTIFICATION(status, peripheral_battery, pc);
        return 0;
    }
#endif

    struct zmk_layer_state_changed *layer_ev = as_zmk_layer_state_changed(eh);
    if (layer_ev) {
        /* イベント発火時点でキーマップ状態は更新済みなので、最前面レイヤーを都度計算 */
        zmk_status_LayerChanged lc = zmk_status_LayerChanged_init_zero;
        lc.highest_active_layer = zmk_keymap_highest_layer_active();
        *n = ZMK_RPC_NOTIFICATION(status, layer, lc);
        return 0;
    }

    struct zmk_position_state_changed *pos_ev = as_zmk_position_state_changed(eh);
    if (pos_ev) {
        /* splitのcentralは右半分のキー位置イベントも受け取るので、全キーがここを通る */
        zmk_status_KeyEvent ke = zmk_status_KeyEvent_init_zero;
        ke.position = pos_ev->position;
        ke.pressed = pos_ev->state;
        *n = ZMK_RPC_NOTIFICATION(status, key, ke);
        return 0;
    }

    return -ENOTSUP;
}

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) &&                                                    \
    IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
ZMK_RPC_EVENT_MAPPER(status, status_event_mapper, zmk_battery_state_changed,
                     zmk_peripheral_battery_state_changed, zmk_layer_state_changed,
                     zmk_position_state_changed);
#elif IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
ZMK_RPC_EVENT_MAPPER(status, status_event_mapper, zmk_battery_state_changed,
                     zmk_layer_state_changed, zmk_position_state_changed);
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
ZMK_RPC_EVENT_MAPPER(status, status_event_mapper, zmk_peripheral_battery_state_changed,
                     zmk_layer_state_changed, zmk_position_state_changed);
#else
ZMK_RPC_EVENT_MAPPER(status, status_event_mapper, zmk_layer_state_changed,
                     zmk_position_state_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && IS_ENABLED(CONFIG_USB_DEVICE_STACK)
/* USBを挿した/抜いた瞬間に「充電中」をアプリへ即通知する。
 * これがないと充電状態は次のバッテリー%変化イベント(数分おき)まで更新されず、
 * 「USBを挿したのに充電中にならない」ように見えてしまう。 */
static int status_usb_event_mapper(const zmk_event_t *eh, zmk_studio_Notification *n) {
    struct zmk_usb_conn_state_changed *usb_ev = as_zmk_usb_conn_state_changed(eh);
    if (usb_ev) {
        zmk_status_BatteryChanged bc = zmk_status_BatteryChanged_init_zero;
        bc.battery_percent = zmk_battery_state_of_charge();
        bc.battery_charging = usb_powered();
        *n = ZMK_RPC_NOTIFICATION(status, battery, bc);
        return 0;
    }
    return -ENOTSUP;
}

ZMK_RPC_EVENT_MAPPER(status_usb, status_usb_event_mapper, zmk_usb_conn_state_changed);
#endif
