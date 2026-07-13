#ifndef BATTERY_ESTIMATE_H
#define BATTERY_ESTIMATE_H

/**
 * @brief Convert a 1S LiPo battery voltage to an approximate state of charge.
 * @param voltage battery voltage in Volts (resting, as read by readBatteryVoltage())
 * @return percentage 0..100, clamped; piecewise-linear over a typical discharge curve
 */
int battery_percent(float voltage);

/**
 * @brief Estimate how long the battery lasts in WiFi-recovery duty cycle.
 * @param percent current state of charge (0..100, clamped)
 * @param full_battery_days days a full battery lasts in recovery mode (see BATTERY_RECOVERY_FULL_DAYS)
 * @return estimated days remaining
 */
float battery_recovery_eta_days(int percent, float full_battery_days);

#endif // BATTERY_ESTIMATE_H
