#include <unity.h>
#include <refresh_policy.h>

static const uint32_t T0 = 1795000000u; // 2026-11, clock synced

void setUp(void)
{
}

void tearDown(void)
{
}

/**
 * Simulate `updates` refreshes every `cadence_s` seconds from power-on and
 * record which ones were forced FULL. Returns the number of FULL refreshes.
 */
static int simulate(uint32_t cadence_s, int updates, uint32_t start_epoch, int *full_idx, int max_full)
{
  uint32_t since_full = 0, last_full = 0;
  int fulls = 0;
  for (int i = 0; i < updates; i++)
  {
    uint32_t now = start_epoch + (uint32_t)i * cadence_s;
    bool full = refresh_policy_full_reason(false, since_full, now, last_full) != FULL_REASON_NONE;
    refresh_policy_note_refresh(full, now, &since_full, &last_full);
    if (full)
    {
      if (fulls < max_full)
        full_idx[fulls] = i;
      fulls++;
    }
  }
  return fulls;
}

void test_max_compat_always_full(void)
{
  TEST_ASSERT_EQUAL(FULL_REASON_MAX_COMPAT, refresh_policy_full_reason(true, 1, T0, T0));
  TEST_ASSERT_EQUAL(FULL_REASON_MAX_COMPAT, refresh_policy_full_reason(true, 0, 0, 0));
}

void test_first_refresh_after_power_on_is_full(void)
{
  TEST_ASSERT_EQUAL(FULL_REASON_FIRST, refresh_policy_full_reason(false, 0, T0, 0));
  TEST_ASSERT_EQUAL(FULL_REASON_FIRST, refresh_policy_full_reason(false, 0, 0, 0));
}

void test_thirty_minute_cadence_full_every_8(void)
{
  int idx[16];
  int fulls = simulate(1800, 48, T0, idx, 16); // 24 h
  TEST_ASSERT_EQUAL_INT(6, fulls);
  for (int i = 0; i < 6; i++)
  {
    TEST_ASSERT_EQUAL_INT(8 * i, idx[i]); // ~every 4 h, same as the old rule
  }
}

void test_sixty_second_cadence_full_every_30_min(void)
{
  int idx[16];
  int fulls = simulate(60, 180, T0, idx, 16); // 3 h
  TEST_ASSERT_EQUAL_INT(6, fulls);
  for (int i = 0; i < 6; i++)
  {
    TEST_ASSERT_EQUAL_INT(30 * i, idx[i]);
  }
}

void test_ten_second_cadence_hits_hard_cap(void)
{
  int idx[16];
  int fulls = simulate(10, 181, T0, idx, 16); // 30 min
  TEST_ASSERT_EQUAL_INT(4, fulls);
  TEST_ASSERT_EQUAL_INT(0, idx[0]);
  TEST_ASSERT_EQUAL_INT(60, idx[1]); // 600 s < 1800 s: the 60-update cap fires first
  TEST_ASSERT_EQUAL_INT(120, idx[2]);
  TEST_ASSERT_EQUAL_INT(180, idx[3]);
  TEST_ASSERT_EQUAL(FULL_REASON_HARD_CAP, refresh_policy_full_reason(false, 60, T0 + 600, T0));
}

void test_unsynced_clock_falls_back_to_every_8(void)
{
  int idx[16];
  int fulls = simulate(60, 40, 1000u, idx, 16); // epoch near 1970: never synced
  TEST_ASSERT_EQUAL_INT(5, fulls);
  for (int i = 0; i < 5; i++)
  {
    TEST_ASSERT_EQUAL_INT(8 * i, idx[i]);
  }
  TEST_ASSERT_EQUAL(FULL_REASON_EVERY_8_NO_CLOCK, refresh_policy_full_reason(false, 8, 1000u, 0));
  TEST_ASSERT_EQUAL(FULL_REASON_NONE, refresh_policy_full_reason(false, 7, 1000u, 0));
}

void test_epoch_validity_boundary(void)
{
  TEST_ASSERT_EQUAL(FULL_REASON_EVERY_8_NO_CLOCK, refresh_policy_full_reason(false, 8, REFRESH_POLICY_EPOCH_VALID - 1, 0));
  TEST_ASSERT_EQUAL(FULL_REASON_AGE, refresh_policy_full_reason(false, 8, REFRESH_POLICY_EPOCH_VALID, 0));
}

void test_unknown_last_full_time_with_synced_clock(void)
{
  // last FULL was drawn before NTP sync (stored as 0)
  TEST_ASSERT_EQUAL(FULL_REASON_NONE, refresh_policy_full_reason(false, 7, T0, 0));
  TEST_ASSERT_EQUAL(FULL_REASON_AGE, refresh_policy_full_reason(false, 8, T0, 0));
}

void test_age_boundary(void)
{
  TEST_ASSERT_EQUAL(FULL_REASON_NONE, refresh_policy_full_reason(false, 8, T0 + 1799, T0));
  TEST_ASSERT_EQUAL(FULL_REASON_AGE, refresh_policy_full_reason(false, 8, T0 + 1800, T0));
  // old enough but too few updates since the last FULL
  TEST_ASSERT_EQUAL(FULL_REASON_NONE, refresh_policy_full_reason(false, 7, T0 + 86400, T0));
  TEST_ASSERT_EQUAL(FULL_REASON_NONE, refresh_policy_full_reason(false, 59, T0 + 1000, T0));
}

void test_clock_jump_backwards(void)
{
  TEST_ASSERT_EQUAL(FULL_REASON_AGE, refresh_policy_full_reason(false, 8, T0 - 100, T0));
  TEST_ASSERT_EQUAL(FULL_REASON_NONE, refresh_policy_full_reason(false, 3, T0 - 100, T0));
}

void test_note_refresh(void)
{
  uint32_t since = 0, last = 0;
  refresh_policy_note_refresh(true, T0, &since, &last);
  TEST_ASSERT_EQUAL_UINT32(1, since);
  TEST_ASSERT_EQUAL_UINT32(T0, last);
  refresh_policy_note_refresh(false, T0 + 60, &since, &last);
  TEST_ASSERT_EQUAL_UINT32(2, since);
  TEST_ASSERT_EQUAL_UINT32(T0, last);
  // FULL while the clock is unsynced: time unknown
  refresh_policy_note_refresh(true, 500u, &since, &last);
  TEST_ASSERT_EQUAL_UINT32(1, since);
  TEST_ASSERT_EQUAL_UINT32(0, last);
  // saturation
  since = 0xFFFFFFFFu;
  refresh_policy_note_refresh(false, T0, &since, &last);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, since);
}

void test_reason_names(void)
{
  TEST_ASSERT_EQUAL_STRING("none", refresh_policy_reason_name(FULL_REASON_NONE));
  TEST_ASSERT_EQUAL_STRING("max-compat", refresh_policy_reason_name(FULL_REASON_MAX_COMPAT));
  TEST_ASSERT_EQUAL_STRING("first-since-reset", refresh_policy_reason_name(FULL_REASON_FIRST));
  TEST_ASSERT_EQUAL_STRING("hard-cap", refresh_policy_reason_name(FULL_REASON_HARD_CAP));
  TEST_ASSERT_EQUAL_STRING("every-8-no-clock", refresh_policy_reason_name(FULL_REASON_EVERY_8_NO_CLOCK));
  TEST_ASSERT_EQUAL_STRING("age", refresh_policy_reason_name(FULL_REASON_AGE));
}

int main(int argc, char **argv)
{
  UNITY_BEGIN();
  RUN_TEST(test_max_compat_always_full);
  RUN_TEST(test_first_refresh_after_power_on_is_full);
  RUN_TEST(test_thirty_minute_cadence_full_every_8);
  RUN_TEST(test_sixty_second_cadence_full_every_30_min);
  RUN_TEST(test_ten_second_cadence_hits_hard_cap);
  RUN_TEST(test_unsynced_clock_falls_back_to_every_8);
  RUN_TEST(test_epoch_validity_boundary);
  RUN_TEST(test_unknown_last_full_time_with_synced_clock);
  RUN_TEST(test_age_boundary);
  RUN_TEST(test_clock_jump_backwards);
  RUN_TEST(test_note_refresh);
  RUN_TEST(test_reason_names);
  return UNITY_END();
}
