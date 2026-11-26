#include "refresh_policy.h"

FullRefreshReason refresh_policy_full_reason(bool maximum_compatibility, uint32_t updates_since_full,
                                             uint32_t now_epoch, uint32_t last_full_epoch)
{
  if (maximum_compatibility)
    return FULL_REASON_MAX_COMPAT;
  if (updates_since_full == 0)
    return FULL_REASON_FIRST;
  if (updates_since_full >= REFRESH_POLICY_MAX_UPDATES)
    return FULL_REASON_HARD_CAP;
  if (updates_since_full < REFRESH_POLICY_MIN_UPDATES)
    return FULL_REASON_NONE;

  if (now_epoch < REFRESH_POLICY_EPOCH_VALID)
    return FULL_REASON_EVERY_8_NO_CLOCK;

  // last FULL time unknown (drawn before the clock was synced) or the clock
  // jumped backwards: be conservative and refresh
  if (last_full_epoch < REFRESH_POLICY_EPOCH_VALID || now_epoch < last_full_epoch)
    return FULL_REASON_AGE;
  if (now_epoch - last_full_epoch >= REFRESH_POLICY_MIN_AGE_S)
    return FULL_REASON_AGE;
  return FULL_REASON_NONE;
}

void refresh_policy_note_refresh(bool was_full, uint32_t now_epoch, uint32_t *updates_since_full, uint32_t *last_full_epoch)
{
  if (was_full)
  {
    *updates_since_full = 0;
    *last_full_epoch = now_epoch >= REFRESH_POLICY_EPOCH_VALID ? now_epoch : 0;
  }
  if (*updates_since_full < 0xFFFFFFFFu)
  {
    (*updates_since_full)++;
  }
}

const char *refresh_policy_reason_name(FullRefreshReason reason)
{
  switch (reason)
  {
  case FULL_REASON_NONE:
    return "none";
  case FULL_REASON_MAX_COMPAT:
    return "max-compat";
  case FULL_REASON_FIRST:
    return "first-since-reset";
  case FULL_REASON_HARD_CAP:
    return "hard-cap";
  case FULL_REASON_EVERY_8_NO_CLOCK:
    return "every-8-no-clock";
  case FULL_REASON_AGE:
    return "age";
  }
  return "?";
}
