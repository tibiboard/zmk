/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/macros.h>
#include <zmk/studio/rpc.h>

#include <pb_encode.h>

ZMK_RPC_SUBSYSTEM(macros)

#define MACROS_RESPONSE(type, ...) ZMK_RPC_RESPONSE(macros, type, __VA_ARGS__)

/* Scope: lists the devicetree-configured macros (name, default wait/tap timing, and the full
 * step sequence as behavior bindings — control steps like &macro_tap or
 * &macro_pause_for_release are just bindings to those special behaviors), and edits an existing
 * macro's timing or replaces individual steps in place (effective immediately, not persisted
 * across reboots). Adding/removing steps or whole macro instances is out of scope: the step
 * array's size is fixed by the devicetree initializer, mirroring how combo key-positions are
 * out of scope in combo_subsystem.c. */

// Mirrors the DEVICE_DT_NAME()-based name matching in behavior_macro.c's handle_control_binding()
// (that logic is static there and not worth exporting just for this read-only classification) so
// the app can render press/release/tap intent without name-matching behaviors itself.
static zmk_macros_MacroBindingKind classify_binding(const struct zmk_behavior_binding *binding) {
    if (!binding->behavior_dev) {
        return zmk_macros_MacroBindingKind_MACRO_BINDING_KIND_NORMAL;
    }

    if (strcmp(binding->behavior_dev, "macro_tap") == 0) {
        return zmk_macros_MacroBindingKind_MACRO_BINDING_KIND_MODE_TAP;
    } else if (strcmp(binding->behavior_dev, "macro_press") == 0) {
        return zmk_macros_MacroBindingKind_MACRO_BINDING_KIND_MODE_PRESS;
    } else if (strcmp(binding->behavior_dev, "macro_release") == 0) {
        return zmk_macros_MacroBindingKind_MACRO_BINDING_KIND_MODE_RELEASE;
    } else if (strcmp(binding->behavior_dev, "macro_wait_time") == 0) {
        return zmk_macros_MacroBindingKind_MACRO_BINDING_KIND_WAIT_TIME;
    } else if (strcmp(binding->behavior_dev, "macro_tap_time") == 0) {
        return zmk_macros_MacroBindingKind_MACRO_BINDING_KIND_TAP_TIME;
    } else if (strcmp(binding->behavior_dev, "macro_pause_for_release") == 0) {
        return zmk_macros_MacroBindingKind_MACRO_BINDING_KIND_PAUSE_FOR_RELEASE;
    } else if (strcmp(binding->behavior_dev, "macro_param_1to1") == 0 ||
               strcmp(binding->behavior_dev, "macro_param_1to2") == 0 ||
               strcmp(binding->behavior_dev, "macro_param_2to1") == 0 ||
               strcmp(binding->behavior_dev, "macro_param_2to2") == 0) {
        return zmk_macros_MacroBindingKind_MACRO_BINDING_KIND_PARAM_MAPPING;
    }

    return zmk_macros_MacroBindingKind_MACRO_BINDING_KIND_NORMAL;
}

static bool encode_packed_uint32_array(pb_ostream_t *stream, const pb_field_t *field,
                                       const uint32_t *values, size_t len) {
    if (len == 0) {
        return true;
    }

    // See the identical helper (and its explanatory comment) in combo_subsystem.c: a packed
    // repeated scalar field needs an explicit length-delimited tag, not the varint wire type
    // pb_encode_tag_for_field would infer from the field's own (non-repeated) element type.
    if (!pb_encode_tag(stream, PB_WT_STRING, field->tag)) {
        return false;
    }

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

// Fixed cap instead of a VLA/heap allocation: real macros here are a handful of steps, and this
// only needs to cover whatever the largest devicetree-defined macro on the keyboard is.
#define MAX_ENCODABLE_MACRO_STEPS 64

static bool encode_macro_binding_kinds(pb_ostream_t *stream, const pb_field_t *field,
                                       void *const *arg) {
    const size_t index = *(size_t *)*arg;

    size_t len;
    const struct zmk_behavior_binding *bindings = zmk_macros_get_bindings(index, &len);

    if (len > MAX_ENCODABLE_MACRO_STEPS) {
        len = MAX_ENCODABLE_MACRO_STEPS;
    }

    uint32_t kinds[MAX_ENCODABLE_MACRO_STEPS];
    for (size_t i = 0; i < len; i++) {
        kinds[i] = (uint32_t)classify_binding(&bindings[i]);
    }

    return encode_packed_uint32_array(stream, field, kinds, len);
}

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
            /* chibiboard FW45 (2026-10-01): control steps (wait time etc.) report their pseudo id
             * so the app can write them back (see zmk/macros.h) */
            if (bb.behavior_id == UINT16_MAX) {
                bb.behavior_id = zmk_macros_control_pseudo_id(bindings[i].behavior_dev);
            }
        }
        bb.param1 = bindings[i].param1;
        bb.param2 = bindings[i].param2;

        if (!pb_encode_submessage(stream, &zmk_keymap_BehaviorBinding_msg, &bb)) {
            return false;
        }
    }

    return true;
}

static bool encode_macro_bindings(pb_ostream_t *stream, const pb_field_t *field,
                                  void *const *arg) {
    const size_t index = *(size_t *)*arg;

    size_t len;
    const struct zmk_behavior_binding *bindings = zmk_macros_get_bindings(index, &len);

    return encode_behavior_binding_array(stream, field, bindings, len);
}

// The macro's own devicetree node is ALSO registered as an ordinary ZMK behavior (that's how a
// macro can be used as a keymap binding at all), so it already has a `zmk_behavior_local_id_t`
// distinct from this subsystem's array index. Exposing THAT as the public `id` (instead of the
// raw array index) lets the app cross-reference a macro against
// `zmk.keymap.BehaviorBinding.behavior_id` in the keymap to show which key position(s) invoke it.
static zmk_behavior_local_id_t local_id_for_index(size_t index) {
    const char *name = zmk_macros_get_name(index);
    if (!name) {
        return 0;
    }
    return zmk_behavior_get_local_id(name);
}

static int index_for_local_id(zmk_behavior_local_id_t id) {
    size_t count = zmk_macros_get_count();
    for (size_t i = 0; i < count; i++) {
        if (local_id_for_index(i) == id) {
            return (int)i;
        }
    }
    return -1;
}

// Not itself a pb_callback (the `arg` double-indirection only matters for functions registered
// as `field.funcs.encode`); this is a plain helper invoked directly by encode_all_macros.
static bool encode_macro(pb_ostream_t *stream, const pb_field_t *field, size_t index) {
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    zmk_macros_Macro macro = zmk_macros_Macro_init_zero;

    macro.id = local_id_for_index(index);
    macro.wait_ms = (uint32_t)zmk_macros_get_wait_ms(index);
    macro.tap_ms = (uint32_t)zmk_macros_get_tap_ms(index);
    macro.name.funcs.encode = encode_string;
    macro.name.arg = (void *)zmk_macros_get_name(index);
    macro.bindings.funcs.encode = encode_macro_bindings;
    macro.bindings.arg = (void *)&index;
    macro.binding_kinds.funcs.encode = encode_macro_binding_kinds;
    macro.binding_kinds.arg = (void *)&index;

    return pb_encode_submessage(stream, &zmk_macros_Macro_msg, &macro);
}

static bool encode_all_macros(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    size_t count = zmk_macros_get_count();

    for (size_t i = 0; i < count; i++) {
        if (!encode_macro(stream, field, i)) {
            return false;
        }
    }

    return true;
}

zmk_studio_Response list_macros(const zmk_studio_Request *req) {
    LOG_DBG("");

    zmk_macros_ListMacrosResponse resp = zmk_macros_ListMacrosResponse_init_zero;

    resp.macros.funcs.encode = encode_all_macros;

    return MACROS_RESPONSE(list_macros, resp);
}

zmk_studio_Response set_macro_properties(const zmk_studio_Request *req) {
    LOG_DBG("");
    const zmk_macros_SetMacroPropertiesRequest *set_req =
        &req->subsystem.macros.request_type.set_macro_properties;

    int index = index_for_local_id(set_req->macro_id);
    int ret = index < 0 ? -EINVAL
                        : zmk_macros_set_timing(index, set_req->wait_ms, set_req->tap_ms);

    if (ret < 0) {
        return MACROS_RESPONSE(
            set_macro_properties,
            ((zmk_macros_SetMacroPropertiesResponse){
                .which_result = zmk_macros_SetMacroPropertiesResponse_err_tag,
                .result = {.err = zmk_macros_MacroErrorCode_MACRO_ERR_INVALID_ID},
            }));
    }

    return MACROS_RESPONSE(set_macro_properties,
                           ((zmk_macros_SetMacroPropertiesResponse){
                               .which_result = zmk_macros_SetMacroPropertiesResponse_ok_tag,
                               .result = {.ok = true},
                           }));
}

static zmk_macros_SetMacroBindingResponse set_binding_err(zmk_macros_MacroErrorCode err) {
    return (zmk_macros_SetMacroBindingResponse){
        .which_result = zmk_macros_SetMacroBindingResponse_err_tag,
        .result = {.err = err},
    };
}

zmk_studio_Response set_macro_binding(const zmk_studio_Request *req) {
    LOG_DBG("");
    const zmk_macros_SetMacroBindingRequest *set_req =
        &req->subsystem.macros.request_type.set_macro_binding;

    if (!set_req->has_binding) {
        return MACROS_RESPONSE(set_macro_binding,
                               set_binding_err(zmk_macros_MacroErrorCode_MACRO_ERR_GENERIC));
    }

    const char *behavior_name =
        zmk_behavior_find_behavior_name_from_local_id(set_req->binding.behavior_id);

    /* chibiboard FW45 (2026-10-01): a control step (wait time etc.) arrives with its pseudo id.
     * Control steps are not devices, so the normal metadata validation cannot run for them;
     * only the time steps take a value, and it is limited to 0..60000 ms. */
    const char *control_name = NULL;
    if (!behavior_name) {
        control_name = zmk_macros_control_name_for_pseudo_id(set_req->binding.behavior_id);
        behavior_name = control_name;
    }

    if (!behavior_name) {
        return MACROS_RESPONSE(
            set_macro_binding,
            set_binding_err(zmk_macros_MacroErrorCode_MACRO_ERR_INVALID_BEHAVIOR));
    }

    struct zmk_behavior_binding binding = {
        .behavior_dev = behavior_name,
        .param1 = set_req->binding.param1,
        .param2 = set_req->binding.param2,
    };

    if (control_name) {
        if (binding.param1 > 60000) {
            return MACROS_RESPONSE(
                set_macro_binding,
                set_binding_err(zmk_macros_MacroErrorCode_MACRO_ERR_INVALID_BEHAVIOR));
        }
    } else if (zmk_behavior_validate_binding(&binding) < 0) {
        return MACROS_RESPONSE(
            set_macro_binding,
            set_binding_err(zmk_macros_MacroErrorCode_MACRO_ERR_INVALID_BEHAVIOR));
    }

    int index = index_for_local_id(set_req->macro_id);
    int ret = index < 0 ? -EINVAL
                        : zmk_macros_set_binding(index, set_req->binding_index, binding);

    if (ret == -ERANGE) {
        return MACROS_RESPONSE(
            set_macro_binding,
            set_binding_err(zmk_macros_MacroErrorCode_MACRO_ERR_INVALID_INDEX));
    } else if (ret < 0) {
        return MACROS_RESPONSE(set_macro_binding,
                               set_binding_err(zmk_macros_MacroErrorCode_MACRO_ERR_INVALID_ID));
    }

    return MACROS_RESPONSE(set_macro_binding,
                           ((zmk_macros_SetMacroBindingResponse){
                               .which_result = zmk_macros_SetMacroBindingResponse_ok_tag,
                               .result = {.ok = true},
                           }));
}

ZMK_RPC_SUBSYSTEM_HANDLER(macros, list_macros, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(macros, set_macro_properties, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(macros, set_macro_binding, ZMK_STUDIO_RPC_HANDLER_SECURED);

/* core resetSettings(初期設定に戻す)でマクロの保存済み編集も出荷時に戻す(F2) */
static int macros_settings_reset(void) { return zmk_macros_reset_settings(); }

ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(macros, macros_settings_reset);
