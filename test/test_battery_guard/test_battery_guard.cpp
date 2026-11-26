#include <unity.h>
#include <battery_guard.h>

// mirror include/config.h (BATTERY_CRITICAL_V 3.40, +0.10 V hysteresis, < 2.5 V invalid)
static const int CRIT = 3400;
static const int HYST = 100;
static const int INVALID = 2500;

void setUp(void)
{
}

void tearDown(void)
{
}

static BatteryGuardAction decide(int mv, bool in_critical)
{
  return battery_guard_decide(mv, in_critical, CRIT, HYST, INVALID);
}

void test_healthy_and_plugged_in_readings_are_normal(void)
{
  TEST_ASSERT_EQUAL(BATTERY_GUARD_NORMAL, decide(4190, false));
  TEST_ASSERT_EQUAL(BATTERY_GUARD_NORMAL, decide(4050, false));
  TEST_ASSERT_EQUAL(BATTERY_GUARD_NORMAL, decide(3700, false));
}

void test_enter_boundary(void)
{
  TEST_ASSERT_EQUAL(BATTERY_GUARD_NORMAL, decide(3400, false));
  TEST_ASSERT_EQUAL(BATTERY_GUARD_ENTER, decide(3399, false));
  TEST_ASSERT_EQUAL(BATTERY_GUARD_ENTER, decide(2920, false)); // Sep 2026 incident level
  TEST_ASSERT_EQUAL(BATTERY_GUARD_ENTER, decide(2500, false)); // lowest valid reading
}

void test_hysteresis_hold_and_release(void)
{
  TEST_ASSERT_EQUAL(BATTERY_GUARD_HOLD, decide(3000, true));
  TEST_ASSERT_EQUAL(BATTERY_GUARD_HOLD, decide(3400, true));
  TEST_ASSERT_EQUAL(BATTERY_GUARD_HOLD, decide(3499, true));
  TEST_ASSERT_EQUAL(BATTERY_GUARD_RELEASE, decide(3500, true));
  TEST_ASSERT_EQUAL(BATTERY_GUARD_RELEASE, decide(4190, true));
  // inside the hysteresis band a non-critical device stays normal
  TEST_ASSERT_EQUAL(BATTERY_GUARD_NORMAL, decide(3450, false));
}

void test_invalid_readings_never_trigger(void)
{
  TEST_ASSERT_EQUAL(BATTERY_GUARD_INVALID, decide(700, false)); // seen in the field: battery disconnected
  TEST_ASSERT_EQUAL(BATTERY_GUARD_INVALID, decide(0, false));
  TEST_ASSERT_EQUAL(BATTERY_GUARD_INVALID, decide(2499, false));
  TEST_ASSERT_EQUAL(BATTERY_GUARD_INVALID, decide(-5, false));
  // the MCU runs, so it is externally powered: an invalid reading releases a held guard
  TEST_ASSERT_EQUAL(BATTERY_GUARD_INVALID, decide(700, true));
}

void test_blocks(void)
{
  TEST_ASSERT_TRUE(battery_guard_blocks(BATTERY_GUARD_ENTER));
  TEST_ASSERT_TRUE(battery_guard_blocks(BATTERY_GUARD_HOLD));
  TEST_ASSERT_FALSE(battery_guard_blocks(BATTERY_GUARD_NORMAL));
  TEST_ASSERT_FALSE(battery_guard_blocks(BATTERY_GUARD_INVALID));
  TEST_ASSERT_FALSE(battery_guard_blocks(BATTERY_GUARD_RELEASE));
}

void test_volts_to_mv(void)
{
  TEST_ASSERT_EQUAL_INT(3400, battery_volts_to_mv(3.40f));
  TEST_ASSERT_EQUAL_INT(3500, battery_volts_to_mv(3.40f + 0.10f));
  TEST_ASSERT_EQUAL_INT(4190, battery_volts_to_mv(4.19f));
  TEST_ASSERT_EQUAL_INT(700, battery_volts_to_mv(0.7f));
  TEST_ASSERT_EQUAL_INT(0, battery_volts_to_mv(0.0f));
  TEST_ASSERT_EQUAL_INT(3500, battery_volts_to_mv(3.4996f));
  TEST_ASSERT_EQUAL_INT(3399, battery_volts_to_mv(3.3994f));
}

// Discharge 4.20 V → 2.90 V, then charge back up; the guard must enter exactly
// once (one screen draw) and release exactly once, at >= 3.50 V.
void test_discharge_and_charge_cycle(void)
{
  bool in_critical = false;
  int enters = 0, releases = 0, release_mv = 0, enter_mv = 0;
  for (int pass = 0; pass < 2; pass++)
  {
    for (int i = 0; i <= 130; i++)
    {
      int mv = pass == 0 ? 4200 - 10 * i : 2900 + 10 * i;
      BatteryGuardAction a = decide(mv, in_critical);
      if (a == BATTERY_GUARD_ENTER)
      {
        enters++;
        enter_mv = mv;
        in_critical = true;
      }
      else if (a == BATTERY_GUARD_RELEASE || a == BATTERY_GUARD_INVALID)
      {
        if (in_critical)
        {
          releases++;
          release_mv = mv;
        }
        in_critical = false;
      }
    }
  }
  TEST_ASSERT_EQUAL_INT(1, enters);
  TEST_ASSERT_EQUAL_INT(3390, enter_mv);
  TEST_ASSERT_EQUAL_INT(1, releases);
  TEST_ASSERT_EQUAL_INT(3500, release_mv);
}

void test_action_names(void)
{
  TEST_ASSERT_EQUAL_STRING("normal", battery_guard_action_name(BATTERY_GUARD_NORMAL));
  TEST_ASSERT_EQUAL_STRING("invalid-reading", battery_guard_action_name(BATTERY_GUARD_INVALID));
  TEST_ASSERT_EQUAL_STRING("enter", battery_guard_action_name(BATTERY_GUARD_ENTER));
  TEST_ASSERT_EQUAL_STRING("hold", battery_guard_action_name(BATTERY_GUARD_HOLD));
  TEST_ASSERT_EQUAL_STRING("release", battery_guard_action_name(BATTERY_GUARD_RELEASE));
}

int main(int argc, char **argv)
{
  UNITY_BEGIN();
  RUN_TEST(test_healthy_and_plugged_in_readings_are_normal);
  RUN_TEST(test_enter_boundary);
  RUN_TEST(test_hysteresis_hold_and_release);
  RUN_TEST(test_invalid_readings_never_trigger);
  RUN_TEST(test_blocks);
  RUN_TEST(test_volts_to_mv);
  RUN_TEST(test_discharge_and_charge_cycle);
  RUN_TEST(test_action_names);
  return UNITY_END();
}
