/*
 * Copyright (c) 2023 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util_macro.h>
#include <string.h>

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS) &&                                                   \
    IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_ID_TYPE_SETTINGS_TABLE)

#include <zephyr/settings/settings.h>

#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#endif

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/hid.h>
#include <zmk/matrix.h>

#include <zmk/events/position_state_changed.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

const struct device *zmk_behavior_get_binding(const char *name) {
    return behavior_get_binding(name);
}

const struct device *z_impl_behavior_get_binding(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return NULL;
    }

    STRUCT_SECTION_FOREACH(zmk_behavior_ref, item) {
        if (device_is_ready(item->device) && item->device->name == name) {
            return item->device;
        }
    }

    STRUCT_SECTION_FOREACH(zmk_behavior_ref, item) {
        if (device_is_ready(item->device) && strcmp(item->device->name, name) == 0) {
            return item->device;
        }
    }

    return NULL;
}

static int invoke_locally(struct zmk_behavior_binding *binding,
                          struct zmk_behavior_binding_event event, bool pressed) {
    if (pressed) {
        return behavior_keymap_binding_pressed(binding, event);
    } else {
        return behavior_keymap_binding_released(binding, event);
    }
}

int zmk_behavior_invoke_binding(const struct zmk_behavior_binding *src_binding,
                                struct zmk_behavior_binding_event event, bool pressed) {
    // We want to make a copy of this, since it may be converted from
    // relative to absolute before being invoked
    struct zmk_behavior_binding binding = *src_binding;

    const struct device *behavior = zmk_behavior_get_binding(binding.behavior_dev);

    if (!behavior) {
        LOG_WRN("No behavior assigned to %d on layer %d", event.position, event.layer);
        return 1;
    }

    int err = behavior_keymap_binding_convert_central_state_dependent_params(&binding, event);
    if (err) {
        LOG_ERR("Failed to convert relative to absolute behavior binding (err %d)", err);
        return err;
    }

    enum behavior_locality locality = BEHAVIOR_LOCALITY_CENTRAL;
    err = behavior_get_locality(behavior, &locality);
    if (err) {
        LOG_ERR("Failed to get behavior locality %d", err);
        return err;
    }

    switch (locality) {
    case BEHAVIOR_LOCALITY_CENTRAL:
        return invoke_locally(&binding, event, pressed);
    case BEHAVIOR_LOCALITY_EVENT_SOURCE:
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        if (event.source == ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL) {
            return invoke_locally(&binding, event, pressed);
        } else {
            return zmk_split_central_invoke_behavior(event.source, &binding, event, pressed);
        }
#else
        return invoke_locally(&binding, event, pressed);
#endif
    case BEHAVIOR_LOCALITY_GLOBAL:
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        for (int i = 0; i < ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT; i++) {
            zmk_split_central_invoke_behavior(i, &binding, event, pressed);
        }
#endif
        return invoke_locally(&binding, event, pressed);
    }

    return -ENOTSUP;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

int zmk_behavior_get_empty_param_metadata(const struct device *dev,
                                          struct behavior_parameter_metadata *metadata) {
    metadata->sets_len = 0;
    return 0;
}

#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static int validate_hid_usage(uint16_t usage_page, uint16_t usage_id) {
    LOG_DBG("Validate usage %d in page %d", usage_id, usage_page);
    switch (usage_page) {
    case HID_USAGE_KEY:
        if (usage_id == 0 || (usage_id > ZMK_HID_KEYBOARD_NKRO_MAX_USAGE &&
                              usage_id < LEFT_CONTROL && usage_id > RIGHT_GUI)) {
            return -EINVAL;
        }
        break;
    case HID_USAGE_CONSUMER:
        if (usage_id >
            COND_CODE_1(IS_ENABLED(CONFIG_ZMK_HID_CONSUMER_REPORT_USAGES_BASIC), (0xFF), (0xFFF))) {
            return -EINVAL;
        }
        break;
    default:
        LOG_WRN("Unsupported HID usage page %d", usage_page);
        return -EINVAL;
    }

    return 0;
}

#define PARAM_MATCHES 0

static int check_param_matches_value(const struct behavior_parameter_value_metadata *value_meta,
                                     uint32_t param) {
    switch (value_meta->type) {
    case BEHAVIOR_PARAMETER_VALUE_TYPE_NIL:
        if (param == 0) {
            return PARAM_MATCHES;
        }
        break;
    case BEHAVIOR_PARAMETER_VALUE_TYPE_HID_USAGE:
        if (validate_hid_usage(ZMK_HID_USAGE_PAGE(param), ZMK_HID_USAGE_ID(param)) >= 0) {
            return PARAM_MATCHES;
        }

        break;
    case BEHAVIOR_PARAMETER_VALUE_TYPE_LAYER_ID:
        if (param >= 0 && param < ZMK_KEYMAP_LEN) {
            return PARAM_MATCHES;
        }
        break;
        /* TODO: Restore with HSV -> RGB refactor
        case BEHAVIOR_PARAMETER_STANDARD_DOMAIN_HSV:
            // TODO: No real way to validate? Maybe max brightness?
            break;
        */
    case BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE:
        if (param == value_meta->value) {
            return PARAM_MATCHES;
        }
        break;
    case BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE:
        if (param >= value_meta->range.min && param <= value_meta->range.max) {
            return PARAM_MATCHES;
        }
        break;
    default:
        LOG_WRN("Unknown type %d", value_meta->type);
        break;
    }

    return -ENOTSUP;
}

int zmk_behavior_validate_param_values(const struct behavior_parameter_value_metadata *values,
                                       size_t values_len, uint32_t param) {
    if (values_len == 0) {
        return -ENODEV;
    }

    for (int v = 0; v < values_len; v++) {
        int ret = check_param_matches_value(&values[v], param);
        if (ret >= 0) {
            return ret;
        }
    }

    return -EINVAL;
}

int zmk_behavior_check_params_match_metadata(const struct behavior_parameter_metadata *metadata,
                                             uint32_t param1, uint32_t param2) {
    if (!metadata || metadata->sets_len == 0) {
        if (!metadata) {
            LOG_ERR("No metadata to check against");
        }

        return (param1 == 0 && param2 == 0) ? 0 : -ENODEV;
    }

    for (int s = 0; s < metadata->sets_len; s++) {
        const struct behavior_parameter_metadata_set *set = &metadata->sets[s];
        int param1_ret =
            zmk_behavior_validate_param_values(set->param1_values, set->param1_values_len, param1);
        int param2_ret =
            zmk_behavior_validate_param_values(set->param2_values, set->param2_values_len, param2);

        if ((param1_ret >= 0 || (param1_ret == -ENODEV && param1 == 0)) &&
            (param2_ret >= 0 || (param2_ret == -ENODEV && param2 == 0))) {
            return 0;
        }
    }

    return -EINVAL;
}

#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

int zmk_behavior_validate_binding(const struct zmk_behavior_binding *binding) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    const struct device *behavior = zmk_behavior_get_binding(binding->behavior_dev);

    if (!behavior) {
        return -ENODEV;
    }

    struct behavior_parameter_metadata metadata;
    int ret = behavior_get_parameter_metadata(behavior, &metadata);

    if (ret < 0) {
        LOG_WRN("Failed getting metadata for %s: %d", binding->behavior_dev, ret);
        return ret;
    }

    return zmk_behavior_check_params_match_metadata(&metadata, binding->param1, binding->param2);
#else
    return 0;
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)

zmk_behavior_local_id_t zmk_behavior_get_local_id(const char *name) {
    if (!name) {
        return UINT16_MAX;
    }

    STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, item) {
        if (device_is_ready(item->device) && strcmp(item->device->name, name) == 0) {
            return item->local_id;
        }
    }

    return UINT16_MAX;
}

const char *zmk_behavior_find_behavior_name_from_local_id(zmk_behavior_local_id_t local_id) {
    STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, item) {
        if (device_is_ready(item->device) && item->local_id == local_id) {
            return item->device->name;
        }
    }

    return NULL;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_ID_TYPE_CRC16)

static int behavior_local_id_init(void) {
    STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, item) {
        item->local_id = crc16_ansi(item->device->name, strlen(item->device->name));
    }

    return 0;
}

SYS_INIT(behavior_local_id_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#elif IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_ID_TYPE_SETTINGS_TABLE)

static zmk_behavior_local_id_t largest_local_id = 0;

// The largest local ID ever assigned, as currently persisted under "behavior/largest". Kept as a
// dedicated, fixed-name setting (updated in place, so it never grows the settings store) so that
// the garbage collection in behavior_handle_commit() can DELETE stale "behavior/local_id/N"
// entries without a later boot re-assigning (reusing) one of those Ns for a new behavior — old
// saved bindings elsewhere in the settings store may still reference them.
static zmk_behavior_local_id_t persisted_largest_local_id = 0;

static int behavior_handle_set(const char *name, size_t len, settings_read_cb read_cb,
                               void *cb_arg) {
    const char *next;

    // IMPORTANT: with the NVS settings backend, a nonzero return from ANY h_set callback makes
    // settings_nvs_load() abort the remainder of the ENTIRE settings load (zephyr
    // subsys/settings/src/settings_nvs.c: `if (ret) break;`), and entries are iterated
    // newest-first — so one bad entry silently reverts every OLDER setting (keymap edits,
    // combo/tap-dance/macro edits, even BT bonds) to compiled-in defaults for that boot.
    // A `behavior/local_id/N` entry naming a behavior that no longer exists is EXPECTED here:
    // this table persists across firmware updates, and behaviors get renamed/removed between
    // builds. So malformed or stale entries must be skipped with a warning, never returned as
    // errors. (Before this change, a single stale name entry aborted the whole load mid-table,
    // and behavior_handle_commit() then re-assigned fresh local IDs to every device whose entry
    // was skipped — churning the persisted id<->name mapping across boots, which broke
    // local-id-based restore of keymap/combo/tap-dance/macro bindings saved on earlier boots.)
    if (settings_name_steq(name, "local_id", &next) && next) {
        char *endptr;
        zmk_behavior_local_id_t local_id = strtoul(next, &endptr, 10);
        if (*endptr != '\0') {
            LOG_WRN("Skipping invalid behavior local ID: %s with endptr %s", next, endptr);
            return 0;
        }

        if (len >= 64) {
            LOG_WRN("Skipping too large behavior name setting size (got %d expected less than %d)",
                    (int)len, 64);
            return 0;
        }

        char name[len + 1];

        int err = read_cb(cb_arg, name, len);
        if (err <= 0) {
            LOG_ERR("Failed to read behavior name from settings (err %d), skipping", err);
            return 0;
        }

        name[len] = '\0';
        STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, item) {
            if (strcmp(name, item->device->name) == 0) {
                item->local_id = local_id;
                largest_local_id = MAX(largest_local_id, local_id);
                return 0;
            }
        }

        // Stale entry from an older firmware build; keep it out of the way but make sure no new
        // assignment in behavior_handle_commit() can collide with (reuse) its ID, since older
        // saved bindings elsewhere in the settings store may still reference it.
        LOG_WRN("Ignoring saved local ID %d for unknown behavior '%s' (from an older firmware?)",
                local_id, name);
        largest_local_id = MAX(largest_local_id, local_id);

        return 0;
    }

    if (settings_name_steq(name, "largest", &next) && !next) {
        zmk_behavior_local_id_t val;
        if (len != sizeof(val)) {
            LOG_WRN("Skipping invalid largest-local-ID setting size %d", (int)len);
            return 0;
        }

        int err = read_cb(cb_arg, &val, sizeof(val));
        if (err <= 0) {
            LOG_ERR("Failed to read largest local ID from settings (err %d), skipping", err);
            return 0;
        }

        persisted_largest_local_id = val;
        largest_local_id = MAX(largest_local_id, val);
        return 0;
    }

    return 0;
}

static void behavior_persist_largest_local_id(void) {
    if (largest_local_id == persisted_largest_local_id) {
        return;
    }

    int ret = settings_save_one("behavior/largest", &largest_local_id, sizeof(largest_local_id));
    if (ret < 0) {
        // Non-fatal: retried after garbage collection frees space (and again on every later boot,
        // since persisted_largest_local_id still differs).
        LOG_WRN("Failed to persist largest behavior local ID (%d)", ret);
        return;
    }

    persisted_largest_local_id = largest_local_id;
}

/* --- Garbage collection of stale/duplicate "behavior/local_id/N" entries ---
 *
 * Historically, any boot where the settings load aborted mid-table (see the NOTE in
 * behavior_handle_set) made behavior_handle_commit() re-assign FRESH local IDs to every behavior
 * whose entry was missed, writing ~one new "behavior/local_id/N" settings entry per behavior per
 * boot with an ever-increasing N. Settings names are permanent live data in the NVS backend
 * (superseded VALUES are reclaimed by NVS gc, but a never-deleted NAME record is live forever), so
 * on a small storage partition (32KiB here) a few such boots permanently fill the store — after
 * which EVERY new settings_save_one() in the whole firmware fails with -ENOSPC (new keymap edits,
 * combo/tap-dance/macro edits, new BT bonds...), while old entries still load fine. That is
 * exactly "old combo edits persist, new tap-dance/macro edits silently don't survive a power
 * cycle".
 *
 * DELIBERATELY CONSERVATIVE, because this runs unconditionally on every boot, nested inside the
 * settings-load call stack, against real accumulated flash content this code has never been
 * tested against (a clean dev build can't reproduce months of reflash churn on a real board). But
 * it must also actually CONVERGE in a small, human-testable number of boots, not one deletion per
 * boot: a real device from this project's history plausibly has high hundreds to low thousands of
 * stale entries (79 behaviors x many historical "settings load aborted mid-table" boots before the
 * behavior.c fix that stopped the churn - see the NOTE in behavior_handle_set), and nobody can
 * power-cycle a keyboard by hand that many times to test a fix.
 *
 * Design, and why each piece is sized the way it is:
 *
 *  - Exactly ONE settings_load_subtree_direct() walk of the "behavior" subtree per boot - never a
 *    rescan-after-every-batch loop. This is the part that actually hung boot in the previous
 *    revision: with CONFIG_NVS_LOOKUP_CACHE off, every walk is a linear flash scan, and repeating
 *    that whole-subtree scan once per batch-of-64 (up to 64 times) compounded into a very long or
 *    effectively-hung boot with no watchdog to bound it. A SINGLE walk's cost is no different from
 *    the single walk settings_load() itself already does for every subtree on every boot.
 *  - During that one walk, collect stale/duplicate numeric local IDs (not full name strings - just
 *    the uint16_t id, 2 bytes each) into a static (.bss) array sized 1024 entries (2KiB). That
 *    comfortably covers a worst-case estimate of ~1600 stale entries (79 behaviors x ~20 bad boots)
 *    with headroom, while costing a trivial fraction of this build's ~48%-used 256KiB RAM budget.
 *    Bounding the collection (rather than an unbounded/heap-backed list) keeps this a fixed, known
 *    RAM cost regardless of how messy the real store turns out to be; if the store somehow has
 *    more than 1024 stale entries, the excess is simply left for a later boot's walk to collect.
 *  - After the walk, delete up to BEHAVIOR_LOCAL_ID_GC_MAX_DELETES_PER_BOOT (128) of the collected
 *    IDs THIS boot - a bounded burst, not "delete all 1024 in one go." Each settings_delete() is a
 *    real NVS write (nvs_write with len=0), and internally walks backward through the store's ATEs
 *    to find the existing entry before it can tombstone it (no lookup cache configured, so this is
 *    also a linear scan, same cost class as the walk above) - 128 such deletes is the same order of
 *    magnitude of NVS work as the *single* full-subtree read-scan that already runs unconditionally
 *    every boot elsewhere in this function, so it should add at most roughly that much extra boot
 *    time, not a qualitatively different amount. It is also the same "loop calling settings_delete
 *    per entry" pattern zmk_keymap_reset_settings() (app/src/keymap.c) already uses elsewhere in
 *    this codebase with no special batching precautions, just capped here to keep any single boot's
 *    worst case bounded and predictable rather than open-ended. 128/boot converges a ~1600-entry
 *    worst case in ~13 boots, and anything at or under 128 stale entries (plausibly the common
 *    case) converges in a single boot.
 *  - All state (the id array, count, and cursor) is static (.bss), not on the stack: this code runs
 *    nested several frames deep inside settings_load()'s own call stack (itself deep inside
 *    main()'s init sequence), which is not the place to add large stack frames.
 *
 * Deletes every "behavior/local_id/N" entry that is not the CURRENT mapping for a live behavior:
 *  - entries naming a behavior that no longer exists in this firmware, and
 *  - duplicate entries for a live behavior whose N lost to the entry that actually stuck.
 *    (NVS iterates newest-first, so the OLDEST entry for a name wins in behavior_handle_set —
 *    which is what keeps IDs referenced by previously saved bindings resolving correctly.)
 * ID reuse for the deleted Ns is prevented by persisting largest_local_id under
 * "behavior/largest" (see above) rather than by keeping the stale entries around.
 */

#define BEHAVIOR_LOCAL_ID_GC_MAX_COLLECT 1024
#define BEHAVIOR_LOCAL_ID_GC_MAX_DELETES_PER_BOOT 128

struct behavior_local_id_gc_state {
    zmk_behavior_local_id_t stale_ids[BEHAVIOR_LOCAL_ID_GC_MAX_COLLECT];
    size_t count;
    bool overflowed;
};

// Static, not on the stack: behavior_local_id_gc_cb() runs nested inside settings_load()'s own
// call stack (settings_load -> csi_load -> settings_call_set_handler -> h_commit ->
// behavior_handle_commit -> behavior_local_id_gc -> settings_load_subtree_direct -> csi_load ->
// ... -> this callback), several frames deeper than a normal settings h_set/h_commit callback.
static struct behavior_local_id_gc_state gc_state;
static char gc_name_buf[64];

static int behavior_local_id_gc_cb(const char *key, size_t len, settings_read_cb read_cb,
                                   void *cb_arg, void *param) {
    struct behavior_local_id_gc_state *state = param;
    const char *next;

    if (!settings_name_steq(key, "local_id", &next) || !next) {
        return 0;
    }

    char *endptr;
    unsigned long entry_id = strtoul(next, &endptr, 10);
    if (*endptr != '\0' || entry_id == 0 || entry_id > UINT16_MAX) {
        // Not a name this firmware ever wrote; leave it alone rather than guess.
        return 0;
    }

    if (len == 0 || len >= sizeof(gc_name_buf)) {
        // A torn/corrupted entry (e.g. from a prior -ENOSPC write) can have an unexpected length;
        // treat as unreadable rather than risk misclassifying it. It will be reconsidered (and,
        // if genuinely stale, deleted) on a later boot once the store has more room.
        return 0;
    }

    int err = read_cb(cb_arg, gc_name_buf, len);
    if (err <= 0) {
        return 0;
    }
    gc_name_buf[len] = '\0';

    bool keep = false;
    STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, item) {
        if (!item->device || !item->device->name) {
            continue;
        }
        if (strcmp(gc_name_buf, item->device->name) == 0) {
            keep = (item->local_id == (zmk_behavior_local_id_t)entry_id);
            break;
        }
    }

    if (keep) {
        return 0;
    }

    // Unknown or superseded: queue for deletion, up to the fixed collection cap.
    if (state->count >= BEHAVIOR_LOCAL_ID_GC_MAX_COLLECT) {
        state->overflowed = true;
        return 0;
    }

    state->stale_ids[state->count++] = (zmk_behavior_local_id_t)entry_id;
    return 0;
}

static void behavior_local_id_gc(void) {
    LOG_INF("behavior_local_id_gc: starting single subtree scan");

    gc_state = (struct behavior_local_id_gc_state){0};

    // Exactly one walk, ever, per boot - never repeated/rescanned. See the big comment above for
    // why this must never become a loop.
    int ret = settings_load_subtree_direct("behavior", behavior_local_id_gc_cb, &gc_state);
    if (ret < 0) {
        LOG_ERR("Failed to enumerate behavior local ID entries for cleanup (%d)", ret);
        return;
    }

    LOG_INF("behavior_local_id_gc: scan found %d stale entr%s%s", (int)gc_state.count,
            gc_state.count == 1 ? "y" : "ies", gc_state.overflowed ? " (truncated, more remain)" : "");

    size_t to_delete = gc_state.count;
    if (to_delete > BEHAVIOR_LOCAL_ID_GC_MAX_DELETES_PER_BOOT) {
        to_delete = BEHAVIOR_LOCAL_ID_GC_MAX_DELETES_PER_BOOT;
    }

    size_t deleted = 0;
    for (size_t i = 0; i < to_delete; i++) {
        char setting_name[32];
        snprintf(setting_name, sizeof(setting_name), "behavior/local_id/%u", gc_state.stale_ids[i]);

        ret = settings_delete(setting_name);
        if (ret < 0) {
            LOG_ERR("Failed to delete stale settings entry %s (%d), stopping this boot's cleanup "
                    "early",
                    setting_name, ret);
            break;
        }
        deleted++;
    }

    bool more_remain = gc_state.overflowed || deleted < gc_state.count;
    if (deleted > 0) {
        LOG_WRN("Deleted %d stale/duplicate behavior local ID settings entries this boot%s",
                (int)deleted, more_remain ? "; more remain and will be cleaned up on later boots"
                                          : "");
    } else {
        LOG_INF("behavior_local_id_gc: nothing to delete");
    }
}

static int behavior_handle_commit(void) {
    // Cheap, early marker for serial console debugging: if a boot hangs somewhere in this
    // function or in behavior_local_id_gc() below (both new/nested in the settings-load call
    // stack), this line reaching the console proves the hang is at or after this point, not
    // earlier in settings_load() or the rest of boot.
    LOG_INF("behavior_handle_commit: start");

    STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, item) {
        if (item->local_id != 0) {
            continue;
        }

        if (!item->device || !item->device->name || !device_is_ready(item->device)) {
            LOG_WRN("Skipping ID for device that doesn't exist or isn't ready");
            continue;
        }

        item->local_id = ++largest_local_id;
        char setting_name[32];
        sprintf(setting_name, "behavior/local_id/%d", item->local_id);

        // If the `device->name` is readonly in flash, settings save can fail to copy/read it while
        // persisting to flash, so copy the device name into memory first before saving.
        char device_name[32];
        snprintf(device_name, ARRAY_SIZE(device_name), "%s", item->device->name);

        settings_save_one(setting_name, device_name, strlen(device_name));
    }

    // First attempt may fail with -ENOSPC on a store already filled by historical churn (see
    // behavior_local_id_gc), so retry after the cleanup has freed space. Once the store is clean
    // and the value is persisted, both calls and the GC pass are cheap no-ops.
    behavior_persist_largest_local_id();
    behavior_local_id_gc();
    behavior_persist_largest_local_id();

    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(behavior, "behavior", NULL, behavior_handle_set,
                               behavior_handle_commit, NULL);

#else

#error "A behavior local ID mechanism must be selected"

#endif

#endif

#if IS_ENABLED(CONFIG_LOG)
static int check_behavior_names(void) {
    // Behavior names must be unique, but we don't have a good way to enforce this
    // at compile time, so log an error at runtime if they aren't unique.
    ptrdiff_t count;
    STRUCT_SECTION_COUNT(zmk_behavior_ref, &count);

    for (ptrdiff_t i = 0; i < count; i++) {
        const struct zmk_behavior_ref *current;
        STRUCT_SECTION_GET(zmk_behavior_ref, i, &current);

        for (ptrdiff_t j = i + 1; j < count; j++) {
            const struct zmk_behavior_ref *other;
            STRUCT_SECTION_GET(zmk_behavior_ref, j, &other);

            if (strcmp(current->device->name, other->device->name) == 0) {
                LOG_ERR("Multiple behaviors have the same name '%s'", current->device->name);
            }
        }
    }

    return 0;
}

SYS_INIT(check_behavior_names, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif // IS_ENABLED(CONFIG_LOG)

#if IS_ENABLED(CONFIG_SETTINGS) && IS_ENABLED(CONFIG_ZMK_USB_LOGGING)

/* --- TEMPORARY SETTINGS-STORE DIAGNOSTIC — debug builds (USB logging enabled) only ---
 *
 * Persistence debugging so far has been theory-driven; this gets ground truth from the real
 * board instead. Every 30s (first run 8s after boot, so a serial console can attach late and
 * still catch one) it:
 *   1. walks the ENTIRE settings store and logs a census of what's actually in flash
 *      (how many entries per subsystem prefix, total count, total value bytes), and
 *   2. once per boot, probe-writes a dummy setting and logs the exact return codes, which
 *      directly answers "do new settings writes currently fail (-ENOSPC = store full)?"
 *
 * Compiles out entirely in production builds (CONFIG_ZMK_USB_LOGGING=n). Remove once the
 * tap-dance/macro persistence issue is resolved.
 */

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#if IS_ENABLED(CONFIG_ZMK_STUDIO_TAP_DANCES)
#include <zmk/tap_dances.h>
#endif

struct settings_census {
    int behavior_local_id;
    int behavior_other;
    int keymap;
    int combos;
    int td;
    int macros;
    int bt_ble;
    int other;
    int total;
    size_t value_bytes;
};

// Dump the raw stored value of interesting (td/macro/combo) entries so their embedded behavior
// local IDs can be cross-checked against the live local-id table — capped per census run to
// avoid flooding the log backend.
#define SETTINGS_DIAG_MAX_DUMPS 24

static int settings_diag_dumps;

static void settings_diag_dump_value(const char *key, size_t len, settings_read_cb read_cb,
                                     void *cb_arg) {
    uint8_t val[32];
    char hex[2 * sizeof(val) + 1];

    if (settings_diag_dumps >= SETTINGS_DIAG_MAX_DUMPS) {
        return;
    }
    settings_diag_dumps++;

    size_t to_read = MIN(len, sizeof(val));
    int rd = read_cb(cb_arg, val, to_read);
    if (rd <= 0) {
        LOG_WRN("SETTINGS-DIAG entry '%s' len %d: read failed (%d)", key, (int)len, rd);
        return;
    }

    for (int i = 0; i < rd; i++) {
        snprintf(&hex[2 * i], 3, "%02x", val[i]);
    }
    hex[2 * rd] = '\0';

    LOG_WRN("SETTINGS-DIAG entry '%s' len %d: %s%s", key, (int)len, hex,
            (size_t)rd < len ? "..." : "");
}

static int settings_census_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
                              void *param) {
    struct settings_census *c = param;

    c->total++;
    c->value_bytes += len;

    if (strncmp(key, "behavior/local_id", 17) == 0) {
        c->behavior_local_id++;
    } else if (strncmp(key, "behavior", 8) == 0) {
        c->behavior_other++;
        settings_diag_dump_value(key, len, read_cb, cb_arg);
    } else if (strncmp(key, "keymap", 6) == 0) {
        c->keymap++;
    } else if (strncmp(key, "combos", 6) == 0) {
        c->combos++;
        settings_diag_dump_value(key, len, read_cb, cb_arg);
    } else if (strncmp(key, "td", 2) == 0 && (key[2] == '/' || key[2] == '\0')) {
        c->td++;
        settings_diag_dump_value(key, len, read_cb, cb_arg);
    } else if (strncmp(key, "macros", 6) == 0) {
        c->macros++;
        settings_diag_dump_value(key, len, read_cb, cb_arg);
    } else if (strncmp(key, "bt", 2) == 0 || strncmp(key, "ble", 3) == 0) {
        c->bt_ble++;
    } else {
        c->other++;
    }

    return 0;
}

static void settings_diag_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(settings_diag_work, settings_diag_work_cb);
static int settings_diag_run;
static struct settings_census settings_diag_census;

static void settings_diag_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

    settings_diag_run++;
    settings_diag_census = (struct settings_census){0};
    settings_diag_dumps = 0;

    int rc = settings_load_subtree_direct(NULL, settings_census_cb, &settings_diag_census);

    LOG_WRN("SETTINGS-DIAG census (rc %d): total %d entries, %d value bytes | "
            "behavior/local_id %d, behavior other %d, keymap %d, combos %d, td %d, macros %d, "
            "bt/ble %d, other %d",
            rc, settings_diag_census.total, (int)settings_diag_census.value_bytes,
            settings_diag_census.behavior_local_id, settings_diag_census.behavior_other,
            settings_diag_census.keymap, settings_diag_census.combos, settings_diag_census.td,
            settings_diag_census.macros, settings_diag_census.bt_ble, settings_diag_census.other);

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS) &&                                                   \
    IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_ID_TYPE_SETTINGS_TABLE)
    LOG_WRN("SETTINGS-DIAG local IDs: largest %d, persisted largest %d; boot GC scan found %d "
            "stale%s",
            (int)largest_local_id, (int)persisted_largest_local_id, (int)gc_state.count,
            gc_state.overflowed ? " (truncated, more remain)" : "");

    // Dump the live behavior name <-> local ID table on the 3rd census run (spread out so a
    // serial console attached ~15s after boot observes each phase live), so saved bindings'
    // embedded local IDs (see the entry dumps above) can be cross-checked against what this
    // build resolves.
    if (settings_diag_run == 3) {
        STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, item) {
            LOG_WRN("SETTINGS-DIAG id-map: %d = '%s'", (int)item->local_id,
                    item->device ? item->device->name : "(null)");
        }
    }
#endif

    // Probe write every census run (fixed key, superseded in place), so a late-attached serial
    // console still sees a fresh result.
    uint32_t v = k_uptime_get_32();
    int wrc = settings_save_one("diag/probe", &v, sizeof(v));
    int drc = settings_delete("diag/probe");
    LOG_WRN("SETTINGS-DIAG probe: save rc %d, delete rc %d (0 = OK, -28 = ENOSPC/store full)", wrc,
            drc);

#if IS_ENABLED(CONFIG_ZMK_STUDIO_TAP_DANCES)
    // Dump the LIVE tap-dance structs (the exact memory keypress execution and RPC reporting
    // read). Flash provably holds the edited values (see entry dumps above); this shows whether
    // boot-time settings_load() actually APPLIED them. Then, once per boot, re-dispatch the "td"
    // subtree load by hand and dump again — if values change only after the re-dispatch, boot-time
    // dispatch is what's broken; if they never change, the h_set apply path itself is broken.
    for (size_t i = 0; i < 2 && i < zmk_tap_dances_get_count(); i++) {
        size_t blen = 0;
        const struct zmk_behavior_binding *bs = zmk_tap_dances_get_bindings(i, &blen);
        LOG_WRN("SETTINGS-DIAG live td[%d]: term %d, b0 dev='%s' p1=0x%08x, b1 dev='%s' "
                "p1=0x%08x",
                (int)i, (int)zmk_tap_dances_get_tapping_term_ms(i),
                (blen > 0 && bs[0].behavior_dev) ? bs[0].behavior_dev : "(null)",
                blen > 0 ? bs[0].param1 : 0,
                (blen > 1 && bs[1].behavior_dev) ? bs[1].behavior_dev : "(null)",
                blen > 1 ? bs[1].param1 : 0);
    }

    if (settings_diag_run == 2) {
        int lrc = settings_load_subtree("td");
        LOG_WRN("SETTINGS-DIAG manual settings_load_subtree(\"td\") rc %d; re-dumping live:", lrc);
        for (size_t i = 0; i < 2 && i < zmk_tap_dances_get_count(); i++) {
            size_t blen = 0;
            const struct zmk_behavior_binding *bs = zmk_tap_dances_get_bindings(i, &blen);
            LOG_WRN("SETTINGS-DIAG live-after td[%d]: term %d, b0 dev='%s' p1=0x%08x",
                    (int)i, (int)zmk_tap_dances_get_tapping_term_ms(i),
                    (blen > 0 && bs[0].behavior_dev) ? bs[0].behavior_dev : "(null)",
                    blen > 0 ? bs[0].param1 : 0);
        }
    }
#endif

    k_work_schedule(&settings_diag_work, K_SECONDS(30));
}

static int settings_diag_init(void) {
    k_work_schedule(&settings_diag_work, K_SECONDS(8));
    return 0;
}

SYS_INIT(settings_diag_init, APPLICATION, 99);

#endif // CONFIG_SETTINGS && CONFIG_ZMK_USB_LOGGING
