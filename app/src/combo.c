/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_combos

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/dlist.h>
#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>
#include <string.h>

#if IS_ENABLED(CONFIG_ZMK_STUDIO_COMBOS) && IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#include <stdio.h>
#include <stdlib.h>
#endif

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <zmk/matrix.h>
#include <zmk/keymap.h>
#include <zmk/virtual_key_position.h>
#include <zmk/combos.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* tibiboard30カスタム: ヒートマップ用コンボ発動カウンタ用フック(2026-07-27)。
 * シールド側(heatmap_counter.c)が実体を持つ。実体が無いビルドではweak(何もしない)。 */
__attribute__((weak)) void tibiboard_combo_fired(int combo_idx) {}

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO > 0

#warning                                                                                           \
    "CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO is deprecated, and is auto-calculated from the devicetree now."

#endif

#if CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY > 0

#warning "CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY is deprecated, and is auto-calculated."

#endif

#define COMBOS_KEYS_BYTE_ARRAY(node_id)                                                            \
    uint8_t _CONCAT(combo_prop_, node_id)[DT_PROP_LEN(node_id, key_positions)];

#define MAX_COMBO_KEYS sizeof(union {DT_INST_FOREACH_CHILD(0, COMBOS_KEYS_BYTE_ARRAY)})

struct combo_cfg {
    int32_t key_positions[MAX_COMBO_KEYS];
    int16_t key_position_len;
    int16_t require_prior_idle_ms;
    int32_t timeout_ms;
    uint32_t layer_mask;
    struct zmk_behavior_binding behavior;
    // if slow release is set, the combo releases when the last key is released.
    // otherwise, the combo releases when the first key is released.
    bool slow_release;
};

struct active_combo {
    uint16_t combo_idx;
    // key_positions_pressed is filled with key_positions when the combo is pressed.
    // The keys are removed from this array when they are released.
    // Once this array is empty, the behavior is released.
    uint16_t key_positions_pressed_count;
    struct zmk_position_state_changed_event key_positions_pressed[MAX_COMBO_KEYS];
};

#define PROP_BIT_AT_IDX(n, prop, idx) BIT(DT_PROP_BY_IDX(n, prop, idx))

#define NODE_PROP_BITMASK(n, prop)                                                                 \
    COND_CODE_1(DT_NODE_HAS_PROP(n, prop),                                                         \
                (DT_FOREACH_PROP_ELEM_SEP(n, prop, PROP_BIT_AT_IDX, (|))), (0))

#define GET_KEY_POSITION_MASK_PORTION(idx, n) ((NODE_PROP_BITMASK(n, key_positions) >> idx) & 0xFF)

#define COMBO_INST(n, positions)                                                                   \
    COND_CODE_1(IS_EQ(DT_PROP_LEN(n, key_positions), positions),                                   \
                (                                                                                  \
                    {                                                                              \
                        .timeout_ms = DT_PROP(n, timeout_ms),                                      \
                        .require_prior_idle_ms = DT_PROP(n, require_prior_idle_ms),                \
                        .key_positions = DT_PROP(n, key_positions),                                \
                        .key_position_len = DT_PROP_LEN(n, key_positions),                         \
                        .behavior = ZMK_KEYMAP_EXTRACT_BINDING(0, n),                              \
                        .slow_release = DT_PROP(n, slow_release),                                  \
                        .layer_mask = NODE_PROP_BITMASK(n, layers),                                \
                    }, ),                                                                          \
                ())

#define COMBO_CONFIGS_WITH_MATCHING_POSITIONS_LEN(positions, _ignore)                              \
    DT_INST_FOREACH_CHILD_VARGS(0, COMBO_INST, positions)

// We do some magic here to generate the `stock_combos` array by "key position length", looping
// by key position length and on each iteration, only include entries where the `key-positions`
// length matches.
// Doing so allows our bitmasks to be "shorted key positions list first" when searching for matches.
// `20` is chosen as a reasonable limit, since the theoretical maximum number of keys you might
// reasonably press simultaneously with 10 fingers is 20 keys, two keys per finger.
//
// `stock_combos` is the immutable devicetree-derived combo set. `combos` is a RAM working copy
// (initialized from `stock_combos` in combo_init()) that the Studio "combos" RPC subsystem
// (app/src/studio/combo_subsystem.c) can mutate at runtime via zmk_combos_set(), so live-editing a
// combo's timeout/behavior takes effect immediately without needing a reflash. layer_mask is not
// yet editable. Edits made through zmk_combos_set() are also persisted to the settings subsystem
// (see combo_save_setting()/combo_settings_set() below) so they survive a power cycle, mirroring
// the keymap.c pattern.
static const struct combo_cfg stock_combos[] = {
    LISTIFY(20, COMBO_CONFIGS_WITH_MATCHING_POSITIONS_LEN, (), 0)};

static struct combo_cfg combos[ARRAY_SIZE(stock_combos)];

#define COMBO_ONE(n) +1

#define COMBO_CHILDREN_COUNT (0 DT_INST_FOREACH_CHILD(0, COMBO_ONE))

// We need at least 4 bytes to avoid alignment issues
#define BYTES_FOR_COMBOS_MASK DIV_ROUND_UP(COMBO_CHILDREN_COUNT, 32)

uint8_t pressed_keys_count = 0;
// set of keys pressed
struct zmk_position_state_changed_event pressed_keys[MAX_COMBO_KEYS] = {};
// the set of candidate combos based on the currently pressed_keys
uint32_t candidates[BYTES_FOR_COMBOS_MASK];
// the last candidate that was completely pressed
int16_t fully_pressed_combo = INT16_MAX;
// a lookup dict that maps a key position to all combos on that position
uint32_t combo_lookup[ZMK_KEYMAP_LEN][BYTES_FOR_COMBOS_MASK] = {};
// combos that have been activated and still have (some) keys pressed
// this array is always contiguous from 0.
struct active_combo active_combos[CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS] = {};
uint8_t active_combo_count = 0;

struct k_work_delayable timeout_task;
int64_t timeout_task_timeout_at;

// this keeps track of the last non-combo, non-mod key tap
int64_t last_tapped_timestamp = INT32_MIN;
// this keeps track of the last time a combo was pressed
int64_t last_combo_timestamp = INT32_MIN;

static void store_last_tapped(int64_t timestamp) {
    if (timestamp > last_combo_timestamp) {
        last_tapped_timestamp = timestamp;
    }
}

// Store the combo key pointer in the combos array, one pointer for each key position
// The combos are sorted shortest-first, then by virtual-key-position.
static int initialize_combo(size_t index) {
    const struct combo_cfg *new_combo = &combos[index];

    for (size_t kp = 0; kp < new_combo->key_position_len; kp++) {
        sys_bitfield_set_bit((mem_addr_t)&combo_lookup[new_combo->key_positions[kp]], index);
    }

    return 0;
}

static bool combo_active_on_layer(const struct combo_cfg *combo, uint8_t layer) {
    if (!combo->layer_mask) {
        return true;
    }

    return combo->layer_mask & BIT(layer);
}

static bool is_quick_tap(const struct combo_cfg *combo, int64_t timestamp) {
    return (last_tapped_timestamp + combo->require_prior_idle_ms) > timestamp;
}

// An unassigned spare slot (binding &none) must never fire: otherwise pressing its keys
// together would swallow ordinary keystrokes on a keyboard whose owner never set a combo.
static bool combo_is_empty_slot(const struct combo_cfg *combo) {
    return combo->behavior.behavior_dev == NULL ||
           strcmp(combo->behavior.behavior_dev, "none") == 0;
}

static int setup_candidates_for_first_keypress(int32_t position, int64_t timestamp) {
    int number_of_combo_candidates = 0;
    uint8_t highest_active_layer = zmk_keymap_highest_layer_active();

    for (size_t i = 0; i < ARRAY_SIZE(combos); i++) {
        if (sys_bitfield_test_bit((mem_addr_t)&combo_lookup[position], i)) {
            const struct combo_cfg *combo = &combos[i];
            if (combo_active_on_layer(combo, highest_active_layer) &&
                !is_quick_tap(combo, timestamp) && !combo_is_empty_slot(combo)) {
                sys_bitfield_set_bit((mem_addr_t)&candidates, i);
                number_of_combo_candidates++;
            }
            // LOG_DBG("combo timeout %d %d %d", position, i, candidates[i].timeout_at);
        }
    }

    return number_of_combo_candidates;
}

static inline uint8_t zero_one_or_more_bits(uint32_t field) {
    if (field == 0) {
        return 0;
    }
    if ((field & (field - 1)) == 0) {
        return 1;
    }
    return 2;
}

static int filter_candidates(int32_t position) {
    int matches = 0;
    for (int i = 0; i < BYTES_FOR_COMBOS_MASK; i++) {
        candidates[i] &= combo_lookup[position][i];
        if (matches < 2) {
            matches += zero_one_or_more_bits(candidates[i]);
        }
    }

    LOG_DBG("combo matches after filter %d", matches);
    return matches;
}

static int64_t first_candidate_timeout() {
    if (pressed_keys_count == 0) {
        return LONG_MAX;
    }

    int64_t first_timeout = LONG_MAX;
    for (int i = 0; i < ARRAY_SIZE(combos); i++) {
        if (sys_bitfield_test_bit((mem_addr_t)&candidates, i)) {
            first_timeout = MIN(first_timeout, combos[i].timeout_ms);
        }
    }

    return pressed_keys[0].data.timestamp + first_timeout;
}

static inline bool candidate_is_completely_pressed(const struct combo_cfg *candidate) {
    // this code assumes set(pressed_keys) <= set(candidate->key_positions)
    // this invariant is enforced by filter_candidates
    // since events may have been reraised after clearing one or more slots at
    // the start of pressed_keys (see: release_pressed_keys), we have to check
    // that each key needed to trigger the combo was pressed, not just the last.
    return candidate->key_position_len == pressed_keys_count;
}

static int cleanup();

static int filter_timed_out_candidates(int64_t timestamp) {
    __ASSERT(pressed_keys_count > 0, "Searching for a candidate timeout with no keys pressed");

    int remaining_candidates = 0;
    for (int i = 0; i < ARRAY_SIZE(combos); i++) {
        if (sys_bitfield_test_bit((mem_addr_t)&candidates, i)) {

            if (pressed_keys[0].data.timestamp + combos[i].timeout_ms > timestamp) {
                remaining_candidates++;
            } else {
                sys_bitfield_clear_bit((mem_addr_t)&candidates, i);
            }
        }
    }

    LOG_DBG(
        "after filtering out timed out combo candidates: remaining_candidates=%d timestamp=%lld",
        remaining_candidates, timestamp);

    return remaining_candidates;
}

static int capture_pressed_key(const struct zmk_position_state_changed *ev) {
    if (pressed_keys_count == MAX_COMBO_KEYS) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    pressed_keys[pressed_keys_count++] = copy_raised_zmk_position_state_changed(ev);
    return ZMK_EV_EVENT_CAPTURED;
}

const struct zmk_listener zmk_listener_combo;

static int release_pressed_keys() {
    uint8_t count = pressed_keys_count;
    pressed_keys_count = 0;
    for (int i = 0; i < count; i++) {
        struct zmk_position_state_changed_event *ev = &pressed_keys[i];
        if (i == 0) {
            LOG_DBG("combo: releasing position event %d", ev->data.position);
            ZMK_EVENT_RELEASE(*ev);
        } else {
            // reprocess events (see tests/combo/fully-overlapping-combos-3 for why this is needed)
            LOG_DBG("combo: reraising position event %d", ev->data.position);
            ZMK_EVENT_RAISE(*ev);
        }
    }

    return count;
}

static inline int press_combo_behavior(int combo_idx, const struct combo_cfg *combo,
                                       int32_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = ZMK_VIRTUAL_KEY_POSITION_COMBO(combo_idx),
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    last_combo_timestamp = timestamp;

    tibiboard_combo_fired(combo_idx);

    return zmk_behavior_invoke_binding(&combo->behavior, event, true);
}

static inline int release_combo_behavior(int combo_idx, const struct combo_cfg *combo,
                                         int32_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = ZMK_VIRTUAL_KEY_POSITION_COMBO(combo_idx),
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    return zmk_behavior_invoke_binding(&combo->behavior, event, false);
}

static void move_pressed_keys_to_active_combo(struct active_combo *active_combo) {

    int combo_length = MIN(pressed_keys_count, combos[active_combo->combo_idx].key_position_len);
    for (int i = 0; i < combo_length; i++) {
        active_combo->key_positions_pressed[i] = pressed_keys[i];
    }
    active_combo->key_positions_pressed_count = combo_length;

    // move any other pressed keys up
    for (int i = 0; i + combo_length < pressed_keys_count; i++) {
        pressed_keys[i] = pressed_keys[i + combo_length];
    }

    pressed_keys_count -= combo_length;
}

static struct active_combo *store_active_combo(int32_t combo_idx) {
    for (int i = 0; i < CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS; i++) {
        if (active_combos[i].combo_idx == UINT16_MAX) {
            active_combos[i].combo_idx = combo_idx;
            active_combo_count++;
            return &active_combos[i];
        }
    }
    LOG_ERR("Unable to store combo; already %d active. Increase "
            "CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS",
            CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS);
    return NULL;
}

static void activate_combo(int combo_idx) {
    struct active_combo *active_combo = store_active_combo(combo_idx);
    if (active_combo == NULL) {
        // unable to store combo
        release_pressed_keys();
        return;
    }
    move_pressed_keys_to_active_combo(active_combo);
    press_combo_behavior(combo_idx, &combos[combo_idx],
                         active_combo->key_positions_pressed[0].data.timestamp);
}

static void deactivate_combo(int active_combo_index) {
    active_combo_count--;
    if (active_combo_index != active_combo_count) {
        memcpy(&active_combos[active_combo_index], &active_combos[active_combo_count],
               sizeof(struct active_combo));
    }
    active_combos[active_combo_count] = (struct active_combo){0};
    active_combos[active_combo_count].combo_idx = UINT16_MAX;
}

/* returns true if a key was released. */
static bool release_combo_key(int32_t position, int64_t timestamp) {
    for (int combo_idx = 0; combo_idx < active_combo_count; combo_idx++) {
        struct active_combo *active_combo = &active_combos[combo_idx];

        bool key_released = false;
        bool all_keys_pressed = active_combo->key_positions_pressed_count ==
                                combos[active_combo->combo_idx].key_position_len;
        bool all_keys_released = true;
        for (int i = 0; i < active_combo->key_positions_pressed_count; i++) {
            if (key_released) {
                active_combo->key_positions_pressed[i - 1] = active_combo->key_positions_pressed[i];
                all_keys_released = false;
            } else if (active_combo->key_positions_pressed[i].data.position != position) {
                all_keys_released = false;
            } else { // position matches
                key_released = true;
            }
        }

        if (key_released) {
            active_combo->key_positions_pressed_count--;
            const struct combo_cfg *c = &combos[active_combo->combo_idx];
            if ((c->slow_release && all_keys_released) || (!c->slow_release && all_keys_pressed)) {
                release_combo_behavior(active_combo->combo_idx, c, timestamp);
            }
            if (all_keys_released) {
                deactivate_combo(combo_idx);
            }
            return true;
        }
    }
    return false;
}

static int cleanup() {
    k_work_cancel_delayable(&timeout_task);
    memset(candidates, 0, BYTES_FOR_COMBOS_MASK * sizeof(uint32_t));
    if (fully_pressed_combo != INT16_MAX) {
        activate_combo(fully_pressed_combo);
        fully_pressed_combo = INT16_MAX;
    }
    return release_pressed_keys();
}

static void update_timeout_task() {
    int64_t first_timeout = first_candidate_timeout();
    if (timeout_task_timeout_at == first_timeout) {
        return;
    }
    if (first_timeout == LLONG_MAX) {
        timeout_task_timeout_at = 0;
        k_work_cancel_delayable(&timeout_task);
        return;
    }
    if (k_work_schedule(&timeout_task, K_MSEC(first_timeout - k_uptime_get())) >= 0) {
        timeout_task_timeout_at = first_timeout;
    }
}

static int position_state_down(const zmk_event_t *ev, struct zmk_position_state_changed *data) {
    int num_candidates;
    if (!pressed_keys_count) {
        num_candidates = setup_candidates_for_first_keypress(data->position, data->timestamp);
        if (num_candidates == 0) {
            return ZMK_EV_EVENT_BUBBLE;
        }
    } else {
        filter_timed_out_candidates(data->timestamp);
        num_candidates = filter_candidates(data->position);
    }

    LOG_DBG("combo: capturing position event %d", data->position);
    int ret = capture_pressed_key(data);
    update_timeout_task();

    if (num_candidates) {
        for (int i = 0; i < ARRAY_SIZE(combos); i++) {
            if (sys_bitfield_test_bit((mem_addr_t)&candidates, i)) {
                const struct combo_cfg *candidate_combo = &combos[i];
                if (candidate_is_completely_pressed(candidate_combo)) {
                    fully_pressed_combo = i;
                    if (num_candidates == 1) {
                        cleanup();
                    }
                }

                return ret;
            }
        }
    } else {
        cleanup();
        return ret;
    }

    return -EINVAL;
}

static int position_state_up(const zmk_event_t *ev, struct zmk_position_state_changed *data) {
    int released_keys = cleanup();
    if (release_combo_key(data->position, data->timestamp)) {
        return ZMK_EV_EVENT_HANDLED;
    }
    if (released_keys > 1) {
        // The second and further key down events are re-raised. To preserve
        // correct order for e.g. hold-taps, reraise the key up event too.
        struct zmk_position_state_changed_event dupe_ev =
            copy_raised_zmk_position_state_changed(data);
        ZMK_EVENT_RAISE(dupe_ev);
        return ZMK_EV_EVENT_CAPTURED;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static void combo_timeout_handler(struct k_work *item) {
    if (timeout_task_timeout_at == 0 || k_uptime_get() < timeout_task_timeout_at) {
        // timer was cancelled or rescheduled.
        return;
    }
    if (filter_timed_out_candidates(timeout_task_timeout_at) == 0) {
        LOG_DBG("CLEANUP!");
        cleanup();
    }

    LOG_DBG("ABOUT TO UPDATE IN TIMEOUT");
    update_timeout_task();
}

static int position_state_changed_listener(const zmk_event_t *ev) {
    struct zmk_position_state_changed *data = as_zmk_position_state_changed(ev);
    if (data == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (data->state) { // keydown
        return position_state_down(ev, data);
    } else { // keyup
        return position_state_up(ev, data);
    }
}

static int keycode_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev->state && !is_mod(ev->usage_page, ev->keycode)) {
        store_last_tapped(ev->timestamp);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

int behavior_combo_listener(const zmk_event_t *eh) {
    if (as_zmk_position_state_changed(eh) != NULL) {
        return position_state_changed_listener(eh);
    } else if (as_zmk_keycode_state_changed(eh) != NULL) {
        return keycode_state_changed_listener(eh);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(combo, behavior_combo_listener);
ZMK_SUBSCRIPTION(combo, zmk_position_state_changed);
ZMK_SUBSCRIPTION(combo, zmk_keycode_state_changed);

static int combo_init(void) {
    for (size_t i = 0; i < CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS; i++) {
        active_combos[i].combo_idx = UINT16_MAX;
    }

    memcpy(combos, stock_combos, sizeof(combos));

    k_work_init_delayable(&timeout_task, combo_timeout_handler);
    LOG_WRN("Have %d combos!", ARRAY_SIZE(combos));
    for (int i = 0; i < ARRAY_SIZE(combos); i++) {
        initialize_combo(i);
    }
    return 0;
}

SYS_INIT(combo_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

/* --- Read-only accessors for the Studio "combos" RPC subsystem (see zmk/combos.h) --- */

size_t zmk_combos_get_combo_count(void) { return ARRAY_SIZE(combos); }

const int32_t *zmk_combos_get_key_positions(size_t index, size_t *len_out) {
    if (index >= ARRAY_SIZE(combos)) {
        *len_out = 0;
        return NULL;
    }

    *len_out = combos[index].key_position_len;
    return combos[index].key_positions;
}

int32_t zmk_combos_get_timeout_ms(size_t index) {
    if (index >= ARRAY_SIZE(combos)) {
        return -EINVAL;
    }

    return combos[index].timeout_ms;
}

int32_t zmk_combos_get_require_prior_idle_ms(size_t index) {
    if (index >= ARRAY_SIZE(combos)) {
        return -EINVAL;
    }

    // The devicetree binding's default for `require-prior-idle-ms` is -1 ("never set" —
    // the runtime feature check is `> 0`, so -1 and 0 are both "disabled"). Clamp to 0 so
    // the Studio "combos" RPC subsystem, whose proto field is uint32, doesn't render an
    // untouched combo as 4294967295; a 0 written back through zmk_combos_set() keeps
    // identical runtime semantics.
    return MAX(combos[index].require_prior_idle_ms, 0);
}

bool zmk_combos_get_slow_release(size_t index) {
    if (index >= ARRAY_SIZE(combos)) {
        return false;
    }

    return combos[index].slow_release;
}

uint32_t zmk_combos_get_layer_mask(size_t index) {
    if (index >= ARRAY_SIZE(combos)) {
        return 0;
    }

    return combos[index].layer_mask;
}

const struct zmk_behavior_binding *zmk_combos_get_behavior(size_t index) {
    if (index >= ARRAY_SIZE(combos)) {
        return NULL;
    }

    return &combos[index].behavior;
}

static int combo_apply_set(size_t index, int32_t timeout_ms, int32_t require_prior_idle_ms,
                           bool slow_release, struct zmk_behavior_binding behavior,
                           const int32_t *key_positions, size_t key_positions_len) {
    if (index >= ARRAY_SIZE(combos)) {
        return -EINVAL;
    }

    if (key_positions_len > MAX_COMBO_KEYS) {
        return -E2BIG;
    }

    for (size_t i = 0; i < key_positions_len; i++) {
        if (key_positions[i] < 0 || key_positions[i] >= ZMK_KEYMAP_LEN) {
            return -EINVAL;
        }
    }

    // Takes effect immediately: `combos` is the exact array the runtime combo-matching logic
    // above (setup_candidates_for_first_keypress, press_combo_behavior, etc.) reads from, so no
    // reboot/reflash is needed to see the change.
    //
    // Key positions changed, so combo_lookup (the key-position -> combo-index bitmask table used
    // to find candidate combos on each keypress) must be rebuilt for this combo: first clear its
    // bit from every position (initialize_combo only ever SETS bits, so without this, stale bits
    // from the old key_positions would linger and the combo could still incorrectly fire from
    // keys it's no longer configured for), then re-set bits for the new positions.
    for (int kp = 0; kp < ZMK_KEYMAP_LEN; kp++) {
        sys_bitfield_clear_bit((mem_addr_t)&combo_lookup[kp], index);
    }

    combos[index].timeout_ms = timeout_ms;
    combos[index].require_prior_idle_ms = require_prior_idle_ms;
    combos[index].slow_release = slow_release;
    combos[index].behavior = behavior;
    combos[index].key_position_len = (int16_t)key_positions_len;
    memcpy(combos[index].key_positions, key_positions, key_positions_len * sizeof(int32_t));

    initialize_combo(index);

    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_STUDIO_COMBOS) && IS_ENABLED(CONFIG_SETTINGS)

/* --- Settings-based persistence, mirroring app/src/keymap.c ---
 *
 * Each edited combo is saved as one blob under "combos/c/<index>". Like keymap.c's
 * "keymap/l/<layer>/<position>" entries, entries are keyed by *index into the compiled-in array*,
 * so saved settings only apply cleanly to the same compiled .keymap structure they were saved
 * against (same combo count/order). That's the same accepted tradeoff the Keymap tab already has.
 */

#define COMBO_SETTINGS_KEY "combos/c/%d"

struct combo_setting {
    int32_t timeout_ms;
    int16_t require_prior_idle_ms;
    uint8_t slow_release;
    uint8_t key_position_len;
    zmk_behavior_local_id_t behavior_local_id;
    uint32_t param1;
    uint32_t param2;
    int32_t key_positions[MAX_COMBO_KEYS];
} __packed;

static int combo_save_setting(size_t index) {
    const struct combo_cfg *combo = &combos[index];

    struct combo_setting setting = {
        .timeout_ms = combo->timeout_ms,
        .require_prior_idle_ms = combo->require_prior_idle_ms,
        .slow_release = combo->slow_release ? 1 : 0,
        .key_position_len = (uint8_t)combo->key_position_len,
        .behavior_local_id = zmk_behavior_get_local_id(combo->behavior.behavior_dev),
        .param1 = combo->behavior.param1,
        .param2 = combo->behavior.param2,
    };

    for (int i = 0; i < combo->key_position_len; i++) {
        setting.key_positions[i] = combo->key_positions[i];
    }

    // Only store the key position slots actually in use, the same way keymap.c trims
    // trailing zero params from its binding settings.
    size_t len = offsetof(struct combo_setting, key_positions) +
                 (size_t)combo->key_position_len * sizeof(int32_t);

    char setting_name[24];
    sprintf(setting_name, COMBO_SETTINGS_KEY, (int)index);

    return settings_save_one(setting_name, &setting, len);
}

/* --- Debounced flush (2026-07-21) ---
 *
 * The app's "restore from backup" flow writes every combo in a tight burst (~26 back-to-back
 * set_combo/set_combo_key_positions RPCs). The original code below called combo_save_setting()
 * synchronously inside zmk_combos_set() on *every* RPC, each of which blocks on an NVS flash
 * write - unlike the Keymap tab, which only touches flash once per changed key via an explicit
 * "save changes" RPC (see zmk_keymap_save_changes() in app/src/keymap.c), combos had no such
 * batching, so a 26-combo restore paid for 26 (or more, since set_combo and
 * set_combo_key_positions each save independently) synchronous flash writes back-to-back.
 *
 * Fix: mark the edited index dirty and (re)schedule a single flush COMBO_SAVE_DEBOUNCE_MS after
 * the *last* edit, mirroring ZMK's own BLE active-profile save (see ble_save_profile() /
 * ble_save_work in app/src/ble.c - same k_work_delayable + reschedule-on-every-change pattern).
 * The in-RAM edit in combo_apply_set() already takes effect immediately regardless of when the
 * flush runs; only the flash write is delayed, so a burst of edits to the same or different
 * combos collapses into one write per touched combo, ~COMBO_SAVE_DEBOUNCE_MS after the burst
 * ends.
 *
 * Tradeoff: a power loss within COMBO_SAVE_DEBOUNCE_MS of the last edit can lose that whole
 * pending batch (previously only a single not-yet-returned edit could be lost, since every prior
 * edit was already on flash by the time the RPC response went out). See the firmware README this
 * shipped in.
 */

#define COMBO_SAVE_DEBOUNCE_MS 2000

static uint8_t combo_dirty[DIV_ROUND_UP(ARRAY_SIZE(stock_combos), 8)];

/* まとめ書き完了の合図(2026-07-23作者要望)。実装はシールド側vibe_handler.c(短いピッ)。
 * 3系統(combo/macro/td)が同時に完了しても、振動側の最小間隔ガードが1回にまとめる。 */
__attribute__((weak)) void tibiboard_on_settings_flushed(void) {}

static void combo_save_work_handler(struct k_work *work) {
    int wrote = 0;
    for (size_t i = 0; i < ARRAY_SIZE(combos); i++) {
        if (!(combo_dirty[i / 8] & BIT(i % 8))) {
            continue;
        }

        // Clear before writing: if a fresh edit lands for this index while the write below is in
        // flight, combo_schedule_save() will re-set the bit and reschedule, so it isn't lost.
        WRITE_BIT(combo_dirty[i / 8], i % 8, 0);

        int ret = combo_save_setting(i);
        if (ret < 0) {
            LOG_ERR("Failed to persist combo %d to settings (%d)", (int)i, ret);
        } else {
            wrote++;
        }
    }
    if (wrote > 0) {
        tibiboard_on_settings_flushed();
    }
}

static K_WORK_DELAYABLE_DEFINE(combo_save_work, combo_save_work_handler);

static void combo_schedule_save(size_t index) {
    WRITE_BIT(combo_dirty[index / 8], index % 8, 1);
    k_work_reschedule(&combo_save_work, K_MSEC(COMBO_SAVE_DEBOUNCE_MS));
}

// NOTE: like every settings h_set callback in this firmware, this must return 0 (skip with a
// warning) for malformed/legacy/out-of-range entries rather than an error: with the NVS backend a
// nonzero return aborts the remainder of the ENTIRE settings load (all subsystems, newest-first
// iteration), silently reverting every not-yet-loaded setting to compiled-in defaults for that
// boot. See behavior_handle_set() in app/src/behavior.c.
static int combo_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                              void *cb_arg) {
    const char *next;

    if (!settings_name_steq(name, "c", &next) || !next) {
        return 0;
    }

    char *endptr;
    unsigned long index = strtoul(next, &endptr, 10);
    if (*endptr != '\0') {
        LOG_WRN("Skipping invalid combo index: %s", next);
        return 0;
    }

    if (index >= ARRAY_SIZE(combos)) {
        // Saved against a firmware build with more combos; skip rather than fail the whole
        // settings load.
        LOG_WRN("Ignoring saved combo %lu (only %d combos in this build)", index,
                (int)ARRAY_SIZE(combos));
        return 0;
    }

    if (len < offsetof(struct combo_setting, key_positions) ||
        len > sizeof(struct combo_setting)) {
        LOG_WRN("Skipping invalid combo setting size %d", (int)len);
        return 0;
    }

    struct combo_setting setting = {0};
    int ret = read_cb(cb_arg, &setting, len);
    if (ret <= 0) {
        LOG_ERR("Failed to read combo setting (err %d), skipping", ret);
        return 0;
    }

    size_t kp_len = setting.key_position_len;
    if (kp_len > MAX_COMBO_KEYS ||
        offsetof(struct combo_setting, key_positions) + kp_len * sizeof(int32_t) > len) {
        LOG_WRN("Skipping invalid combo key position count %d", (int)kp_len);
        return 0;
    }

    // May be NULL here if the "behavior" local-id settings table hasn't been loaded yet;
    // combo_settings_commit() re-resolves from local_id afterwards, same as keymap.c.
    const char *behavior_name =
        zmk_behavior_find_behavior_name_from_local_id(setting.behavior_local_id);

    struct zmk_behavior_binding behavior = {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
        .local_id = setting.behavior_local_id,
#endif
        .behavior_dev = behavior_name,
        .param1 = setting.param1,
        .param2 = setting.param2,
    };

    ret = combo_apply_set(index, setting.timeout_ms, setting.require_prior_idle_ms,
                          setting.slow_release != 0, behavior, setting.key_positions, kp_len);
    if (ret < 0) {
        LOG_ERR("Failed to apply saved combo %lu (%d)", index, ret);
    }

    return 0;
}

static int combo_settings_commit(void) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
    for (size_t i = 0; i < ARRAY_SIZE(combos); i++) {
        struct zmk_behavior_binding *binding = &combos[i].behavior;

        if (binding->local_id > 0 && !binding->behavior_dev) {
            binding->behavior_dev =
                zmk_behavior_find_behavior_name_from_local_id(binding->local_id);

            if (!binding->behavior_dev) {
                LOG_ERR("Failed to find behavior for local ID %d for combo %d after settings load",
                        binding->local_id, (int)i);
            }
        }
    }
#endif

    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(combos, "combos", NULL, combo_settings_set, combo_settings_commit,
                               NULL);

/* "Restore Stock Settings": delete only the entries that differ from stock (edited combos are
 * exactly the ones with a stored key), then restore the RAM working copy from stock_combos[] in
 * place - no reboot needed, mirroring zmk_keymap_reset_settings(). memcmp is a safe "was edited"
 * test here: combos[] starts as a byte-for-byte memcpy of stock_combos[], and any edit (Studio
 * RPC or settings load) rewrites fields/pointers, so untouched entries stay byte-identical. */
int zmk_combos_reset_settings(void) {
    // Drop any not-yet-flushed edits and their pending flush: they're about to be overwritten by
    // stock values below, and we don't want a stale debounced write landing after the reset.
    /* _syncで「実行中のまとめ書き」の完了も待つ: 走行中のflushと入れ違いになると
     * リセット後に古い1件が書き残される(2026-07-23作者の指摘で発見した隙) */
    struct k_work_sync sync;
    k_work_cancel_delayable_sync(&combo_save_work, &sync);
    memset(combo_dirty, 0, sizeof(combo_dirty));

    int first_err = 0;
    for (size_t i = 0; i < ARRAY_SIZE(combos); i++) {
        if (memcmp(&combos[i], &stock_combos[i], sizeof(struct combo_cfg)) == 0) {
            continue;
        }

        char setting_name[24];
        sprintf(setting_name, COMBO_SETTINGS_KEY, (int)i);
        int ret = settings_delete(setting_name);
        /* FW48 (10/11 アストラ8本目⑭): 消せなかった時は失敗をアプリへ返す(前は成功と返していた)。
         * (10/11 アストラ10本目⑦) その時は RAM も戻さない: 戻すと次の「デフォルトに戻す」で出荷時と同じに見えて
         * 消すのを飛ばし、保存領域の古い設定が電源を入れ直した時に戻ってきた。戻さなければ次にもう一度消しに行く */
        if (ret < 0 && ret != -ENOENT) {
            LOG_ERR("Failed to delete combo %d setting (%d)", (int)i, ret);
            if (first_err == 0) first_err = ret;
            continue;
        }

        /* FW48 (10/11 アストラ8本目⑫): どのキーがどのコンボかの表(combo_lookup)も出荷時の形に作り直す。
         * 前は中身だけ戻して表が古いままで、電源を入れ直すまで前の組み合わせで発動していた */
        for (int kp = 0; kp < ZMK_KEYMAP_LEN; kp++) {
            sys_bitfield_clear_bit((mem_addr_t)&combo_lookup[kp], i);
        }
        combos[i] = stock_combos[i];
        initialize_combo(i);
    }

    return first_err;
}

#endif // IS_ENABLED(CONFIG_ZMK_STUDIO_COMBOS) && IS_ENABLED(CONFIG_SETTINGS)

int zmk_combos_set(size_t index, int32_t timeout_ms, int32_t require_prior_idle_ms,
                   bool slow_release, struct zmk_behavior_binding behavior,
                   const int32_t *key_positions, size_t key_positions_len) {
    int ret = combo_apply_set(index, timeout_ms, require_prior_idle_ms, slow_release, behavior,
                              key_positions, key_positions_len);
    if (ret < 0) {
        return ret;
    }

#if IS_ENABLED(CONFIG_ZMK_STUDIO_COMBOS) && IS_ENABLED(CONFIG_SETTINGS)
    // Debounced: flush to flash COMBO_SAVE_DEBOUNCE_MS after the *last* edit rather than
    // synchronously on every RPC. See combo_schedule_save() above for why. Save errors are no
    // longer reportable to this call's caller (the flush happens later, off the RPC path); they're
    // logged from combo_save_work_handler() instead.
    combo_schedule_save(index);
#endif

    return 0;
}

#else

/* No `zmk,combos` node in the devicetree: report zero combos rather than failing to link, since
 * app/src/studio/combo_subsystem.c is built unconditionally when CONFIG_ZMK_STUDIO_COMBOS=y. */

size_t zmk_combos_get_combo_count(void) { return 0; }

const int32_t *zmk_combos_get_key_positions(size_t index, size_t *len_out) {
    *len_out = 0;
    return NULL;
}

int32_t zmk_combos_get_timeout_ms(size_t index) { return -EINVAL; }

int32_t zmk_combos_get_require_prior_idle_ms(size_t index) { return -EINVAL; }

bool zmk_combos_get_slow_release(size_t index) { return false; }

uint32_t zmk_combos_get_layer_mask(size_t index) { return 0; }

const struct zmk_behavior_binding *zmk_combos_get_behavior(size_t index) { return NULL; }

int zmk_combos_set(size_t index, int32_t timeout_ms, int32_t require_prior_idle_ms,
                   bool slow_release, struct zmk_behavior_binding behavior,
                   const int32_t *key_positions, size_t key_positions_len) {
    return -EINVAL;
}

#endif
