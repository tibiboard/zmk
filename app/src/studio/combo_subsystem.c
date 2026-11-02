/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/combos.h>
#include <zmk/studio/rpc.h>

#include <pb_encode.h>

ZMK_RPC_SUBSYSTEM(combos)

#define COMBOS_RESPONSE(type, ...) ZMK_RPC_RESPONSE(combos, type, __VA_ARGS__)

/* Milestone 1 scope: read-only listing of the devicetree-configured combos. See
 * app/include/zmk/combos.h for the read-only accessors this relies on, and the design plan for
 * later milestones (editing key positions/binding/timeout, add/remove, persistence). */

static bool encode_packed_uint32_array(pb_ostream_t *stream, const pb_field_t *field,
                                       const int32_t *values, size_t len) {
    if (len == 0) {
        return true;
    }

    // NOT pb_encode_tag_for_field: that emits the wire type nanopb infers from the *field's own*
    // type (VARINT for a uint32 field), but a packed-repeated encoding needs an explicit
    // length-delimited (PB_WT_STRING) tag before the run of varints that follows. Using the wrong
    // wire type here produced well-formed-looking-to-the-firmware but actually corrupt bytes on
    // the wire: the client's protobuf decoder would misinterpret the length prefix as a varint
    // value and the following byte as a new field tag, throwing mid-stream and killing the
    // client's read pipeline (which looked like the device disconnecting, though the firmware
    // itself was fine).
    if (!pb_encode_tag(stream, PB_WT_STRING, field->tag)) {
        return false;
    }

    // proto3 repeated scalar fields are packed: a single tag followed by a length-prefixed run of
    // varints. Nanopb's callback API requires computing that length with a sizing pass first.
    pb_ostream_t sizing_stream = PB_OSTREAM_SIZING;
    for (size_t i = 0; i < len; i++) {
        if (!pb_encode_varint(&sizing_stream, (uint64_t)values[i])) {
            return false;
        }
    }

    if (!pb_encode_varint(stream, sizing_stream.bytes_written)) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        if (!pb_encode_varint(stream, (uint64_t)values[i])) {
            return false;
        }
    }

    return true;
}

static bool encode_combo_key_positions(pb_ostream_t *stream, const pb_field_t *field,
                                       void *const *arg) {
    const size_t combo_index = *(size_t *)*arg;

    size_t len;
    const int32_t *positions = zmk_combos_get_key_positions(combo_index, &len);

    return encode_packed_uint32_array(stream, field, positions, len);
}

// Not itself a pb_callback (the `arg` double-indirection only matters for functions registered
// as `field.funcs.encode`); this is a plain helper invoked directly by encode_all_combos below.
static bool encode_combo(pb_ostream_t *stream, const pb_field_t *field, size_t combo_index) {
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    zmk_combos_Combo combo = zmk_combos_Combo_init_zero;

    combo.id = combo_index;
    combo.timeout_ms = (uint32_t)zmk_combos_get_timeout_ms(combo_index);
    combo.require_prior_idle_ms = (uint32_t)zmk_combos_get_require_prior_idle_ms(combo_index);
    combo.slow_release = zmk_combos_get_slow_release(combo_index);

    combo.key_positions.funcs.encode = encode_combo_key_positions;
    combo.key_positions.arg = (void *)&combo_index;

    const struct zmk_behavior_binding *binding = zmk_combos_get_behavior(combo_index);
    if (binding && binding->behavior_dev) {
        combo.has_binding = true;
        combo.binding.behavior_id = zmk_behavior_get_local_id(binding->behavior_dev);
        combo.binding.param1 = binding->param1;
        combo.binding.param2 = binding->param2;
    }

    // Milestone 1 doesn't yet expose the DT `layers` bitmask as stable layer IDs (that mapping is
    // needed for later milestones that let editing survive layer reordering); report "all layers"
    // for now by leaving layer_ids empty, which matches the combo's real runtime behavior when
    // layer_mask is 0, and is at worst a display simplification when a DT combo restricts layers.

    return pb_encode_submessage(stream, &zmk_combos_Combo_msg, &combo);
}

static bool encode_all_combos(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    size_t count = zmk_combos_get_combo_count();

    for (size_t i = 0; i < count; i++) {
        if (!encode_combo(stream, field, i)) {
            return false;
        }
    }

    return true;
}

zmk_studio_Response list_combos(const zmk_studio_Request *req) {
    LOG_DBG("");

    zmk_combos_ListCombosResponse resp = zmk_combos_ListCombosResponse_init_zero;

    resp.combos.funcs.encode = encode_all_combos;
    resp.available_combos = 0; // Milestone 1 is read-only; add/remove lands in a later milestone.
    resp.max_keys_per_combo = 0;
    resp.max_name_length = 0;

    return COMBOS_RESPONSE(list_combos, resp);
}

static zmk_combos_SetComboResponse set_combo_err(zmk_combos_SetComboErrorCode err) {
    return (zmk_combos_SetComboResponse){
        .which_result = zmk_combos_SetComboResponse_err_tag,
        .result = {.err = err},
    };
}

// Milestone 3 scope: edits timeout_ms/require_prior_idle_ms/slow_release/behavior on an existing
// combo (looked up by `combo.id`, which today is just the combo's index — see
// app/src/combo.c:zmk_combos_set()). Key positions and layer restriction aren't editable yet.
zmk_studio_Response set_combo(const zmk_studio_Request *req) {
    LOG_DBG("");
    const zmk_combos_SetComboRequest *set_req = &req->subsystem.combos.request_type.set_combo;

    if (!set_req->has_combo) {
        return COMBOS_RESPONSE(set_combo,
                               set_combo_err(zmk_combos_SetComboErrorCode_SET_COMBO_ERR_GENERIC));
    }

    const zmk_combos_Combo *combo = &set_req->combo;

    struct zmk_behavior_binding binding = {0};
    if (combo->has_binding) {
        const char *behavior_name =
            zmk_behavior_find_behavior_name_from_local_id(combo->binding.behavior_id);

        if (!behavior_name) {
            return COMBOS_RESPONSE(
                set_combo,
                set_combo_err(zmk_combos_SetComboErrorCode_SET_COMBO_ERR_INVALID_BEHAVIOR));
        }

        binding = (struct zmk_behavior_binding){
            .behavior_dev = behavior_name,
            .param1 = combo->binding.param1,
            .param2 = combo->binding.param2,
        };

        if (zmk_behavior_validate_binding(&binding) < 0) {
            return COMBOS_RESPONSE(
                set_combo,
                set_combo_err(zmk_combos_SetComboErrorCode_SET_COMBO_ERR_INVALID_BEHAVIOR));
        }
    }

    // This handler doesn't touch key positions - fetch the combo's current ones unchanged so
    // zmk_combos_set() (which always takes the full field set) doesn't clobber them.
    size_t existing_len;
    const int32_t *existing_positions = zmk_combos_get_key_positions(combo->id, &existing_len);

    int ret = zmk_combos_set(combo->id, combo->timeout_ms, combo->require_prior_idle_ms,
                             combo->slow_release, binding, existing_positions, existing_len);

    if (ret < 0) {
        return COMBOS_RESPONSE(set_combo,
                               set_combo_err(zmk_combos_SetComboErrorCode_SET_COMBO_ERR_INVALID_ID));
    }

    return COMBOS_RESPONSE(set_combo, ((zmk_combos_SetComboResponse){
                                          .which_result = zmk_combos_SetComboResponse_ok_tag,
                                          .result = {.ok = true},
                                      }));
}

static zmk_combos_SetComboKeyPositionsResponse
set_combo_key_positions_err(zmk_combos_SetComboErrorCode err) {
    return (zmk_combos_SetComboKeyPositionsResponse){
        .which_result = zmk_combos_SetComboKeyPositionsResponse_err_tag,
        .result = {.err = err},
    };
}

// Milestone 4: change which key(s) trigger an existing combo. Uses fixed slots (see
// SetComboKeyPositionsRequest in combos.proto) rather than a repeated field, since ZMK's generic
// RPC decoder can't populate pb_callback_t/repeated fields on incoming requests.
zmk_studio_Response set_combo_key_positions(const zmk_studio_Request *req) {
    LOG_DBG("");
    const zmk_combos_SetComboKeyPositionsRequest *set_req =
        &req->subsystem.combos.request_type.set_combo_key_positions;

    if (set_req->count > 4) {
        return COMBOS_RESPONSE(
            set_combo_key_positions,
            set_combo_key_positions_err(zmk_combos_SetComboErrorCode_SET_COMBO_ERR_TOO_MANY_KEYS));
    }

    int32_t positions[4] = {
        (int32_t)set_req->key_position_0,
        (int32_t)set_req->key_position_1,
        (int32_t)set_req->key_position_2,
        (int32_t)set_req->key_position_3,
    };

    // Preserve everything else about the combo unchanged; only key_positions is being replaced.
    struct zmk_behavior_binding binding = {0};
    const struct zmk_behavior_binding *existing_binding =
        zmk_combos_get_behavior(set_req->combo_id);
    if (existing_binding) {
        binding = *existing_binding;
    }

    int ret = zmk_combos_set(set_req->combo_id,
                             (int32_t)zmk_combos_get_timeout_ms(set_req->combo_id),
                             (int32_t)zmk_combos_get_require_prior_idle_ms(set_req->combo_id),
                             zmk_combos_get_slow_release(set_req->combo_id), binding, positions,
                             set_req->count);

    if (ret == -E2BIG) {
        return COMBOS_RESPONSE(
            set_combo_key_positions,
            set_combo_key_positions_err(zmk_combos_SetComboErrorCode_SET_COMBO_ERR_TOO_MANY_KEYS));
    } else if (ret < 0) {
        return COMBOS_RESPONSE(
            set_combo_key_positions,
            set_combo_key_positions_err(zmk_combos_SetComboErrorCode_SET_COMBO_ERR_INVALID_ID));
    }

    return COMBOS_RESPONSE(set_combo_key_positions,
                           ((zmk_combos_SetComboKeyPositionsResponse){
                               .which_result = zmk_combos_SetComboKeyPositionsResponse_ok_tag,
                               .result = {.ok = true},
                           }));
}

ZMK_RPC_SUBSYSTEM_HANDLER(combos, list_combos, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(combos, set_combo_key_positions, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(combos, set_combo, ZMK_STUDIO_RPC_HANDLER_SECURED);

/* core resetSettings(初期設定に戻す)でコンボの保存済み編集も出荷時に戻す(F2)。
 * これが無いと「キーマップだけ戻ってコンボは残る」不整合になる。 */
static int combos_settings_reset(void) { return zmk_combos_reset_settings(); }

ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(combos, combos_settings_reset);
