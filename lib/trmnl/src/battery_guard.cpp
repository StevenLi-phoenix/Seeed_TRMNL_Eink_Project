#include "battery_guard.h"

BatteryGuardAction battery_guard_decide(int vbatt_mv, bool in_critical, int critical_mv, int hysteresis_mv, int invalid_below_mv)
{
  if (vbatt_mv < invalid_below_mv)
  {
    // garbage / disconnected battery: the MCU runs, so it is externally
    // powered — never hold the device off on such a reading
    return BATTERY_GUARD_INVALID;
  }
  if (in_critical)
  {
    return vbatt_mv >= critical_mv + hysteresis_mv ? BATTERY_GUARD_RELEASE : BATTERY_GUARD_HOLD;
  }
  return vbatt_mv < critical_mv ? BATTERY_GUARD_ENTER : BATTERY_GUARD_NORMAL;
}

bool battery_guard_blocks(BatteryGuardAction action)
{
  return action == BATTERY_GUARD_ENTER || action == BATTERY_GUARD_HOLD;
}

int battery_volts_to_mv(float volts)
{
  float mv = volts * 1000.0f;
  return (int)(mv >= 0.0f ? mv + 0.5f : mv - 0.5f);
}

const char *battery_guard_action_name(BatteryGuardAction action)
{
  switch (action)
  {
  case BATTERY_GUARD_NORMAL:
    return "normal";
  case BATTERY_GUARD_INVALID:
    return "invalid-reading";
  case BATTERY_GUARD_ENTER:
    return "enter";
  case BATTERY_GUARD_HOLD:
    return "hold";
  case BATTERY_GUARD_RELEASE:
    return "release";
  }
  return "?";
}
