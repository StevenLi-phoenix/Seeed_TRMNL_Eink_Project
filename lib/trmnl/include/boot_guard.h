#ifndef BOOT_GUARD_H
#define BOOT_GUARD_H

#include <stdint.h>

/**
 * Boot-loop guard (fw 1.7.0).
 *
 * Counts consecutive boots that did not end in an intentional deep sleep.
 * The state lives in RTC_NOINIT memory on the device (survives panic / WDT /
 * brownout / software resets and deep sleep; random after power-on), so it is
 * protected by a magic word plus an integrity check word.
 *
 * Once BOOT_GUARD_THRESHOLD consecutive boots failed to reach a deep sleep,
 * the device skips WiFi and deep-sleeps with exponential backoff
 * (60, 120, 240 ... capped at 1800 s). The wake after each backoff sleep (or a
 * button wake) gets exactly one normal attempt; if that attempt again fails to
 * reach a deep sleep, the next boot backs off with the next (longer) step.
 * Any intentional deep sleep resets everything.
 *
 * Everything here is pure (no hardware access) so it is unit-tested natively.
 */

#define BOOT_GUARD_MAGIC 0xB0071D70u
#define BOOT_GUARD_THRESHOLD 3u              // boots without deep sleep that trigger backoff
#define BOOT_GUARD_BACKOFF_BASE_S 60u        // first backoff sleep
#define BOOT_GUARD_BACKOFF_MAX_S 1800u       // backoff cap
#define BOOT_GUARD_MAX_FORGIVEN_RESTARTS 2u  // consecutive intentional ESP.restart() boots not counted
#define BOOT_GUARD_COUNTER_MAX 0xFFFFu       // counters saturate here (no wrap-around)

#define BOOT_GUARD_FLAG_BACKOFF_SLEPT 0x1u   // the previous boot ended in a backoff sleep
#define BOOT_GUARD_FLAG_RESTART_PENDING 0x2u // the previous boot called an intentional restart

enum BootGuardResetKind
{
  BOOT_RESET_POWER_ON,   // power-on / reset pin: state is cleared
  BOOT_RESET_DEEP_SLEEP, // wake from deep sleep
  BOOT_RESET_SOFTWARE,   // esp_restart() / ESP.restart()
  BOOT_RESET_ABNORMAL    // panic, watchdogs, brownout, unknown
};

enum BootGuardAction
{
  BOOT_GUARD_NORMAL,         // below the threshold: run the normal wake
  BOOT_GUARD_ATTEMPT,        // at/over the threshold, previous boot was a backoff sleep (or an intentional restart
                             // of an allowed attempt): one normal attempt
  BOOT_GUARD_ATTEMPT_BUTTON, // at/over the threshold, woken by the button: the user gets a normal attempt
  BOOT_GUARD_BACKOFF         // skip WiFi and deep-sleep for sleep_seconds
};

struct BootGuardState
{
  uint32_t magic;
  uint32_t boots;                // consecutive boots without an intentional deep sleep (incl. the current one)
  uint32_t backoff_step;         // backoff sleeps taken in the current episode
  uint32_t flags;                // BOOT_GUARD_FLAG_*
  uint32_t forgiven;             // consecutive intentional restarts that were not counted
  uint32_t last_abnormal_reason; // raw platform reset reason of the latest abnormal reset, 0 = none
  uint32_t check;                // integrity word over the fields above
};

struct BootGuardDecision
{
  BootGuardAction action;
  uint32_t sleep_seconds;    // only meaningful for BOOT_GUARD_BACKOFF
  uint32_t boots;            // counter value after this boot was counted
  bool reinitialized;        // state was cleared (power-on, or magic/check invalid)
  bool forgiven_restart;     // boot followed an intentional restart and was not counted
  bool abnormal;             // previous boot did not reach an intentional deep sleep, or this reset was abnormal
};

/** Backoff duration for the given step: 60 * 2^step seconds, capped at BOOT_GUARD_BACKOFF_MAX_S. */
uint32_t boot_guard_backoff_seconds(uint32_t step);

/** True when the magic word and integrity check match. */
bool boot_guard_is_valid(const BootGuardState *state);

/** Clear the state to a valid all-zero episode. */
void boot_guard_reset(BootGuardState *state);

/**
 * Count the current boot and decide what to do. Call first thing in bl_init().
 * @param state RTC_NOINIT state (updated and re-sealed in place)
 * @param kind classified reset reason
 * @param raw_reason platform reset reason value (kept for logging when abnormal)
 * @param button_wake true when woken from deep sleep by the button
 */
BootGuardDecision boot_guard_on_boot(BootGuardState *state, BootGuardResetKind kind, uint32_t raw_reason, bool button_wake);

/** Every intentional (non-backoff) deep sleep: the device is healthy, clear the episode. */
void boot_guard_on_intentional_sleep(BootGuardState *state);

/** A backoff deep sleep: keep the counter, advance the backoff step, grant one attempt on wake. */
void boot_guard_on_backoff_sleep(BootGuardState *state);

/** Right before an intentional ESP.restart() (credential reset, successful OTA): don't count the next boot. */
void boot_guard_on_intentional_restart(BootGuardState *state);

/** Short name for logs. */
const char *boot_guard_action_name(BootGuardAction action);

#endif // BOOT_GUARD_H
