/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/tap_dances.h>
#include <zmk/studio/rpc.h>

#include <pb_encode.h>

ZMK_RPC_SUBSYSTEM(tapdances)

#define TAPDANCES_RESPONSE(type, ...) ZMK_RPC_RESPONSE(tapdances, type, __VA_ARGS__)

/* Scope: lists the devicetree-configured tap-dances, and edits an existing one's
 * tapping-term-ms and per-tap bindings in place (effective immediately, not persisted across
 * reboots). Adding/removing tap-dance instances or changing the number of taps is out of scope,
 * mirroring how combo key-positions are out of scope in combo_subsystem.c. */

static bool encode_string(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const char *s = (const char *)*arg;

    if (!s) {
        return true;
    }

    // String fields are inherently length-delimited, so pb_encode_tag_for_field would infer the
    // right wire type here too; the explicit form is used for consistency with the packed-array
    // encoder in combo_subsystem.c, where the distinction actually bit us.
    if (!pb_encode_tag(stream, PB_WT_STRING, field->tag)) {
        return false;
    }

    return pb_encode_string(stream, s, strlen(s));
}

// Encodes a run of `struct zmk_behavior_binding` as repeated zmk.keymap.BehaviorBinding
// submessages. Message-type repeated fields are NOT packed: each element gets its own
// length-delimited tag, and pb_encode_tag_for_field infers exactly that from the field
// descriptor, so (unlike packed scalar arrays) it is the correct call here.
static bool encode_behavior_binding_array(pb_ostream_t *stream, const pb_field_t *field,
                                          const struct zmk_behavior_binding *bindings,
                                          size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        zmk_keymap_BehaviorBinding bb = zmk_keymap_BehaviorBinding_init_zero;
        if (bindings[i].behavior_dev) {
            bb.behavior_id = zmk_behavior_get_local_id(bindings[i].behavior_dev);
        }
        bb.param1 = bindings[i].param1;
        bb.param2 = bindings[i].param2;

        if (!pb_encode_submessage(stream, &zmk_keymap_BehaviorBinding_msg, &bb)) {
            return false;
        }
    }

    return true;
}

static bool encode_tap_dance_bindings(pb_ostream_t *stream, const pb_field_t *field,
                                      void *const *arg) {
    const size_t index = *(size_t *)*arg;

    size_t len;
    const struct zmk_behavior_binding *bindings = zmk_tap_dances_get_bindings(index, &len);

    return encode_behavior_binding_array(stream, field, bindings, len);
}

// The tap-dance's own devicetree node is ALSO registered as an ordinary ZMK behavior (that's how
// `&td0`/`&td1` can be used as keymap bindings at all), so it already has a
// `zmk_behavior_local_id_t` distinct from this subsystem's array index. Exposing THAT as the
// public `id` (instead of the raw array index) lets the app cross-reference a tap-dance against
// `zmk.keymap.BehaviorBinding.behavior_id` in the keymap to show which key position(s) it's bound
// to - the array index alone can't be matched against anything the keymap subsystem reports.
static zmk_behavior_local_id_t local_id_for_index(size_t index) {
    const char *name = zmk_tap_dances_get_name(index);
    if (!name) {
        return 0;
    }
    return zmk_behavior_get_local_id(name);
}

static int index_for_local_id(zmk_behavior_local_id_t id) {
    size_t count = zmk_tap_dances_get_count();
    for (size_t i = 0; i < count; i++) {
        if (local_id_for_index(i) == id) {
            return (int)i;
        }
    }
    return -1;
}

// Not itself a pb_callback (the `arg` double-indirection only matters for functions registered
// as `field.funcs.encode`); this is a plain helper invoked directly by encode_all_tap_dances.
static bool encode_tap_dance(pb_ostream_t *stream, const pb_field_t *field, size_t index) {
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    zmk_tapdances_TapDance td = zmk_tapdances_TapDance_init_zero;

    td.id = local_id_for_index(index);
    td.tapping_term_ms = (uint32_t)zmk_tap_dances_get_tapping_term_ms(index);
    td.name.funcs.encode = encode_string;
    td.name.arg = (void *)zmk_tap_dances_get_name(index);
    td.bindings.funcs.encode = encode_tap_dance_bindings;
    td.bindings.arg = (void *)&index;

    return pb_encode_submessage(stream, &zmk_tapdances_TapDance_msg, &td);
}

static bool encode_all_tap_dances(pb_ostream_t *stream, const pb_field_t *field,
                                  void *const *arg) {
    size_t count = zmk_tap_dances_get_count();

    for (size_t i = 0; i < count; i++) {
        if (!encode_tap_dance(stream, field, i)) {
            return false;
        }
    }

    return true;
}

zmk_studio_Response list_tap_dances(const zmk_studio_Request *req) {
    LOG_DBG("");

    zmk_tapdances_ListTapDancesResponse resp = zmk_tapdances_ListTapDancesResponse_init_zero;

    resp.tap_dances.funcs.encode = encode_all_tap_dances;

    return TAPDANCES_RESPONSE(list_tap_dances, resp);
}

zmk_studio_Response set_tap_dance(const zmk_studio_Request *req) {
    LOG_DBG("");
    const zmk_tapdances_SetTapDanceRequest *set_req =
        &req->subsystem.tapdances.request_type.set_tap_dance;

    int index = index_for_local_id(set_req->tap_dance_id);
    int ret = index < 0 ? -EINVAL
                        : zmk_tap_dances_set_tapping_term_ms(index, set_req->tapping_term_ms);

    if (ret < 0) {
        return TAPDANCES_RESPONSE(
            set_tap_dance,
            ((zmk_tapdances_SetTapDanceResponse){
                .which_result = zmk_tapdances_SetTapDanceResponse_err_tag,
                .result = {.err = zmk_tapdances_TapDanceErrorCode_TAP_DANCE_ERR_INVALID_ID},
            }));
    }

    return TAPDANCES_RESPONSE(set_tap_dance,
                              ((zmk_tapdances_SetTapDanceResponse){
                                  .which_result = zmk_tapdances_SetTapDanceResponse_ok_tag,
                                  .result = {.ok = true},
                              }));
}

static zmk_tapdances_SetTapDanceBindingResponse
set_binding_err(zmk_tapdances_TapDanceErrorCode err) {
    return (zmk_tapdances_SetTapDanceBindingResponse){
        .which_result = zmk_tapdances_SetTapDanceBindingResponse_err_tag,
        .result = {.err = err},
    };
}

zmk_studio_Response set_tap_dance_binding(const zmk_studio_Request *req) {
    LOG_DBG("");
    const zmk_tapdances_SetTapDanceBindingRequest *set_req =
        &req->subsystem.tapdances.request_type.set_tap_dance_binding;

    if (!set_req->has_binding) {
        return TAPDANCES_RESPONSE(
            set_tap_dance_binding,
            set_binding_err(zmk_tapdances_TapDanceErrorCode_TAP_DANCE_ERR_GENERIC));
    }

    const char *behavior_name =
        zmk_behavior_find_behavior_name_from_local_id(set_req->binding.behavior_id);

    if (!behavior_name) {
        return TAPDANCES_RESPONSE(
            set_tap_dance_binding,
            set_binding_err(zmk_tapdances_TapDanceErrorCode_TAP_DANCE_ERR_INVALID_BEHAVIOR));
    }

    struct zmk_behavior_binding binding = {
        .behavior_dev = behavior_name,
        .param1 = set_req->binding.param1,
        .param2 = set_req->binding.param2,
    };

    if (zmk_behavior_validate_binding(&binding) < 0) {
        return TAPDANCES_RESPONSE(
            set_tap_dance_binding,
            set_binding_err(zmk_tapdances_TapDanceErrorCode_TAP_DANCE_ERR_INVALID_BEHAVIOR));
    }

    int index = index_for_local_id(set_req->tap_dance_id);
    int ret = index < 0
                  ? -EINVAL
                  : zmk_tap_dances_set_binding(index, set_req->binding_index, binding);

    if (ret == -ERANGE) {
        return TAPDANCES_RESPONSE(
            set_tap_dance_binding,
            set_binding_err(zmk_tapdances_TapDanceErrorCode_TAP_DANCE_ERR_INVALID_INDEX));
    } else if (ret < 0) {
        return TAPDANCES_RESPONSE(
            set_tap_dance_binding,
            set_binding_err(zmk_tapdances_TapDanceErrorCode_TAP_DANCE_ERR_INVALID_ID));
    }

    return TAPDANCES_RESPONSE(
        set_tap_dance_binding,
        ((zmk_tapdances_SetTapDanceBindingResponse){
            .which_result = zmk_tapdances_SetTapDanceBindingResponse_ok_tag,
            .result = {.ok = true},
        }));
}

ZMK_RPC_SUBSYSTEM_HANDLER(tapdances, list_tap_dances, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(tapdances, set_tap_dance, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(tapdances, set_tap_dance_binding, ZMK_STUDIO_RPC_HANDLER_SECURED);

/* core resetSettings(初期設定に戻す)でタップダンスの保存済み編集も出荷時に戻す(F2) */
static int tapdances_settings_reset(void) { return zmk_tap_dances_reset_settings(); }

ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(tapdances, tapdances_settings_reset);
