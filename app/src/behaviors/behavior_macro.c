/*
 * Copyright (c) 2022 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/keymap.h>
#include <zmk/macros.h>

#if IS_ENABLED(CONFIG_ZMK_STUDIO_MACROS) && IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#include <stdio.h>
#include <stdlib.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* chibiboard FW45 (2026-10-01): fixed pseudo ids for macro control steps (see zmk/macros.h).
 * The names must match the devicetree node names used by handle_control_binding() below. */
static const struct {
    uint16_t id;
    const char *name;
} macro_control_pseudo_ids[] = {
    {0xFFF0, "macro_tap"},       {0xFFF1, "macro_press"},    {0xFFF2, "macro_release"},
    {0xFFF3, "macro_wait_time"}, {0xFFF4, "macro_tap_time"}, {0xFFF5, "macro_pause_for_release"},
};

uint16_t zmk_macros_control_pseudo_id(const char *behavior_name) {
    if (!behavior_name) {
        return ZMK_MACRO_CONTROL_PSEUDO_ID_NONE;
    }
    for (size_t i = 0; i < ARRAY_SIZE(macro_control_pseudo_ids); i++) {
        if (strcmp(behavior_name, macro_control_pseudo_ids[i].name) == 0) {
            return macro_control_pseudo_ids[i].id;
        }
    }
    return ZMK_MACRO_CONTROL_PSEUDO_ID_NONE;
}

const char *zmk_macros_control_name_for_pseudo_id(uint32_t id) {
    for (size_t i = 0; i < ARRAY_SIZE(macro_control_pseudo_ids); i++) {
        if (macro_control_pseudo_ids[i].id == id) {
            return macro_control_pseudo_ids[i].name;
        }
    }
    return NULL;
}

enum behavior_macro_mode {
    MACRO_MODE_TAP,
    MACRO_MODE_PRESS,
    MACRO_MODE_RELEASE,
};

enum param_source { PARAM_SOURCE_BINDING, PARAM_SOURCE_MACRO_1ST, PARAM_SOURCE_MACRO_2ND };

struct behavior_macro_trigger_state {
    uint32_t wait_ms;
    uint32_t tap_ms;
    enum behavior_macro_mode mode;
    uint16_t start_index;
    uint16_t count;
    enum param_source param1_source;
    enum param_source param2_source;
};

struct behavior_macro_state {
    struct behavior_macro_trigger_state release_state;

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    struct behavior_parameter_metadata_set set;
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

    uint32_t press_bindings_count;
};

struct behavior_macro_config {
    uint32_t default_wait_ms;
    uint32_t default_tap_ms;
    uint32_t count;
    struct zmk_behavior_binding bindings[];
};

#define TAP_MODE DEVICE_DT_NAME(DT_INST(0, zmk_macro_control_mode_tap))
#define PRESS_MODE DEVICE_DT_NAME(DT_INST(0, zmk_macro_control_mode_press))
#define REL_MODE DEVICE_DT_NAME(DT_INST(0, zmk_macro_control_mode_release))

#define TAP_TIME DEVICE_DT_NAME(DT_INST(0, zmk_macro_control_tap_time))
#define WAIT_TIME DEVICE_DT_NAME(DT_INST(0, zmk_macro_control_wait_time))
#define WAIT_REL DEVICE_DT_NAME(DT_INST(0, zmk_macro_pause_for_release))

#define P1TO1 DEVICE_DT_NAME(DT_INST(0, zmk_macro_param_1to1))
#define P1TO2 DEVICE_DT_NAME(DT_INST(0, zmk_macro_param_1to2))
#define P2TO1 DEVICE_DT_NAME(DT_INST(0, zmk_macro_param_2to1))
#define P2TO2 DEVICE_DT_NAME(DT_INST(0, zmk_macro_param_2to2))

#define ZM_IS_NODE_MATCH(a, b) (strcmp(a, b) == 0)
#define IS_TAP_MODE(dev) ZM_IS_NODE_MATCH(dev, TAP_MODE)
#define IS_PRESS_MODE(dev) ZM_IS_NODE_MATCH(dev, PRESS_MODE)
#define IS_RELEASE_MODE(dev) ZM_IS_NODE_MATCH(dev, REL_MODE)

#define IS_TAP_TIME(dev) ZM_IS_NODE_MATCH(dev, TAP_TIME)
#define IS_WAIT_TIME(dev) ZM_IS_NODE_MATCH(dev, WAIT_TIME)
#define IS_PAUSE(dev) ZM_IS_NODE_MATCH(dev, WAIT_REL)

#define IS_P1TO1(dev) ZM_IS_NODE_MATCH(dev, P1TO1)
#define IS_P1TO2(dev) ZM_IS_NODE_MATCH(dev, P1TO2)
#define IS_P2TO1(dev) ZM_IS_NODE_MATCH(dev, P2TO1)
#define IS_P2TO2(dev) ZM_IS_NODE_MATCH(dev, P2TO2)

static bool handle_control_binding(struct behavior_macro_trigger_state *state,
                                   const struct zmk_behavior_binding *binding) {
    // A binding loaded from settings may (transiently) have no resolved behavior name if the
    // behavior local-id table hadn't been loaded yet; guard the strcmp-based matching below.
    if (binding->behavior_dev == NULL) {
        return false;
    }

    if (IS_TAP_MODE(binding->behavior_dev)) {
        state->mode = MACRO_MODE_TAP;
        LOG_DBG("macro mode set: tap");
    } else if (IS_PRESS_MODE(binding->behavior_dev)) {
        state->mode = MACRO_MODE_PRESS;
        LOG_DBG("macro mode set: press");
    } else if (IS_RELEASE_MODE(binding->behavior_dev)) {
        state->mode = MACRO_MODE_RELEASE;
        LOG_DBG("macro mode set: release");
    } else if (IS_TAP_TIME(binding->behavior_dev)) {
        state->tap_ms = binding->param1;
        LOG_DBG("macro tap time set: %d", state->tap_ms);
    } else if (IS_WAIT_TIME(binding->behavior_dev)) {
        state->wait_ms = binding->param1;
        LOG_DBG("macro wait time set: %d", state->wait_ms);
    } else if (IS_P1TO1(binding->behavior_dev)) {
        state->param1_source = PARAM_SOURCE_MACRO_1ST;
        LOG_DBG("macro param: 1to1");
    } else if (IS_P1TO2(binding->behavior_dev)) {
        state->param2_source = PARAM_SOURCE_MACRO_1ST;
        LOG_DBG("macro param: 1to2");
    } else if (IS_P2TO1(binding->behavior_dev)) {
        state->param1_source = PARAM_SOURCE_MACRO_2ND;
        LOG_DBG("macro param: 2to1");
    } else if (IS_P2TO2(binding->behavior_dev)) {
        state->param2_source = PARAM_SOURCE_MACRO_2ND;
        LOG_DBG("macro param: 2to2");
    } else {
        return false;
    }

    return true;
}

// Precalculates the state used to play the "release" half of the macro (everything after
// &macro_pause_for_release, if present). Called at init, and again by zmk_macros_set_binding()
// whenever the Studio "macros" RPC subsystem edits a step, since the split point and the
// initial control-mode/timing state all depend on the step list contents.
static void macro_recalculate_state(const struct behavior_macro_config *cfg,
                                    struct behavior_macro_state *state) {
    state->release_state = (struct behavior_macro_trigger_state){0};
    state->press_bindings_count = cfg->count;
    state->release_state.start_index = cfg->count;
    state->release_state.count = 0;

    LOG_DBG("Precalculate initial release state:");
    for (int i = 0; i < cfg->count; i++) {
        if (handle_control_binding(&state->release_state, &cfg->bindings[i])) {
            // Updated state used for initial state on release.
        } else if (cfg->bindings[i].behavior_dev != NULL &&
                   IS_PAUSE(cfg->bindings[i].behavior_dev)) {
            state->release_state.start_index = i + 1;
            state->release_state.count = cfg->count - state->release_state.start_index;
            state->press_bindings_count = i;
            LOG_DBG("Release will resume at %d", state->release_state.start_index);
            break;
        } else {
            // Mostly ignore regular invokable bindings, except they will consume macro parameters
            state->release_state.param1_source = PARAM_SOURCE_BINDING;
            state->release_state.param2_source = PARAM_SOURCE_BINDING;
        }
    }

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    // Drop any parameter metadata cached by get_macro_parameter_metadata(); it is derived from
    // the step bindings, so it must be recomputed lazily after an edit.
    state->set = (struct behavior_parameter_metadata_set){0};
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
}

static int behavior_macro_init(const struct device *dev) {
    macro_recalculate_state(dev->config, dev->data);
    return 0;
};

static uint32_t select_param(enum param_source param_source, uint32_t source_binding,
                             const struct zmk_behavior_binding *macro_binding) {
    switch (param_source) {
    case PARAM_SOURCE_MACRO_1ST:
        return macro_binding->param1;
    case PARAM_SOURCE_MACRO_2ND:
        return macro_binding->param2;
    default:
        return source_binding;
    }
};

static void replace_params(struct behavior_macro_trigger_state *state,
                           struct zmk_behavior_binding *binding,
                           const struct zmk_behavior_binding *macro_binding) {
    binding->param1 = select_param(state->param1_source, binding->param1, macro_binding);
    binding->param2 = select_param(state->param2_source, binding->param2, macro_binding);

    state->param1_source = PARAM_SOURCE_BINDING;
    state->param2_source = PARAM_SOURCE_BINDING;
}

static void queue_macro(struct zmk_behavior_binding_event *event,
                        const struct zmk_behavior_binding bindings[],
                        struct behavior_macro_trigger_state state,
                        const struct zmk_behavior_binding *macro_binding) {
    LOG_DBG("Iterating macro bindings - starting: %d, count: %d", state.start_index, state.count);
    for (int i = state.start_index; i < state.start_index + state.count; i++) {
        if (!handle_control_binding(&state, &bindings[i])) {
            struct zmk_behavior_binding binding = bindings[i];
            replace_params(&state, &binding, macro_binding);

            switch (state.mode) {
            case MACRO_MODE_TAP:
                zmk_behavior_queue_add(event, binding, true, state.tap_ms);
                zmk_behavior_queue_add(event, binding, false, state.wait_ms);
                break;
            case MACRO_MODE_PRESS:
                zmk_behavior_queue_add(event, binding, true, state.wait_ms);
                break;
            case MACRO_MODE_RELEASE:
                zmk_behavior_queue_add(event, binding, false, state.wait_ms);
                break;
            default:
                LOG_ERR("Unknown macro mode: %d", state.mode);
                break;
            }
        }
    }
}

static int on_macro_binding_pressed(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_macro_config *cfg = dev->config;
    struct behavior_macro_state *state = dev->data;
    struct behavior_macro_trigger_state trigger_state = {.mode = MACRO_MODE_TAP,
                                                         .tap_ms = cfg->default_tap_ms,
                                                         .wait_ms = cfg->default_wait_ms,
                                                         .start_index = 0,
                                                         .count = state->press_bindings_count};

    queue_macro(&event, cfg->bindings, trigger_state, binding);

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_macro_binding_released(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_macro_config *cfg = dev->config;
    struct behavior_macro_state *state = dev->data;

    queue_macro(&event, cfg->bindings, state->release_state, binding);

    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static void assign_values_to_set(enum param_source param_source,
                                 struct behavior_parameter_metadata_set *set,
                                 const struct behavior_parameter_value_metadata *values,
                                 size_t values_len) {
    if (param_source == PARAM_SOURCE_MACRO_1ST) {
        set->param1_values = values;
        set->param1_values_len = values_len;
    } else {
        set->param2_values = values;
        set->param2_values_len = values_len;
    }
}

// This function will dynamically determine the parameter metadata for a particular macro by
// inspecting the macro *bindings* to see what behaviors in that list receive the macro parameters,
// and then using the metadata from those behaviors for the macro itself.
//
// Care need be taken, where a behavior in the list takes two parameters, and the macro passes along
// a value for the *second* parameter, we need to make sure we find the right metadata set for the
// referenced behavior that matches the first parameter.
static int get_macro_parameter_metadata(const struct device *macro,
                                        struct behavior_parameter_metadata *param_metadata) {
    const struct behavior_macro_config *cfg = macro->config;
    struct behavior_macro_state *data = macro->data;
    struct behavior_macro_trigger_state state = {0};

    for (int i = 0; (i < cfg->count) && (!data->set.param1_values || !data->set.param2_values);
         i++) {
        if (handle_control_binding(&state, &cfg->bindings[i]) ||
            (state.param1_source == PARAM_SOURCE_BINDING &&
             state.param2_source == PARAM_SOURCE_BINDING)) {
            continue;
        }

        LOG_DBG("checking %d for the given state", i);

        struct behavior_parameter_metadata binding_meta;
        int err = behavior_get_parameter_metadata(
            zmk_behavior_get_binding(cfg->bindings[i].behavior_dev), &binding_meta);
        if (err < 0 || binding_meta.sets_len == 0) {
            LOG_WRN("Failed to fetch macro binding parameter details %d", err);
            return -ENOTSUP;
        }

        // If both macro parameters get passed to this one entry, use
        // the metadata for this behavior verbatim.
        if (state.param1_source != PARAM_SOURCE_BINDING &&
            state.param2_source != PARAM_SOURCE_BINDING) {
            param_metadata->sets_len = binding_meta.sets_len;
            param_metadata->sets = binding_meta.sets;
            return 0;
        }

        if (state.param1_source != PARAM_SOURCE_BINDING) {
            assign_values_to_set(state.param1_source, &data->set,
                                 binding_meta.sets[0].param1_values,
                                 binding_meta.sets[0].param1_values_len);
        }

        if (state.param2_source != PARAM_SOURCE_BINDING) {
            // For the param2 metadata, we need to find a set that matches fully bound first
            // parameter of our macro entry, and use the metadata from that set.
            for (int s = 0; s < binding_meta.sets_len; s++) {
                if (zmk_behavior_validate_param_values(binding_meta.sets[s].param1_values,
                                                       binding_meta.sets[s].param1_values_len,
                                                       cfg->bindings[i].param1) >= 0) {
                    assign_values_to_set(state.param2_source, &data->set,
                                         binding_meta.sets[s].param2_values,
                                         binding_meta.sets[s].param2_values_len);
                    break;
                }
            }
        }

        state.param1_source = PARAM_SOURCE_BINDING;
        state.param2_source = PARAM_SOURCE_BINDING;
    }

    param_metadata->sets_len = 1;
    param_metadata->sets = &data->set;

    return 0;
}

#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_driver_api behavior_macro_driver_api = {
    .binding_pressed = on_macro_binding_pressed,
    .binding_released = on_macro_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = get_macro_parameter_metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

#define TRANSFORMED_BEHAVIORS(n)                                                                   \
    {LISTIFY(DT_PROP_LEN(n, bindings), ZMK_KEYMAP_EXTRACT_BINDING, (, ), n)},

#define MACRO_INST(inst)                                                                           \
    static struct behavior_macro_state behavior_macro_state_##inst = {};                           \
    static struct behavior_macro_config behavior_macro_config_##inst = {                           \
        .default_wait_ms = DT_PROP_OR(inst, wait_ms, CONFIG_ZMK_MACRO_DEFAULT_WAIT_MS),            \
        .default_tap_ms = DT_PROP_OR(inst, tap_ms, CONFIG_ZMK_MACRO_DEFAULT_TAP_MS),               \
        .count = DT_PROP_LEN(inst, bindings),                                                      \
        .bindings = TRANSFORMED_BEHAVIORS(inst)};                                                  \
    /* Immutable devicetree-derived copy (flash), kept so "Restore Stock Settings" can put the    \
     * live (mutable, Studio-editable) config back without a reboot - mirrors combo.c's           \
     * stock_combos[]. Uses the same GCC flexible-array-member static-init extension as the       \
     * live copy above. */                                                                        \
    static const struct behavior_macro_config behavior_macro_stock_config_##inst = {              \
        .default_wait_ms = DT_PROP_OR(inst, wait_ms, CONFIG_ZMK_MACRO_DEFAULT_WAIT_MS),            \
        .default_tap_ms = DT_PROP_OR(inst, tap_ms, CONFIG_ZMK_MACRO_DEFAULT_TAP_MS),               \
        .count = DT_PROP_LEN(inst, bindings),                                                      \
        .bindings = TRANSFORMED_BEHAVIORS(inst)};                                                  \
    BEHAVIOR_DT_DEFINE(inst, behavior_macro_init, NULL, &behavior_macro_state_##inst,              \
                       &behavior_macro_config_##inst, POST_KERNEL,                                 \
                       CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_macro_driver_api);

DT_FOREACH_STATUS_OKAY(zmk_behavior_macro, MACRO_INST)
DT_FOREACH_STATUS_OKAY(zmk_behavior_macro_one_param, MACRO_INST)
DT_FOREACH_STATUS_OKAY(zmk_behavior_macro_two_param, MACRO_INST)

/* --- Accessors for the Studio "macros" RPC subsystem (see zmk/macros.h) --- */

#define ZMK_MACROS_HAS_ANY_INST                                                                    \
    (DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_macro) ||                                              \
     DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_macro_one_param) ||                                    \
     DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_macro_two_param))

#if ZMK_MACROS_HAS_ANY_INST

/* The per-instance config structs defined by MACRO_INST above are mutable RAM (non-const
 * statics) that the runtime reads through dev->config on every invocation, so the setters below
 * can write to them directly and the change takes effect immediately, exactly like the
 * `combos[]` working copy in app/src/combo.c. The step count stays fixed (the config's
 * flexible bindings[] array is sized by the devicetree initializer), so only in-place step
 * replacement is supported. */

struct macro_registry_entry {
    struct behavior_macro_config *cfg;
    struct behavior_macro_state *state;
    /* Stock (devicetree) values for "Restore Stock Settings" */
    const struct behavior_macro_config *stock;
    const char *dev_name;
};

#define MACRO_REGISTRY_ENTRY(inst)                                                                 \
    {.cfg = &behavior_macro_config_##inst,                                                         \
     .state = &behavior_macro_state_##inst,                                                        \
     .stock = &behavior_macro_stock_config_##inst,                                                 \
     .dev_name = DEVICE_DT_NAME(inst)},

static struct macro_registry_entry macro_registry[] = {
    DT_FOREACH_STATUS_OKAY(zmk_behavior_macro, MACRO_REGISTRY_ENTRY)
        DT_FOREACH_STATUS_OKAY(zmk_behavior_macro_one_param, MACRO_REGISTRY_ENTRY)
            DT_FOREACH_STATUS_OKAY(zmk_behavior_macro_two_param, MACRO_REGISTRY_ENTRY)};

size_t zmk_macros_get_count(void) { return ARRAY_SIZE(macro_registry); }

const char *zmk_macros_get_name(size_t index) {
    if (index >= ARRAY_SIZE(macro_registry)) {
        return NULL;
    }

    return macro_registry[index].dev_name;
}

int32_t zmk_macros_get_wait_ms(size_t index) {
    if (index >= ARRAY_SIZE(macro_registry)) {
        return -EINVAL;
    }

    return macro_registry[index].cfg->default_wait_ms;
}

int32_t zmk_macros_get_tap_ms(size_t index) {
    if (index >= ARRAY_SIZE(macro_registry)) {
        return -EINVAL;
    }

    return macro_registry[index].cfg->default_tap_ms;
}

const struct zmk_behavior_binding *zmk_macros_get_bindings(size_t index, size_t *len_out) {
    if (index >= ARRAY_SIZE(macro_registry)) {
        *len_out = 0;
        return NULL;
    }

    *len_out = macro_registry[index].cfg->count;
    return macro_registry[index].cfg->bindings;
}

#if IS_ENABLED(CONFIG_ZMK_STUDIO_MACROS) && IS_ENABLED(CONFIG_SETTINGS)

/* --- Settings-based persistence, mirroring app/src/keymap.c ---
 *
 * "macros/t/<index>" stores a macro's default wait/tap timing, and
 * "macros/b/<index>/<binding_index>" stores one step's binding. Entries are keyed by *index into
 * the compiled-in registry*, so saved settings only apply cleanly to the same compiled .keymap
 * structure they were saved against (same macro count/order/step counts) - the same accepted
 * tradeoff the Keymap tab already has. Step counts are fixed per macro (this project pins every
 * macro to 30 steps in the devicetree), so per-step entries always line up.
 */

#define MACRO_TIMING_SETTINGS_KEY "macros/t/%d"
#define MACRO_BINDING_SETTINGS_KEY "macros/b/%d/%d"

struct macro_timing_setting {
    uint32_t wait_ms;
    uint32_t tap_ms;
} __packed;

struct macro_binding_setting {
    zmk_behavior_local_id_t behavior_local_id;
    uint32_t param1;
    uint32_t param2;
} __packed;

static int macro_save_timing(size_t index) {
    struct macro_timing_setting setting = {
        .wait_ms = macro_registry[index].cfg->default_wait_ms,
        .tap_ms = macro_registry[index].cfg->default_tap_ms,
    };

    char setting_name[20];
    sprintf(setting_name, MACRO_TIMING_SETTINGS_KEY, (int)index);

    return settings_save_one(setting_name, &setting, sizeof(setting));
}

static int macro_save_binding(size_t index, size_t binding_index) {
    const struct zmk_behavior_binding *binding = &macro_registry[index].cfg->bindings[binding_index];

    /* FW45: control steps have no local id; save their pseudo id so they survive a reboot */
    zmk_behavior_local_id_t lid = zmk_behavior_get_local_id(binding->behavior_dev);
    if (lid == UINT16_MAX) {
        lid = zmk_macros_control_pseudo_id(binding->behavior_dev);
    }

    struct macro_binding_setting setting = {
        .behavior_local_id = lid,
        .param1 = binding->param1,
        .param2 = binding->param2,
    };

    char setting_name[24];
    sprintf(setting_name, MACRO_BINDING_SETTINGS_KEY, (int)index, (int)binding_index);

    return settings_save_one(setting_name, &setting, sizeof(setting));
}

/* --- Debounced flush (2026-07-22, combo.cと同じ設計) ---
 * アプリの復元はマクロ最大30ステップ×全マクロを連射で書いてくるが、従来は1RPCごとに
 * 同期フラッシュ書き込みだったため復元が数百回のNVS書き込みになっていた(復元1時間級の主犯)。
 * 編集はRAMに即時反映済みなので、フラッシュへの書き出しだけを最終編集の
 * MACRO_SAVE_DEBOUNCE_MS 後に1回にまとめる。電源断でその猶予中の編集が失われ得るのは
 * combo.cと同じ割り切り。ステップ数は本プロジェクトでは30固定(uint32_tのビットに収まる)。 */

#define MACRO_SAVE_DEBOUNCE_MS 2000

static uint8_t macro_timing_dirty[DIV_ROUND_UP(ARRAY_SIZE(macro_registry), 8)];
static uint32_t macro_binding_dirty[ARRAY_SIZE(macro_registry)];

/* まとめ書き完了の合図。combo.c参照(weak、実装はvibe_handler.c) */
__attribute__((weak)) void tibiboard_on_settings_flushed(void) {}

static void macro_save_work_handler(struct k_work *work) {
    int wrote = 0;
    for (size_t i = 0; i < ARRAY_SIZE(macro_registry); i++) {
        if (macro_timing_dirty[i / 8] & BIT(i % 8)) {
            WRITE_BIT(macro_timing_dirty[i / 8], i % 8, 0);
            int ret = macro_save_timing(i);
            if (ret < 0) {
                LOG_ERR("Failed to persist macro %d timing (%d)", (int)i, ret);
            } else {
                wrote++;
            }
        }
        while (macro_binding_dirty[i]) {
            uint32_t b = __builtin_ctz(macro_binding_dirty[i]);
            WRITE_BIT(macro_binding_dirty[i], b, 0);
            int ret = macro_save_binding(i, b);
            if (ret < 0) {
                LOG_ERR("Failed to persist macro %d step %d (%d)", (int)i, (int)b, ret);
            } else {
                wrote++;
            }
        }
    }
    if (wrote > 0) {
        tibiboard_on_settings_flushed();
    }
}

static K_WORK_DELAYABLE_DEFINE(macro_save_work, macro_save_work_handler);

static void macro_schedule_save_timing(size_t index) {
    WRITE_BIT(macro_timing_dirty[index / 8], index % 8, 1);
    k_work_reschedule(&macro_save_work, K_MSEC(MACRO_SAVE_DEBOUNCE_MS));
}

static int macro_schedule_save_binding(size_t index, size_t binding_index) {
    if (binding_index >= 32) {
        /* ビットに収まらない構成(将来31ステップ超)への保険: 従来どおり即時保存 */
        return macro_save_binding(index, binding_index);
    }
    WRITE_BIT(macro_binding_dirty[index], binding_index, 1);
    k_work_reschedule(&macro_save_work, K_MSEC(MACRO_SAVE_DEBOUNCE_MS));
    return 0;
}

// NOTE: like every settings h_set callback in this firmware, this must return 0 (skip with a
// warning) for malformed/legacy/out-of-range entries rather than an error: with the NVS backend a
// nonzero return aborts the remainder of the ENTIRE settings load (all subsystems, newest-first
// iteration), silently reverting every not-yet-loaded setting to compiled-in defaults for that
// boot. See behavior_handle_set() in app/src/behavior.c.
static int macro_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                              void *cb_arg) {
    const char *next;

    if (settings_name_steq(name, "t", &next) && next) {
        char *endptr;
        unsigned long index = strtoul(next, &endptr, 10);
        if (*endptr != '\0') {
            LOG_WRN("Skipping invalid macro index: %s", next);
            return 0;
        }

        if (index >= ARRAY_SIZE(macro_registry)) {
            LOG_WRN("Ignoring saved timing for macro %lu (only %d in this build)", index,
                    (int)ARRAY_SIZE(macro_registry));
            return 0;
        }

        struct macro_timing_setting setting;
        if (len != sizeof(setting)) {
            LOG_WRN("Skipping invalid macro timing setting size %d", (int)len);
            return 0;
        }

        int ret = read_cb(cb_arg, &setting, sizeof(setting));
        if (ret <= 0) {
            LOG_ERR("Failed to read macro timing setting (err %d), skipping", ret);
            return 0;
        }

        macro_registry[index].cfg->default_wait_ms = setting.wait_ms;
        macro_registry[index].cfg->default_tap_ms = setting.tap_ms;
    } else if (settings_name_steq(name, "b", &next) && next) {
        char *endptr;
        unsigned long index = strtoul(next, &endptr, 10);
        if (*endptr != '/') {
            LOG_WRN("Skipping invalid macro index: %s", next);
            return 0;
        }

        unsigned long binding_index = strtoul(endptr + 1, &endptr, 10);
        if (*endptr != '\0') {
            LOG_WRN("Skipping invalid macro binding index: %s", next);
            return 0;
        }

        if (index >= ARRAY_SIZE(macro_registry) ||
            binding_index >= macro_registry[index].cfg->count) {
            LOG_WRN("Ignoring saved binding %lu for macro %lu (out of range in this build)",
                    binding_index, index);
            return 0;
        }

        struct macro_binding_setting setting = {0};
        if (len > sizeof(setting)) {
            LOG_WRN("Skipping invalid macro binding setting size %d", (int)len);
            return 0;
        }

        int ret = read_cb(cb_arg, &setting, len);
        if (ret <= 0) {
            LOG_ERR("Failed to read macro binding setting (err %d), skipping", ret);
            return 0;
        }

        // May be NULL here if the "behavior" local-id settings table hasn't been loaded yet;
        // macro_settings_commit() re-resolves from local_id afterwards, same as keymap.c.
        // handle_control_binding()/macro_recalculate_state() are NULL-name safe.
        const char *behavior_name =
            zmk_behavior_find_behavior_name_from_local_id(setting.behavior_local_id);
        if (!behavior_name) {
            /* FW45: a saved control step (pseudo id) */
            behavior_name = zmk_macros_control_name_for_pseudo_id(setting.behavior_local_id);
        }

        macro_registry[index].cfg->bindings[binding_index] = (struct zmk_behavior_binding){
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
            .local_id = setting.behavior_local_id,
#endif
            .behavior_dev = behavior_name,
            .param1 = setting.param1,
            .param2 = setting.param2,
        };
    }

    return 0;
}

static int macro_settings_commit(void) {
    for (size_t i = 0; i < ARRAY_SIZE(macro_registry); i++) {
        struct macro_registry_entry *entry = &macro_registry[i];

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
        for (size_t b = 0; b < entry->cfg->count; b++) {
            struct zmk_behavior_binding *binding = &entry->cfg->bindings[b];

            if (binding->local_id > 0 && !binding->behavior_dev) {
                binding->behavior_dev =
                    zmk_behavior_find_behavior_name_from_local_id(binding->local_id);
                if (!binding->behavior_dev) {
                    binding->behavior_dev = zmk_macros_control_name_for_pseudo_id(binding->local_id);
                }

                if (!binding->behavior_dev) {
                    LOG_ERR("Failed to find behavior for local ID %d for macro %d step %d after "
                            "settings load",
                            binding->local_id, (int)i, (int)b);
                }
            }
        }
#endif

        // The precalculated release state depends on the step list contents (position of
        // &macro_pause_for_release, leading control-mode/timing steps), so recompute it once per
        // macro after all saved steps have been loaded and names resolved. Cheap and harmless for
        // macros with no saved edits.
        macro_recalculate_state(entry->cfg, entry->state);
    }

    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(macros, "macros", NULL, macro_settings_set, macro_settings_commit,
                               NULL);

/* Bindings can't be compared with memcmp: behavior_dev points at string literals whose addresses
 * differ between the stock copy and a value re-resolved from a settings load / Studio edit even
 * when they name the same behavior. Compare by name + params instead. */
static bool binding_equals_stock(const struct zmk_behavior_binding *a,
                                 const struct zmk_behavior_binding *b) {
    if (a->param1 != b->param1 || a->param2 != b->param2) {
        return false;
    }
    if (!a->behavior_dev || !b->behavior_dev) {
        return a->behavior_dev == b->behavior_dev;
    }
    return strcmp(a->behavior_dev, b->behavior_dev) == 0;
}

/* "Restore Stock Settings": delete only the entries that actually differ from stock (each saved
 * edit implies a stored key; deleting blindly would burn one NVS write per key for ~900 untouched
 * step entries), then put the live config back to the devicetree values in place - no reboot
 * needed, mirroring zmk_keymap_reset_settings(). */
int zmk_macros_reset_settings(void) {
    /* 未フラッシュの編集とその予約を破棄(直後にstock値で上書きされるため。combo.cと同じ) */
    /* _syncで実行中のまとめ書きの完了も待つ(combo.c参照) */
    struct k_work_sync sync;
    k_work_cancel_delayable_sync(&macro_save_work, &sync);
    memset(macro_timing_dirty, 0, sizeof(macro_timing_dirty));
    memset(macro_binding_dirty, 0, sizeof(macro_binding_dirty));

    for (size_t i = 0; i < ARRAY_SIZE(macro_registry); i++) {
        struct macro_registry_entry *entry = &macro_registry[i];
        struct behavior_macro_config *cfg = entry->cfg;
        const struct behavior_macro_config *stock = entry->stock;
        char setting_name[28];
        bool changed = false;

        if (cfg->default_wait_ms != stock->default_wait_ms ||
            cfg->default_tap_ms != stock->default_tap_ms) {
            sprintf(setting_name, MACRO_TIMING_SETTINGS_KEY, (int)i);
            settings_delete(setting_name);
            cfg->default_wait_ms = stock->default_wait_ms;
            cfg->default_tap_ms = stock->default_tap_ms;
            changed = true;
        }

        for (size_t b = 0; b < cfg->count; b++) {
            if (!binding_equals_stock(&cfg->bindings[b], &stock->bindings[b])) {
                sprintf(setting_name, MACRO_BINDING_SETTINGS_KEY, (int)i, (int)b);
                settings_delete(setting_name);
                cfg->bindings[b] = stock->bindings[b];
                changed = true;
            }
        }

        if (changed) {
            /* 復元後のステップ構成でrelease状態を作り直す(settings_commitと同じ理由) */
            macro_recalculate_state(cfg, entry->state);
        }
    }

    return 0;
}

#endif // IS_ENABLED(CONFIG_ZMK_STUDIO_MACROS) && IS_ENABLED(CONFIG_SETTINGS)

int zmk_macros_set_timing(size_t index, uint32_t wait_ms, uint32_t tap_ms) {
    if (index >= ARRAY_SIZE(macro_registry)) {
        return -EINVAL;
    }

    macro_registry[index].cfg->default_wait_ms = wait_ms;
    macro_registry[index].cfg->default_tap_ms = tap_ms;

#if IS_ENABLED(CONFIG_ZMK_STUDIO_MACROS) && IS_ENABLED(CONFIG_SETTINGS)
    // デバウンス保存(2026-07-22): 最終編集の2秒後にまとめて1回フラッシュへ。
    // 保存エラーはRPC応答では返せなくなった(work側でログ)。combo.cと同じ割り切り。
    macro_schedule_save_timing(index);
#endif

    return 0;
}

int zmk_macros_set_binding(size_t index, size_t binding_index,
                           struct zmk_behavior_binding binding) {
    if (index >= ARRAY_SIZE(macro_registry)) {
        return -EINVAL;
    }

    struct macro_registry_entry *entry = &macro_registry[index];
    if (binding_index >= entry->cfg->count) {
        return -ERANGE;
    }

    entry->cfg->bindings[binding_index] = binding;

    // The precalculated release state depends on the step list (position of
    // &macro_pause_for_release, leading control-mode/timing steps), so recompute it now that
    // the steps changed.
    macro_recalculate_state(entry->cfg, entry->state);

#if IS_ENABLED(CONFIG_ZMK_STUDIO_MACROS) && IS_ENABLED(CONFIG_SETTINGS)
    // デバウンス保存(2026-07-22): combo.c参照。復元の数百連射を1回のフラッシュにまとめる。
    int ret = macro_schedule_save_binding(index, binding_index);
    if (ret < 0) {
        LOG_ERR("Failed to persist macro %d step %d to settings (%d)", (int)index,
                (int)binding_index, ret);
        return ret;
    }
#endif

    return 0;
}

#else

/* No macro nodes in the devicetree. CONFIG_ZMK_STUDIO_MACROS depends on
 * CONFIG_ZMK_BEHAVIOR_MACRO (which itself requires DT nodes), so the Studio subsystem is
 * normally not built in this case; these stubs just keep this file self-consistent if that ever
 * changes, mirroring app/src/combo.c. */

size_t zmk_macros_get_count(void) { return 0; }

const char *zmk_macros_get_name(size_t index) { return NULL; }

int32_t zmk_macros_get_wait_ms(size_t index) { return -EINVAL; }

int32_t zmk_macros_get_tap_ms(size_t index) { return -EINVAL; }

const struct zmk_behavior_binding *zmk_macros_get_bindings(size_t index, size_t *len_out) {
    *len_out = 0;
    return NULL;
}

int zmk_macros_set_timing(size_t index, uint32_t wait_ms, uint32_t tap_ms) { return -EINVAL; }

int zmk_macros_set_binding(size_t index, size_t binding_index,
                           struct zmk_behavior_binding binding) {
    return -EINVAL;
}

#endif // ZMK_MACROS_HAS_ANY_INST
