#ifndef BATTERY_GUARD_H
#define BATTERY_GUARD_H

#include <stdbool.h>

/**
 * Deep-discharge guard (fw 1.7.0).
 *
 * The MCU cannot disable the charger or sense VBUS, so the only protection
 * against draining the LiPo is to stop using WiFi while the battery voltage is
 * low and wait (deep sleep) until it recovers. Readings below the invalid
 * floor are ADC garbage / no battery attached (the MCU is evidently powered,
 * i.e. on USB) and never hold the device off.
 *
 * Voltages are integer millivolts to keep the threshold comparisons exact.
 */

enum BatteryGuardAction
{
  BATTERY_GUARD_NORMAL,  // not critical: run the normal wake
  BATTERY_GUARD_INVALID, // reading below the invalid floor: ignore it, run the normal wake (releases a held guard)
  BATTERY_GUARD_ENTER,   // just became critical: draw the "battery empty" screen once, skip WiFi, sleep
  BATTERY_GUARD_HOLD,    // still critical (below the resume threshold): skip WiFi, sleep, no redraw
  BATTERY_GUARD_RELEASE  // recovered above the resume threshold: clear the flag, run the normal wake
};

/**
 * @param vbatt_mv battery reading in millivolts
 * @param in_critical persisted flag: the guard is currently holding (screen already drawn)
 * @param critical_mv enter the guard below this voltage (BATTERY_CRITICAL_V)
 * @param hysteresis_mv resume only at or above critical_mv + hysteresis_mv
 * @param invalid_below_mv readings below this are invalid and never trigger the guard
 */
BatteryGuardAction battery_guard_decide(int vbatt_mv, bool in_critical, int critical_mv, int hysteresis_mv, int invalid_below_mv);

/** True when the action means: skip WiFi and deep-sleep. */
bool battery_guard_blocks(BatteryGuardAction action);

/** Volts → millivolts, rounded to nearest. */
int battery_volts_to_mv(float volts);

/** Short name for logs. */
const char *battery_guard_action_name(BatteryGuardAction action);

#endif // BATTERY_GUARD_H
