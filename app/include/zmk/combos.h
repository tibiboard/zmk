/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/devicetree.h>
#include <zmk/behavior.h>

#define ZMK_COMBOS_UTIL_ONE(n) +1

#define ZMK_COMBOS_LEN                                                                             \
    COND_CODE_1(DT_HAS_COMPAT_STATUS_OKAY(zmk_combos),                                             \
                (0 DT_FOREACH_CHILD_STATUS_OKAY(DT_INST(0, zmk_combos), ZMK_COMBOS_UTIL_ONE)),     \
                (0))

/**
 * @brief Accessors used by the Studio "combos" RPC subsystem (see
 *        app/src/studio/combo_subsystem.c) to enumerate and edit the combos currently configured
 *        on the device, without exposing the internal `struct combo_cfg` layout from combo.c.
 *
 * As of Milestone 3: combos live in a RAM working copy (seeded from the devicetree at boot), and
 * timeout/require-prior-idle/slow-release/behavior are editable via zmk_combos_set() and take
 * effect immediately. Key positions and layer restriction aren't editable yet, and nothing is
 * persisted across reboots yet (both are later milestones), mirroring how app/src/keymap.c
 * eventually handles runtime keymap edits with settings persistence.
 */

/** @brief Number of combos currently configured (devicetree-derived for now). */
size_t zmk_combos_get_combo_count(void);

/**
 * @brief Get a pointer to the key positions array for the combo at `index`, along with its length.
 *        The returned pointer is valid for the lifetime of the combo (static storage today).
 * @return Pointer to the key positions, with `*len_out` set to the number of entries. NULL (with
 *         `*len_out` set to 0) if `index` is out of range.
 */
const int32_t *zmk_combos_get_key_positions(size_t index, size_t *len_out);

/** @return The combo's timeout in ms, or -EINVAL if `index` is out of range. */
int32_t zmk_combos_get_timeout_ms(size_t index);

/** @return The combo's require-prior-idle-ms, or -EINVAL if `index` is out of range. */
int32_t zmk_combos_get_require_prior_idle_ms(size_t index);

/** @return Whether the combo releases on last-key-up (true) or first-key-up (false). */
bool zmk_combos_get_slow_release(size_t index);

/** @return The bitmask of layer indices the combo is active on (0 == all layers). */
uint32_t zmk_combos_get_layer_mask(size_t index);

/** @return The behavior binding invoked by the combo, or NULL if `index` is out of range. */
const struct zmk_behavior_binding *zmk_combos_get_behavior(size_t index);

/**
 * @brief Update an existing combo's timeout/require-prior-idle/slow-release/behavior/key-positions
 *        in place. Takes effect immediately (no reboot/reflash needed); not persisted across
 *        reboots yet.
 * @param key_positions Array of key position indices (0..ZMK_KEYMAP_LEN-1) this combo should
 *        activate on. Replaces the combo's existing key positions entirely.
 * @param key_positions_len Number of entries in `key_positions`.
 * @return 0 on success, -EINVAL if `index` is out of range or a key position is invalid, -E2BIG
 *         if `key_positions_len` exceeds the devicetree-derived per-combo key limit.
 */
int zmk_combos_set(size_t index, int32_t timeout_ms, int32_t require_prior_idle_ms,
                   bool slow_release, struct zmk_behavior_binding behavior,
                   const int32_t *key_positions, size_t key_positions_len);

/**
 * @brief Delete every persisted combo edit and restore the RAM working copy to the stock
 *        (devicetree) values, in place. Used by the Studio "Restore Stock Settings" RPC.
 * @return 0 on success.
 */
int zmk_combos_reset_settings(void);
