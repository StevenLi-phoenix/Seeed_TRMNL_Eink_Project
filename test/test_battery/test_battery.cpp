#include <unity.h>
#include <battery_estimate.h>

void setUp(void)
{
}

void tearDown(void)
{
}

void test_battery_percent_full(void)
{
  TEST_ASSERT_EQUAL_INT(100, battery_percent(4.20f));
}

void test_battery_percent_empty(void)
{
  TEST_ASSERT_EQUAL_INT(0, battery_percent(3.00f));
}

void test_battery_percent_clamp_above(void)
{
  TEST_ASSERT_EQUAL_INT(100, battery_percent(4.35f)); // charging / above full
  TEST_ASSERT_EQUAL_INT(100, battery_percent(5.00f));
}

void test_battery_percent_clamp_below(void)
{
  TEST_ASSERT_EQUAL_INT(0, battery_percent(2.50f));
  TEST_ASSERT_EQUAL_INT(0, battery_percent(0.00f));
}

void test_battery_percent_table_points(void)
{
  TEST_ASSERT_EQUAL_INT(5, battery_percent(3.30f));
  TEST_ASSERT_EQUAL_INT(15, battery_percent(3.50f));
  TEST_ASSERT_EQUAL_INT(30, battery_percent(3.60f));
  TEST_ASSERT_EQUAL_INT(50, battery_percent(3.70f));
  TEST_ASSERT_EQUAL_INT(65, battery_percent(3.80f));
  TEST_ASSERT_EQUAL_INT(80, battery_percent(3.90f));
  TEST_ASSERT_EQUAL_INT(92, battery_percent(4.05f));
}

void test_battery_percent_interpolation(void)
{
  // midpoint of 3.60V(30%)..3.70V(50%) segment
  TEST_ASSERT_EQUAL_INT(40, battery_percent(3.65f));
  // midpoint of 3.90V(80%)..4.05V(92%) segment
  TEST_ASSERT_EQUAL_INT(86, battery_percent(3.975f));
}

void test_battery_percent_monotonic(void)
{
  int prev = -1;
  for (float v = 2.8f; v <= 4.4f; v += 0.01f)
  {
    int pct = battery_percent(v);
    TEST_ASSERT_TRUE_MESSAGE(pct >= prev, "battery_percent must be monotonically non-decreasing");
    TEST_ASSERT_TRUE(pct >= 0 && pct <= 100);
    prev = pct;
  }
}

void test_recovery_eta_full(void)
{
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.0f, battery_recovery_eta_days(100, 3.0f));
}

void test_recovery_eta_half(void)
{
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.5f, battery_recovery_eta_days(50, 3.0f));
}

void test_recovery_eta_empty(void)
{
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, battery_recovery_eta_days(0, 3.0f));
}

void test_recovery_eta_clamps_bad_input(void)
{
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, battery_recovery_eta_days(-10, 3.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.0f, battery_recovery_eta_days(150, 3.0f));
}

int main(int argc, char **argv)
{
  UNITY_BEGIN();
  RUN_TEST(test_battery_percent_full);
  RUN_TEST(test_battery_percent_empty);
  RUN_TEST(test_battery_percent_clamp_above);
  RUN_TEST(test_battery_percent_clamp_below);
  RUN_TEST(test_battery_percent_table_points);
  RUN_TEST(test_battery_percent_interpolation);
  RUN_TEST(test_battery_percent_monotonic);
  RUN_TEST(test_recovery_eta_full);
  RUN_TEST(test_recovery_eta_half);
  RUN_TEST(test_recovery_eta_empty);
  RUN_TEST(test_recovery_eta_clamps_bad_input);
  return UNITY_END();
}
