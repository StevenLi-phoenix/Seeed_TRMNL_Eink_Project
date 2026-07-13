#include "battery_estimate.h"

// Typical 1S LiPo resting-voltage discharge curve, coarse piecewise-linear.
// Kept monotonic; endpoints clamp.
struct VoltagePoint
{
  float voltage;
  int percent;
};

static const VoltagePoint CURVE[] = {
    {3.00f, 0},
    {3.30f, 5},
    {3.50f, 15},
    {3.60f, 30},
    {3.70f, 50},
    {3.80f, 65},
    {3.90f, 80},
    {4.05f, 92},
    {4.20f, 100},
};
static const int CURVE_LEN = sizeof(CURVE) / sizeof(CURVE[0]);

int battery_percent(float voltage)
{
  if (voltage <= CURVE[0].voltage)
    return CURVE[0].percent;
  if (voltage >= CURVE[CURVE_LEN - 1].voltage)
    return CURVE[CURVE_LEN - 1].percent;

  for (int i = 1; i < CURVE_LEN; i++)
  {
    if (voltage <= CURVE[i].voltage)
    {
      float span_v = CURVE[i].voltage - CURVE[i - 1].voltage;
      float span_p = (float)(CURVE[i].percent - CURVE[i - 1].percent);
      float frac = (voltage - CURVE[i - 1].voltage) / span_v;
      return CURVE[i - 1].percent + (int)(span_p * frac + 0.5f);
    }
  }
  return CURVE[CURVE_LEN - 1].percent; // unreachable
}

float battery_recovery_eta_days(int percent, float full_battery_days)
{
  if (percent < 0)
    percent = 0;
  if (percent > 100)
    percent = 100;
  return full_battery_days * (float)percent / 100.0f;
}
