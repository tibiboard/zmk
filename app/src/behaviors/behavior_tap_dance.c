/*
 * Copyright (c) 2022 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_tap_dance

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <zmk/tap_dances.h>

#if IS_ENABLED(CONFIG_ZMK_STUDIO_TAP_DANCES) && IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#include <stdio.h>
#include <stdlib.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ちびボード v2.2.1 (2026-09-05): タップダンスが「何回タップで確定したか」の合図。
 * シールド側(vibe_handler.c)が実体を持ち🔔表(kind6)を引く。実体が無いビルドではweak(何もしない)。
 * combo.c の tibiboard_combo_fired と同じ作法。 */
__attribute__((weak)) void tibiboard_tap_dance_decided(const struct device *dev, int count) {
    ARG_UNUSED(dev);
    ARG_UNUSED(count);
}

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define ZMK_BHV_TAP_DANCE_MAX_HELD CONFIG_ZMK_BEHAVIOR_TAP_DANCE_MAX_HELD

#define ZMK_BHV_TAP_DANCE_POSITION_FREE UINT32_MAX

struct behavior_tap_dance_config {
    uint32_t tapping_term_ms;
    size_t behavior_count;
    struct zmk_behavior_binding *behaviors;
};

struct active_tap_dance {
    // Tap Dance Data
    int counter;
    uint32_t position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    uint8_t source;
#endif
    uint32_t param1;
    uint32_t param2;
    bool is_pressed;
    const struct behavior_tap_dance_config *config;
    const struct device *dev; /* ちびボード v2.2.1: 確定の合図でどのタップダンスかを伝える用 */

    // Timer Data
    bool timer_started;
    bool timer_cancelled;
    bool tap_dance_decided;
    int64_t release_at;
    struct k_work_delayable release_timer;
};

struct active_tap_dance active_tap_dances[ZMK_BHV_TAP_DANCE_MAX_HELD] = {};

static struct active_tap_dance *find_tap_dance(uint32_t position) {
    for (int i = 0; i < ZMK_BHV_TAP_DANCE_MAX_HELD; i++) {
        if (active_tap_dances[i].position == position && !active_tap_dances[i].timer_cancelled) {
            return &active_tap_dances[i];
        }
    }
    return NULL;
}

static int new_tap_dance(struct zmk_behavior_binding_event *event,
                         const struct behavior_tap_dance_config *config,
                         struct active_tap_dance **tap_dance) {
    for (int i = 0; i < ZMK_BHV_TAP_DANCE_MAX_HELD; i++) {
        struct active_tap_dance *const ref_dance = &active_tap_dances[i];
        if (ref_dance->position == ZMK_BHV_TAP_DANCE_POSITION_FREE) {
            ref_dance->counter = 0;
            ref_dance->position = event->position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
            ref_dance->source = event->source;
#endif
            ref_dance->config = config;
            ref_dance->release_at = 0;
            ref_dance->is_pressed = true;
            ref_dance->timer_started = true;
            ref_dance->timer_cancelled = false;
            ref_dance->tap_dance_decided = false;
            *tap_dance = ref_dance;
            return 0;
        }
    }
    return -ENOMEM;
}

static void clear_tap_dance(struct active_tap_dance *tap_dance) {
    tap_dance->position = ZMK_BHV_TAP_DANCE_POSITION_FREE;
}

static int stop_timer(struct active_tap_dance *tap_dance) {
    int timer_cancel_result = k_work_cancel_delayable(&tap_dance->release_timer);
    if (timer_cancel_result == -EINPROGRESS) {
        // too late to cancel, we'll let the timer handler clear up.
        tap_dance->timer_cancelled = true;
    }
    return timer_cancel_result;
}

static void reset_timer(struct active_tap_dance *tap_dance,
                        struct zmk_behavior_binding_event event) {
    tap_dance->release_at = event.timestamp + tap_dance->config->tapping_term_ms;
    int32_t ms_left = tap_dance->release_at - k_uptime_get();
    if (ms_left > 0) {
        k_work_schedule(&tap_dance->release_timer, K_MSEC(ms_left));
        LOG_DBG("Successfully reset timer at position %d", tap_dance->position);
    }
}

static inline int press_tap_dance_behavior(struct active_tap_dance *tap_dance, int64_t timestamp) {
    tap_dance->tap_dance_decided = true;
    tibiboard_tap_dance_decided(tap_dance->dev, tap_dance->counter); /* ちびボード v2.2.1 */
    struct zmk_behavior_binding binding = tap_dance->config->behaviors[tap_dance->counter - 1];
    struct zmk_behavior_binding_event event = {
        .position = tap_dance->position,
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = tap_dance->source,
#endif
    };
    return zmk_behavior_invoke_binding(&binding, event, true);
}

static inline int release_tap_dance_behavior(struct active_tap_dance *tap_dance,
                                             int64_t timestamp) {
    struct zmk_behavior_binding binding = tap_dance->config->behaviors[tap_dance->counter - 1];
    struct zmk_behavior_binding_event event = {
        .position = tap_dance->position,
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = tap_dance->source,
#endif
    };
    clear_tap_dance(tap_dance);
    return zmk_behavior_invoke_binding(&binding, event, false);
}

static int on_tap_dance_binding_pressed(struct zmk_behavior_binding *binding,
                                        struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_tap_dance_config *cfg = dev->config;
    struct active_tap_dance *tap_dance;
    tap_dance = find_tap_dance(event.position);
    if (tap_dance == NULL) {
        if (new_tap_dance(&event, cfg, &tap_dance) == -ENOMEM) {
            LOG_ERR("Unable to create new tap dance. Insufficient space in active_tap_dances[].");
            return ZMK_BEHAVIOR_OPAQUE;
        }
        LOG_DBG("%d created new tap dance", event.position);
    }
    tap_dance->dev = dev; /* ちびボード v2.2.1 */
    tap_dance->is_pressed = true;
    LOG_DBG("%d tap dance pressed", event.position);
    stop_timer(tap_dance);
    // Increment the counter on keypress. If the counter has reached its maximum
    // value, invoke the last binding available.
    if (tap_dance->counter < cfg->behavior_count) {
        tap_dance->counter++;
    }
    if (tap_dance->counter == cfg->behavior_count) {
        // LOG_DBG("Tap dance has been decided via maximum counter value");
        press_tap_dance_behavior(tap_dance, event.timestamp);
        return ZMK_EV_EVENT_BUBBLE;
    }
    reset_timer(tap_dance, event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_tap_dance_binding_released(struct zmk_behavior_binding *binding,
                                         struct zmk_behavior_binding_event event) {
    LOG_DBG("%d tap dance keybind released", event.position);
    struct active_tap_dance *tap_dance = find_tap_dance(event.position);
    if (tap_dance == NULL) {
        LOG_ERR("ACTIVE TAP DANCE CLEARED TOO EARLY");
        return ZMK_BEHAVIOR_OPAQUE;
    }
    tap_dance->is_pressed = false;
    if (tap_dance->tap_dance_decided) {
        release_tap_dance_behavior(tap_dance, event.timestamp);
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

void behavior_tap_dance_timer_handler(struct k_work *item) {
    struct k_work_delayable *d_work = k_work_delayable_from_work(item);
    struct active_tap_dance *tap_dance =
        CONTAINER_OF(d_work, struct active_tap_dance, release_timer);
    if (tap_dance->position == ZMK_BHV_TAP_DANCE_POSITION_FREE) {
        return;
    }
    if (tap_dance->timer_cancelled) {
        return;
    }
    LOG_DBG("Tap dance has been decided via timer. Counter reached: %d", tap_dance->counter);
    press_tap_dance_behavior(tap_dance, tap_dance->release_at);
    if (tap_dance->is_pressed) {
        return;
    }
    release_tap_dance_behavior(tap_dance, tap_dance->release_at);
}

static const struct behavior_driver_api behavior_tap_dance_driver_api = {
    .binding_pressed = on_tap_dance_binding_pressed,
    .binding_released = on_tap_dance_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

static int tap_dance_position_state_changed_listener(const zmk_event_t *eh);

ZMK_LISTENER(behavior_tap_dance, tap_dance_position_state_changed_listener);
ZMK_SUBSCRIPTION(behavior_tap_dance, zmk_position_state_changed);

static int tap_dance_position_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (!ev->state) {
        LOG_DBG("Ignore upstroke at position %d.", ev->position);
        return ZMK_EV_EVENT_BUBBLE;
    }
    for (int i = 0; i < ZMK_BHV_TAP_DANCE_MAX_HELD; i++) {
        struct active_tap_dance *tap_dance = &active_tap_dances[i];
        if (tap_dance->position == ZMK_BHV_TAP_DANCE_POSITION_FREE) {
            continue;
        }
        if (tap_dance->position == ev->position) {
            continue;
        }
        stop_timer(tap_dance);
        LOG_DBG("Tap dance interrupted, activating tap-dance at %d", tap_dance->position);
        if (!tap_dance->tap_dance_decided) {
            press_tap_dance_behavior(tap_dance, ev->timestamp);
            if (!tap_dance->is_pressed) {
                release_tap_dance_behavior(tap_dance, ev->timestamp);
            }
            return ZMK_EV_EVENT_BUBBLE;
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int behavior_tap_dance_init(const struct device *dev) {
    static bool init_first_run = true;
    if (init_first_run) {
        for (int i = 0; i < ZMK_BHV_TAP_DANCE_MAX_HELD; i++) {
            k_work_init_delayable(&active_tap_dances[i].release_timer,
                                  behavior_tap_dance_timer_handler);
            clear_tap_dance(&active_tap_dances[i]);
        }
    }
    init_first_run = false;
    return 0;
}

#define _TRANSFORM_ENTRY(idx, node) ZMK_KEYMAP_EXTRACT_BINDING(idx, node)

#define TRANSFORMED_BINDINGS(node)                                                                 \
    {LISTIFY(DT_INST_PROP_LEN(node, bindings), _TRANSFORM_ENTRY, (, ), DT_DRV_INST(node))}

#define KP_INST(n)                                                                                 \
    static struct zmk_behavior_binding                                                             \
        behavior_tap_dance_config_##n##_bindings[DT_INST_PROP_LEN(n, bindings)] =                  \
            TRANSFORMED_BINDINGS(n);                                                               \
    /* Immutable devicetree-derived copy (flash), kept so "Restore Stock Settings" can put the    \
     * live (mutable, Studio-editable) config back without a reboot - mirrors how combo.c keeps   \
     * stock_combos[] next to its combos[] working copy. */                                       \
    static const struct zmk_behavior_binding                                                       \
        behavior_tap_dance_config_##n##_stock_bindings[DT_INST_PROP_LEN(n, bindings)] =           \
            TRANSFORMED_BINDINGS(n);                                                               \
    static struct behavior_tap_dance_config behavior_tap_dance_config_##n = {                      \
        .tapping_term_ms = DT_INST_PROP(n, tapping_term_ms),                                       \
        .behaviors = behavior_tap_dance_config_##n##_bindings,                                     \
        .behavior_count = DT_INST_PROP_LEN(n, bindings)};                                          \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_tap_dance_init, NULL, NULL,                                \
                            &behavior_tap_dance_config_##n, POST_KERNEL,                           \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_tap_dance_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KP_INST)

/* --- Accessors for the Studio "tapdances" RPC subsystem (see zmk/tap_dances.h) --- */

/* The per-instance config structs and their bindings arrays defined by KP_INST above are
 * mutable RAM (non-const statics) that the runtime reads through dev->config on every press, so
 * the setters below can write to them directly and the change takes effect immediately, exactly
 * like the `combos[]` working copy in app/src/combo.c. */

struct tap_dance_registry_entry {
    struct behavior_tap_dance_config *cfg;
    /* Stock (devicetree) values for "Restore Stock Settings" */
    const struct zmk_behavior_binding *stock_bindings;
    uint32_t stock_tapping_term_ms;
    const char *dev_name;
};

#define TAP_DANCE_REGISTRY_ENTRY(n)                                                                \
    {.cfg = &behavior_tap_dance_config_##n,                                                        \
     .stock_bindings = behavior_tap_dance_config_##n##_stock_bindings,                             \
     .stock_tapping_term_ms = DT_INST_PROP(n, tapping_term_ms),                                    \
     .dev_name = DEVICE_DT_NAME(DT_DRV_INST(n))},

static struct tap_dance_registry_entry tap_dance_registry[] = {
    DT_INST_FOREACH_STATUS_OKAY(TAP_DANCE_REGISTRY_ENTRY)};

size_t zmk_tap_dances_get_count(void) { return ARRAY_SIZE(tap_dance_registry); }

const char *zmk_tap_dances_get_name(size_t index) {
    if (index >= ARRAY_SIZE(tap_dance_registry)) {
        return NULL;
    }

    return tap_dance_registry[index].dev_name;
}

int32_t zmk_tap_dances_get_tapping_term_ms(size_t index) {
    if (index >= ARRAY_SIZE(tap_dance_registry)) {
        return -EINVAL;
    }

    return tap_dance_registry[index].cfg->tapping_term_ms;
}

const struct zmk_behavior_binding *zmk_tap_dances_get_bindings(size_t index, size_t *len_out) {
    if (index >= ARRAY_SIZE(tap_dance_registry)) {
        *len_out = 0;
        return NULL;
    }

    *len_out = tap_dance_registry[index].cfg->behavior_count;
    return tap_dance_registry[index].cfg->behaviors;
}

#if IS_ENABLED(CONFIG_ZMK_STUDIO_TAP_DANCES) && IS_ENABLED(CONFIG_SETTINGS)

/* --- Settings-based persistence, mirroring app/src/keymap.c ---
 *
 * "td/t/<index>" stores a tap-dance's tapping_term_ms (uint32), and "td/b/<index>/<binding_index>"
 * stores one per-tap-count binding. Entries are keyed by *index into the compiled-in registry*, so
 * saved settings only apply cleanly to the same compiled .keymap structure they were saved against
 * (same tap-dance count/order/steps) - the same accepted tradeoff the Keymap tab already has.
 */

#define TAP_DANCE_TERM_SETTINGS_KEY "td/t/%d"
#define TAP_DANCE_BINDING_SETTINGS_KEY "td/b/%d/%d"

struct tap_dance_binding_setting {
    zmk_behavior_local_id_t behavior_local_id;
    uint32_t param1;
    uint32_t param2;
} __packed;

static int tap_dance_save_tapping_term(size_t index) {
    uint32_t term = tap_dance_registry[index].cfg->tapping_term_ms;

    char setting_name[16];
    sprintf(setting_name, TAP_DANCE_TERM_SETTINGS_KEY, (int)index);

    return settings_save_one(setting_name, &term, sizeof(term));
}

static int tap_dance_save_binding(size_t index, size_t binding_index) {
    const struct zmk_behavior_binding *binding =
        &tap_dance_registry[index].cfg->behaviors[binding_index];

    struct tap_dance_binding_setting setting = {
        .behavior_local_id = zmk_behavior_get_local_id(binding->behavior_dev),
        .param1 = binding->param1,
        .param2 = binding->param2,
    };

    char setting_name[20];
    sprintf(setting_name, TAP_DANCE_BINDING_SETTINGS_KEY, (int)index, (int)binding_index);

    return settings_save_one(setting_name, &setting, sizeof(setting));
}

/* --- Debounced flush (2026-07-22, combo.c/behavior_macro.cと同じ設計) ---
 * アプリの復元はタップダンスの項目も連射で書いてくる。編集はRAM即時反映のまま、
 * フラッシュ書き込みだけを最終編集の2秒後に1回へまとめる。タップ段数は少数(≦32前提)。 */

#define TAP_DANCE_SAVE_DEBOUNCE_MS 2000

static uint8_t td_term_dirty[DIV_ROUND_UP(ARRAY_SIZE(tap_dance_registry), 8)];
static uint32_t td_binding_dirty[ARRAY_SIZE(tap_dance_registry)];

/* まとめ書き完了の合図。combo.c参照(weak、実装はvibe_handler.c) */
__attribute__((weak)) void tibiboard_on_settings_flushed(void) {}

static void tap_dance_save_work_handler(struct k_work *work) {
    int wrote = 0;
    for (size_t i = 0; i < ARRAY_SIZE(tap_dance_registry); i++) {
        if (td_term_dirty[i / 8] & BIT(i % 8)) {
            WRITE_BIT(td_term_dirty[i / 8], i % 8, 0);
            int ret = tap_dance_save_tapping_term(i);
            if (ret < 0) {
                LOG_ERR("Failed to persist tap dance %d term (%d)", (int)i, ret);
            } else {
                wrote++;
            }
        }
        while (td_binding_dirty[i]) {
            uint32_t b = __builtin_ctz(td_binding_dirty[i]);
            WRITE_BIT(td_binding_dirty[i], b, 0);
            int ret = tap_dance_save_binding(i, b);
            if (ret < 0) {
                LOG_ERR("Failed to persist tap dance %d binding %d (%d)", (int)i, (int)b, ret);
            } else {
                wrote++;
            }
        }
    }
    if (wrote > 0) {
        tibiboard_on_settings_flushed();
    }
}

static K_WORK_DELAYABLE_DEFINE(tap_dance_save_work, tap_dance_save_work_handler);

static void td_schedule_save_term(size_t index) {
    WRITE_BIT(td_term_dirty[index / 8], index % 8, 1);
    k_work_reschedule(&tap_dance_save_work, K_MSEC(TAP_DANCE_SAVE_DEBOUNCE_MS));
}

static int td_schedule_save_binding(size_t index, size_t binding_index) {
    if (binding_index >= 32) {
        return tap_dance_save_binding(index, binding_index); /* 32段超の保険: 即時保存 */
    }
    WRITE_BIT(td_binding_dirty[index], binding_index, 1);
    k_work_reschedule(&tap_dance_save_work, K_MSEC(TAP_DANCE_SAVE_DEBOUNCE_MS));
    return 0;
}

// NOTE: like every settings h_set callback in this firmware, this must return 0 (skip with a
// warning) for malformed/legacy/out-of-range entries rather than an error: with the NVS backend a
// nonzero return aborts the remainder of the ENTIRE settings load (all subsystems, newest-first
// iteration), silently reverting every not-yet-loaded setting to compiled-in defaults for that
// boot. See behavior_handle_set() in app/src/behavior.c.
static int tap_dance_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                  void *cb_arg) {
    const char *next;

    if (settings_name_steq(name, "t", &next) && next) {
        char *endptr;
        unsigned long index = strtoul(next, &endptr, 10);
        if (*endptr != '\0') {
            LOG_WRN("Skipping invalid tap dance index: %s", next);
            return 0;
        }

        if (index >= ARRAY_SIZE(tap_dance_registry)) {
            LOG_WRN("Ignoring saved tapping term for tap dance %lu (only %d in this build)", index,
                    (int)ARRAY_SIZE(tap_dance_registry));
            return 0;
        }

        uint32_t term;
        if (len != sizeof(term)) {
            LOG_WRN("Skipping invalid tapping term setting size %d", (int)len);
            return 0;
        }

        int ret = read_cb(cb_arg, &term, sizeof(term));
        if (ret <= 0) {
            LOG_ERR("Failed to read tapping term setting (err %d), skipping", ret);
            return 0;
        }

        tap_dance_registry[index].cfg->tapping_term_ms = term;
    } else if (settings_name_steq(name, "b", &next) && next) {
        char *endptr;
        unsigned long index = strtoul(next, &endptr, 10);
        if (*endptr != '/') {
            LOG_WRN("Skipping invalid tap dance index: %s", next);
            return 0;
        }

        unsigned long binding_index = strtoul(endptr + 1, &endptr, 10);
        if (*endptr != '\0') {
            LOG_WRN("Skipping invalid tap dance binding index: %s", next);
            return 0;
        }

        if (index >= ARRAY_SIZE(tap_dance_registry) ||
            binding_index >= tap_dance_registry[index].cfg->behavior_count) {
            LOG_WRN("Ignoring saved binding %lu for tap dance %lu (out of range in this build)",
                    binding_index, index);
            return 0;
        }

        struct tap_dance_binding_setting setting = {0};
        if (len > sizeof(setting)) {
            LOG_WRN("Skipping invalid tap dance binding setting size %d", (int)len);
            return 0;
        }

        int ret = read_cb(cb_arg, &setting, len);
        if (ret <= 0) {
            LOG_ERR("Failed to read tap dance binding setting (err %d), skipping", ret);
            return 0;
        }

        // May be NULL here if the "behavior" local-id settings table hasn't been loaded yet;
        // tap_dance_settings_commit() re-resolves from local_id afterwards, same as keymap.c.
        const char *behavior_name =
            zmk_behavior_find_behavior_name_from_local_id(setting.behavior_local_id);

        tap_dance_registry[index].cfg->behaviors[binding_index] = (struct zmk_behavior_binding){
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

static int tap_dance_settings_commit(void) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
    for (size_t i = 0; i < ARRAY_SIZE(tap_dance_registry); i++) {
        struct behavior_tap_dance_config *cfg = tap_dance_registry[i].cfg;

        for (size_t b = 0; b < cfg->behavior_count; b++) {
            struct zmk_behavior_binding *binding = &cfg->behaviors[b];

            if (binding->local_id > 0 && !binding->behavior_dev) {
                binding->behavior_dev =
                    zmk_behavior_find_behavior_name_from_local_id(binding->local_id);

                if (!binding->behavior_dev) {
                    LOG_ERR("Failed to find behavior for local ID %d for tap dance %d binding %d "
                            "after settings load",
                            binding->local_id, (int)i, (int)b);
                }
            }
        }
    }
#endif

    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(tap_dance, "td", NULL, tap_dance_settings_set,
                               tap_dance_settings_commit, NULL);

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
 * edit implies a stored key; deleting blindly would burn one NVS write per key for hundreds of
 * untouched entries), then put the live config back to the devicetree values in place - no
 * reboot needed, mirroring zmk_keymap_reset_settings(). */
int zmk_tap_dances_reset_settings(void) {
    /* 未フラッシュの編集とその予約を破棄(直後にstock値で上書きされるため。combo.cと同じ) */
    /* _syncで実行中のまとめ書きの完了も待つ(combo.c参照) */
    struct k_work_sync sync;
    k_work_cancel_delayable_sync(&tap_dance_save_work, &sync);
    memset(td_term_dirty, 0, sizeof(td_term_dirty));
    memset(td_binding_dirty, 0, sizeof(td_binding_dirty));

    for (size_t i = 0; i < ARRAY_SIZE(tap_dance_registry); i++) {
        struct tap_dance_registry_entry *entry = &tap_dance_registry[i];
        struct behavior_tap_dance_config *cfg = entry->cfg;
        char setting_name[24];

        if (cfg->tapping_term_ms != entry->stock_tapping_term_ms) {
            sprintf(setting_name, TAP_DANCE_TERM_SETTINGS_KEY, (int)i);
            settings_delete(setting_name);
            cfg->tapping_term_ms = entry->stock_tapping_term_ms;
        }

        for (size_t b = 0; b < cfg->behavior_count; b++) {
            if (!binding_equals_stock(&cfg->behaviors[b], &entry->stock_bindings[b])) {
                sprintf(setting_name, TAP_DANCE_BINDING_SETTINGS_KEY, (int)i, (int)b);
                settings_delete(setting_name);
                cfg->behaviors[b] = entry->stock_bindings[b];
            }
        }
    }

    return 0;
}

#endif // IS_ENABLED(CONFIG_ZMK_STUDIO_TAP_DANCES) && IS_ENABLED(CONFIG_SETTINGS)

int zmk_tap_dances_set_tapping_term_ms(size_t index, uint32_t tapping_term_ms) {
    if (index >= ARRAY_SIZE(tap_dance_registry)) {
        return -EINVAL;
    }

    tap_dance_registry[index].cfg->tapping_term_ms = tapping_term_ms;

#if IS_ENABLED(CONFIG_ZMK_STUDIO_TAP_DANCES) && IS_ENABLED(CONFIG_SETTINGS)
    // デバウンス保存(2026-07-22): combo.c参照。保存エラーはwork側でログ。
    td_schedule_save_term(index);
#endif

    return 0;
}

int zmk_tap_dances_set_binding(size_t index, size_t binding_index,
                               struct zmk_behavior_binding binding) {
    if (index >= ARRAY_SIZE(tap_dance_registry)) {
        return -EINVAL;
    }

    struct behavior_tap_dance_config *cfg = tap_dance_registry[index].cfg;
    if (binding_index >= cfg->behavior_count) {
        return -ERANGE;
    }

    cfg->behaviors[binding_index] = binding;

#if IS_ENABLED(CONFIG_ZMK_STUDIO_TAP_DANCES) && IS_ENABLED(CONFIG_SETTINGS)
    // デバウンス保存(2026-07-22): combo.c参照。
    int ret = td_schedule_save_binding(index, binding_index);
    if (ret < 0) {
        LOG_ERR("Failed to persist tap dance %d binding %d to settings (%d)", (int)index,
                (int)binding_index, ret);
        return ret;
    }
#endif

    return 0;
}

#else

/* No `zmk,behavior-tap-dance` nodes in the devicetree. CONFIG_ZMK_STUDIO_TAP_DANCES depends on
 * CONFIG_ZMK_BEHAVIOR_TAP_DANCE (which itself requires DT nodes), so the Studio subsystem is
 * normally not built in this case; these stubs just keep this file self-consistent if that ever
 * changes, mirroring app/src/combo.c. */

size_t zmk_tap_dances_get_count(void) { return 0; }

const char *zmk_tap_dances_get_name(size_t index) { return NULL; }

int32_t zmk_tap_dances_get_tapping_term_ms(size_t index) { return -EINVAL; }

const struct zmk_behavior_binding *zmk_tap_dances_get_bindings(size_t index, size_t *len_out) {
    *len_out = 0;
    return NULL;
}

int zmk_tap_dances_set_tapping_term_ms(size_t index, uint32_t tapping_term_ms) { return -EINVAL; }

int zmk_tap_dances_set_binding(size_t index, size_t binding_index,
                               struct zmk_behavior_binding binding) {
    return -EINVAL;
}

#endif
