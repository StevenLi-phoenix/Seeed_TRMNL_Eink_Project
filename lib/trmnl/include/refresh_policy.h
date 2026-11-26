#ifndef REFRESH_POLICY_H
#define REFRESH_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Anti-ghosting FULL-refresh policy (fw 1.7.0).
 *
 * The old rule forced a FULL refresh every 8th update, which at a 60 s
 * cadence (server sends 60 s to >= 1.7.0 on external power) would flash the
 * panel every 8 minutes. The new rule forces FULL when:
 *   - maximum_compatibility is set, or
 *   - nothing is known since power-on/reset (updates_since_full == 0), or
 *   - updates_since_full >= REFRESH_POLICY_MAX_UPDATES (hard cap), or
 *   - updates_since_full >= REFRESH_POLICY_MIN_UPDATES and the last FULL was
 *     at least REFRESH_POLICY_MIN_AGE_S ago (or its time is unknown).
 * Without a synced clock (epoch before 2020-01-01) it falls back to the old
 * every-8 rule. Net effect: 30-min cadence → FULL every 8 updates (~4 h);
 * 60 s cadence → FULL about every 30 min.
 *
 * updates_since_full counts refreshes since the last FULL, the FULL itself
 * included (1 right after a FULL, 0 after power-on/reset).
 */

#define REFRESH_POLICY_MIN_UPDATES 8u
#define REFRESH_POLICY_MAX_UPDATES 60u
#define REFRESH_POLICY_MIN_AGE_S 1800u
#define REFRESH_POLICY_EPOCH_VALID 1577836800u // 2020-01-01T00:00:00Z

enum FullRefreshReason
{
  FULL_REASON_NONE = 0,       // no FULL needed
  FULL_REASON_MAX_COMPAT,     // server asked for maximum compatibility
  FULL_REASON_FIRST,          // first refresh since power-on/reset
  FULL_REASON_HARD_CAP,       // REFRESH_POLICY_MAX_UPDATES reached
  FULL_REASON_EVERY_8_NO_CLOCK, // clock unsynced: old every-8 rule
  FULL_REASON_AGE             // >= 8 updates and the last FULL is old enough (or of unknown time)
};

/** Decide whether this refresh must be FULL. */
FullRefreshReason refresh_policy_full_reason(bool maximum_compatibility, uint32_t updates_since_full,
                                             uint32_t now_epoch, uint32_t last_full_epoch);

/**
 * Bookkeeping after a refresh was sent to the panel.
 * @param was_full the refresh mode actually used was FULL
 * @param now_epoch current epoch (stored as last FULL time only if the clock is synced, else 0 = unknown)
 */
void refresh_policy_note_refresh(bool was_full, uint32_t now_epoch, uint32_t *updates_since_full, uint32_t *last_full_epoch);

/** Short name for logs. */
const char *refresh_policy_reason_name(FullRefreshReason reason);

#endif // REFRESH_POLICY_H
