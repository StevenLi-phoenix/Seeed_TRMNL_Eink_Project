#include "boot_guard.h"

#include <stddef.h>

static uint32_t sat_inc(uint32_t value)
{
  return value >= BOOT_GUARD_COUNTER_MAX ? BOOT_GUARD_COUNTER_MAX : value + 1;
}

// FNV-1a over the payload words; random RTC garbage after power-on or a
// partially corrupted struct will not match.
static uint32_t compute_check(const BootGuardState *s)
{
  const uint32_t words[] = {s->magic, s->boots, s->backoff_step, s->flags, s->forgiven, s->last_abnormal_reason};
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
  {
    for (int b = 0; b < 4; b++)
    {
      hash ^= (words[i] >> (8 * b)) & 0xFFu;
      hash *= 16777619u;
    }
  }
  return hash;
}

static void seal(BootGuardState *s)
{
  s->magic = BOOT_GUARD_MAGIC;
  s->check = compute_check(s);
}

uint32_t boot_guard_backoff_seconds(uint32_t step)
{
  uint32_t seconds = BOOT_GUARD_BACKOFF_BASE_S;
  for (uint32_t i = 0; i < step && seconds < BOOT_GUARD_BACKOFF_MAX_S; i++)
  {
    seconds *= 2;
  }
  return seconds > BOOT_GUARD_BACKOFF_MAX_S ? BOOT_GUARD_BACKOFF_MAX_S : seconds;
}

bool boot_guard_is_valid(const BootGuardState *state)
{
  return state != nullptr && state->magic == BOOT_GUARD_MAGIC && state->check == compute_check(state);
}

void boot_guard_reset(BootGuardState *state)
{
  state->boots = 0;
  state->backoff_step = 0;
  state->flags = 0;
  state->forgiven = 0;
  state->last_abnormal_reason = 0;
  seal(state);
}

BootGuardDecision boot_guard_on_boot(BootGuardState *state, BootGuardResetKind kind, uint32_t raw_reason, bool button_wake)
{
  BootGuardDecision d = {};
  d.action = BOOT_GUARD_NORMAL;

  if (kind == BOOT_RESET_POWER_ON || !boot_guard_is_valid(state))
  {
    boot_guard_reset(state);
    d.reinitialized = true;
  }

  const bool backoff_slept = (state->flags & BOOT_GUARD_FLAG_BACKOFF_SLEPT) != 0;
  const bool restart_pending = (state->flags & BOOT_GUARD_FLAG_RESTART_PENDING) != 0;
  state->flags = 0; // both flags describe only the immediately preceding boot

  if (restart_pending && kind == BOOT_RESET_SOFTWARE && state->forgiven < BOOT_GUARD_MAX_FORGIVEN_RESTARTS)
  {
    // intentional ESP.restart(): this boot is a continuation, not a failure
    state->forgiven++;
    d.forgiven_restart = true;
  }
  else
  {
    state->boots = sat_inc(state->boots);
  }

  if (kind == BOOT_RESET_ABNORMAL)
  {
    state->last_abnormal_reason = raw_reason;
  }

  d.boots = state->boots;
  d.abnormal = !d.forgiven_restart && (state->boots >= 2 || kind == BOOT_RESET_ABNORMAL);

  if (state->boots >= BOOT_GUARD_THRESHOLD)
  {
    if (d.forgiven_restart)
    {
      // only a running (non-backoff) boot can restart intentionally, so this
      // boot continues an attempt that was already allowed (e.g. OTA reboot)
      d.action = BOOT_GUARD_ATTEMPT;
    }
    else if (button_wake && kind == BOOT_RESET_DEEP_SLEEP)
    {
      d.action = BOOT_GUARD_ATTEMPT_BUTTON;
    }
    else if (backoff_slept && kind == BOOT_RESET_DEEP_SLEEP)
    {
      d.action = BOOT_GUARD_ATTEMPT;
    }
    else
    {
      d.action = BOOT_GUARD_BACKOFF;
      d.sleep_seconds = boot_guard_backoff_seconds(state->backoff_step);
    }
  }

  seal(state);
  return d;
}

void boot_guard_on_intentional_sleep(BootGuardState *state)
{
  boot_guard_reset(state);
}

void boot_guard_on_backoff_sleep(BootGuardState *state)
{
  if (!boot_guard_is_valid(state))
  {
    boot_guard_reset(state);
  }
  state->backoff_step = sat_inc(state->backoff_step);
  state->flags |= BOOT_GUARD_FLAG_BACKOFF_SLEPT;
  seal(state);
}

void boot_guard_on_intentional_restart(BootGuardState *state)
{
  if (!boot_guard_is_valid(state))
  {
    boot_guard_reset(state);
  }
  state->flags |= BOOT_GUARD_FLAG_RESTART_PENDING;
  seal(state);
}

const char *boot_guard_action_name(BootGuardAction action)
{
  switch (action)
  {
  case BOOT_GUARD_NORMAL:
    return "normal";
  case BOOT_GUARD_ATTEMPT:
    return "attempt-after-backoff";
  case BOOT_GUARD_ATTEMPT_BUTTON:
    return "attempt-button";
  case BOOT_GUARD_BACKOFF:
    return "backoff";
  }
  return "?";
}
