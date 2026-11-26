#include <unity.h>
#include <string.h>
#include <boot_guard.h>

// raw esp_reset_reason_t values, only used as opaque log payload by the guard
static const uint32_t RAW_POWERON = 1;
static const uint32_t RAW_SW = 3;
static const uint32_t RAW_PANIC = 4;
static const uint32_t RAW_DEEPSLEEP = 8;

static BootGuardState state;

void setUp(void)
{
  // simulate a healthy device that just went through an intentional deep sleep
  memset(&state, 0, sizeof(state));
  boot_guard_reset(&state);
}

void tearDown(void)
{
}

static BootGuardDecision wake(bool button = false)
{
  return boot_guard_on_boot(&state, BOOT_RESET_DEEP_SLEEP, RAW_DEEPSLEEP, button);
}

static BootGuardDecision crash_boot()
{
  return boot_guard_on_boot(&state, BOOT_RESET_ABNORMAL, RAW_PANIC, false);
}

void test_backoff_schedule(void)
{
  TEST_ASSERT_EQUAL_UINT32(60, boot_guard_backoff_seconds(0));
  TEST_ASSERT_EQUAL_UINT32(120, boot_guard_backoff_seconds(1));
  TEST_ASSERT_EQUAL_UINT32(240, boot_guard_backoff_seconds(2));
  TEST_ASSERT_EQUAL_UINT32(480, boot_guard_backoff_seconds(3));
  TEST_ASSERT_EQUAL_UINT32(960, boot_guard_backoff_seconds(4));
  TEST_ASSERT_EQUAL_UINT32(1800, boot_guard_backoff_seconds(5));
  TEST_ASSERT_EQUAL_UINT32(1800, boot_guard_backoff_seconds(6));
  TEST_ASSERT_EQUAL_UINT32(1800, boot_guard_backoff_seconds(1000));
  TEST_ASSERT_EQUAL_UINT32(1800, boot_guard_backoff_seconds(0xFFFFFFFFu));
}

void test_invalid_magic_first_boot_after_flash(void)
{
  memset(&state, 0xA5, sizeof(state)); // random RTC_NOINIT garbage
  TEST_ASSERT_FALSE(boot_guard_is_valid(&state));
  BootGuardDecision d = boot_guard_on_boot(&state, BOOT_RESET_SOFTWARE, RAW_SW, false);
  TEST_ASSERT_TRUE(d.reinitialized);
  TEST_ASSERT_EQUAL_UINT32(1, d.boots);
  TEST_ASSERT_EQUAL(BOOT_GUARD_NORMAL, d.action);
  TEST_ASSERT_FALSE(d.forgiven_restart);
  TEST_ASSERT_TRUE(boot_guard_is_valid(&state));
}

void test_corrupted_state_is_reinitialized(void)
{
  crash_boot();
  crash_boot(); // boots = 2
  state.boots ^= 0x40; // single bit flip, check word no longer matches
  TEST_ASSERT_FALSE(boot_guard_is_valid(&state));
  BootGuardDecision d = crash_boot();
  TEST_ASSERT_TRUE(d.reinitialized);
  TEST_ASSERT_EQUAL_UINT32(1, d.boots);
  TEST_ASSERT_EQUAL(BOOT_GUARD_NORMAL, d.action);
}

void test_power_on_clears_state(void)
{
  for (int i = 0; i < 5; i++)
    crash_boot();
  BootGuardDecision d = boot_guard_on_boot(&state, BOOT_RESET_POWER_ON, RAW_POWERON, false);
  TEST_ASSERT_TRUE(d.reinitialized);
  TEST_ASSERT_EQUAL_UINT32(1, d.boots);
  TEST_ASSERT_EQUAL(BOOT_GUARD_NORMAL, d.action);
  TEST_ASSERT_FALSE(d.abnormal);
  TEST_ASSERT_EQUAL_UINT32(0, state.backoff_step);
}

void test_healthy_cycle_never_backs_off(void)
{
  for (int i = 0; i < 1000; i++)
  {
    BootGuardDecision d = wake();
    TEST_ASSERT_EQUAL(BOOT_GUARD_NORMAL, d.action);
    TEST_ASSERT_EQUAL_UINT32(1, d.boots);
    TEST_ASSERT_FALSE(d.abnormal);
    boot_guard_on_intentional_sleep(&state);
  }
}

void test_crash_loop_backs_off_exponentially(void)
{
  BootGuardDecision d = wake(); // normal wake, then crashes before sleeping
  TEST_ASSERT_EQUAL(BOOT_GUARD_NORMAL, d.action);
  d = crash_boot();
  TEST_ASSERT_EQUAL(BOOT_GUARD_NORMAL, d.action);
  TEST_ASSERT_TRUE(d.abnormal);
  TEST_ASSERT_EQUAL_UINT32(2, d.boots);
  d = crash_boot(); // third boot without a deep sleep
  TEST_ASSERT_EQUAL(BOOT_GUARD_BACKOFF, d.action);
  TEST_ASSERT_EQUAL_UINT32(60, d.sleep_seconds);

  const uint32_t expected[] = {120, 240, 480, 960, 1800, 1800, 1800};
  for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
  {
    boot_guard_on_backoff_sleep(&state);
    d = wake(); // exactly one normal attempt after the backoff sleep
    TEST_ASSERT_EQUAL(BOOT_GUARD_ATTEMPT, d.action);
    TEST_ASSERT_TRUE(d.abnormal);
    d = crash_boot(); // the attempt crashed again
    TEST_ASSERT_EQUAL(BOOT_GUARD_BACKOFF, d.action);
    TEST_ASSERT_EQUAL_UINT32(expected[i], d.sleep_seconds);
  }
  TEST_ASSERT_EQUAL_UINT32(RAW_PANIC, state.last_abnormal_reason);
}

void test_only_one_attempt_per_backoff(void)
{
  crash_boot();
  crash_boot();
  TEST_ASSERT_EQUAL(BOOT_GUARD_BACKOFF, crash_boot().action);
  boot_guard_on_backoff_sleep(&state);
  TEST_ASSERT_EQUAL(BOOT_GUARD_ATTEMPT, wake().action);
  // the attempt rebooted without reaching deep sleep (even a plain SW reset)
  BootGuardDecision d = boot_guard_on_boot(&state, BOOT_RESET_SOFTWARE, RAW_SW, false);
  TEST_ASSERT_EQUAL(BOOT_GUARD_BACKOFF, d.action);
  TEST_ASSERT_EQUAL_UINT32(120, d.sleep_seconds);
}

void test_recovery_resets_episode(void)
{
  crash_boot();
  crash_boot();
  crash_boot();
  boot_guard_on_backoff_sleep(&state);
  // an abnormal reset (not a timer wake) right after a backoff sleep still backs off
  TEST_ASSERT_EQUAL(BOOT_GUARD_BACKOFF, crash_boot().action);
  boot_guard_on_backoff_sleep(&state);
  TEST_ASSERT_EQUAL(BOOT_GUARD_ATTEMPT, wake().action);
  boot_guard_on_intentional_sleep(&state); // the attempt succeeded
  TEST_ASSERT_EQUAL_UINT32(0, state.boots);
  TEST_ASSERT_EQUAL_UINT32(0, state.backoff_step);
  BootGuardDecision d = wake();
  TEST_ASSERT_EQUAL(BOOT_GUARD_NORMAL, d.action);
  TEST_ASSERT_EQUAL_UINT32(1, d.boots);
  // a new crash loop starts again at the shortest backoff
  crash_boot();
  d = crash_boot();
  TEST_ASSERT_EQUAL(BOOT_GUARD_BACKOFF, d.action);
  TEST_ASSERT_EQUAL_UINT32(60, d.sleep_seconds);
}

void test_button_wake_during_backoff_runs_normally(void)
{
  crash_boot();
  crash_boot();
  TEST_ASSERT_EQUAL(BOOT_GUARD_BACKOFF, crash_boot().action);
  boot_guard_on_backoff_sleep(&state);
  BootGuardDecision d = wake(true);
  TEST_ASSERT_EQUAL(BOOT_GUARD_ATTEMPT_BUTTON, d.action);
  // that attempt crashed too: the next backoff step applies
  d = crash_boot();
  TEST_ASSERT_EQUAL(BOOT_GUARD_BACKOFF, d.action);
  TEST_ASSERT_EQUAL_UINT32(120, d.sleep_seconds);
}

void test_button_flag_ignored_on_abnormal_reset(void)
{
  crash_boot();
  crash_boot();
  BootGuardDecision d = boot_guard_on_boot(&state, BOOT_RESET_ABNORMAL, RAW_PANIC, true);
  TEST_ASSERT_EQUAL(BOOT_GUARD_BACKOFF, d.action);
}

void test_intentional_restart_is_forgiven(void)
{
  wake();
  boot_guard_on_intentional_restart(&state); // e.g. credential reset
  BootGuardDecision d = boot_guard_on_boot(&state, BOOT_RESET_SOFTWARE, RAW_SW, false);
  TEST_ASSERT_TRUE(d.forgiven_restart);
  TEST_ASSERT_FALSE(d.abnormal);
  TEST_ASSERT_EQUAL_UINT32(1, d.boots);
  TEST_ASSERT_EQUAL(BOOT_GUARD_NORMAL, d.action);
}

void test_intentional_restart_after_crashes_does_not_trap(void)
{
  wake();
  crash_boot(); // boots = 2
  boot_guard_on_intentional_restart(&state); // user soft-reset / OTA success on this boot
  BootGuardDecision d = boot_guard_on_boot(&state, BOOT_RESET_SOFTWARE, RAW_SW, false);
  TEST_ASSERT_EQUAL(BOOT_GUARD_NORMAL, d.action);
  TEST_ASSERT_EQUAL_UINT32(2, d.boots);
}

void test_intentional_restart_during_attempt_continues_attempt(void)
{
  crash_boot();
  crash_boot();
  crash_boot();
  boot_guard_on_backoff_sleep(&state);
  TEST_ASSERT_EQUAL(BOOT_GUARD_ATTEMPT, wake().action);
  boot_guard_on_intentional_restart(&state); // successful OTA during the attempt
  BootGuardDecision d = boot_guard_on_boot(&state, BOOT_RESET_SOFTWARE, RAW_SW, false);
  TEST_ASSERT_TRUE(d.forgiven_restart);
  TEST_ASSERT_EQUAL(BOOT_GUARD_ATTEMPT, d.action);
}

void test_unexpected_software_reset_is_counted(void)
{
  wake();
  BootGuardDecision d = boot_guard_on_boot(&state, BOOT_RESET_SOFTWARE, RAW_SW, false);
  TEST_ASSERT_FALSE(d.forgiven_restart);
  TEST_ASSERT_TRUE(d.abnormal);
  TEST_ASSERT_EQUAL_UINT32(2, d.boots);
}

void test_restart_pending_but_crashed_is_counted(void)
{
  wake();
  boot_guard_on_intentional_restart(&state);
  BootGuardDecision d = crash_boot(); // panicked between marking and restarting
  TEST_ASSERT_FALSE(d.forgiven_restart);
  TEST_ASSERT_EQUAL_UINT32(2, d.boots);
}

void test_endless_intentional_restarts_are_capped(void)
{
  // e.g. server keeps ordering an OTA that "succeeds" every time
  wake();
  BootGuardDecision d = {};
  int forgiven = 0;
  for (int i = 0; i < 10; i++)
  {
    boot_guard_on_intentional_restart(&state);
    d = boot_guard_on_boot(&state, BOOT_RESET_SOFTWARE, RAW_SW, false);
    if (d.forgiven_restart)
      forgiven++;
    if (d.action == BOOT_GUARD_BACKOFF)
      break;
  }
  TEST_ASSERT_EQUAL_INT(BOOT_GUARD_MAX_FORGIVEN_RESTARTS, forgiven);
  TEST_ASSERT_EQUAL(BOOT_GUARD_BACKOFF, d.action);
  TEST_ASSERT_EQUAL_UINT32(BOOT_GUARD_THRESHOLD, d.boots);
}

void test_counter_saturates(void)
{
  BootGuardDecision d = {};
  for (uint32_t i = 0; i < BOOT_GUARD_COUNTER_MAX + 100u; i++)
  {
    d = crash_boot();
  }
  TEST_ASSERT_EQUAL_UINT32(BOOT_GUARD_COUNTER_MAX, d.boots);
  TEST_ASSERT_EQUAL(BOOT_GUARD_BACKOFF, d.action);
  TEST_ASSERT_TRUE(boot_guard_is_valid(&state));
  for (uint32_t i = 0; i < BOOT_GUARD_COUNTER_MAX + 100u; i++)
  {
    boot_guard_on_backoff_sleep(&state);
  }
  TEST_ASSERT_EQUAL_UINT32(BOOT_GUARD_COUNTER_MAX, state.backoff_step);
  d = crash_boot();
  TEST_ASSERT_EQUAL_UINT32(1800, d.sleep_seconds);
}

void test_sleep_hooks_repair_invalid_state(void)
{
  memset(&state, 0x5A, sizeof(state));
  boot_guard_on_backoff_sleep(&state);
  TEST_ASSERT_TRUE(boot_guard_is_valid(&state));
  TEST_ASSERT_EQUAL_UINT32(1, state.backoff_step);
  memset(&state, 0x5A, sizeof(state));
  boot_guard_on_intentional_restart(&state);
  TEST_ASSERT_TRUE(boot_guard_is_valid(&state));
  memset(&state, 0x5A, sizeof(state));
  boot_guard_on_intentional_sleep(&state);
  TEST_ASSERT_TRUE(boot_guard_is_valid(&state));
  TEST_ASSERT_EQUAL_UINT32(0, state.boots);
}

void test_action_names(void)
{
  TEST_ASSERT_EQUAL_STRING("normal", boot_guard_action_name(BOOT_GUARD_NORMAL));
  TEST_ASSERT_EQUAL_STRING("attempt-after-backoff", boot_guard_action_name(BOOT_GUARD_ATTEMPT));
  TEST_ASSERT_EQUAL_STRING("attempt-button", boot_guard_action_name(BOOT_GUARD_ATTEMPT_BUTTON));
  TEST_ASSERT_EQUAL_STRING("backoff", boot_guard_action_name(BOOT_GUARD_BACKOFF));
}

int main(int argc, char **argv)
{
  UNITY_BEGIN();
  RUN_TEST(test_backoff_schedule);
  RUN_TEST(test_invalid_magic_first_boot_after_flash);
  RUN_TEST(test_corrupted_state_is_reinitialized);
  RUN_TEST(test_power_on_clears_state);
  RUN_TEST(test_healthy_cycle_never_backs_off);
  RUN_TEST(test_crash_loop_backs_off_exponentially);
  RUN_TEST(test_only_one_attempt_per_backoff);
  RUN_TEST(test_recovery_resets_episode);
  RUN_TEST(test_button_wake_during_backoff_runs_normally);
  RUN_TEST(test_button_flag_ignored_on_abnormal_reset);
  RUN_TEST(test_intentional_restart_is_forgiven);
  RUN_TEST(test_intentional_restart_after_crashes_does_not_trap);
  RUN_TEST(test_intentional_restart_during_attempt_continues_attempt);
  RUN_TEST(test_unexpected_software_reset_is_counted);
  RUN_TEST(test_restart_pending_but_crashed_is_counted);
  RUN_TEST(test_endless_intentional_restarts_are_capped);
  RUN_TEST(test_counter_saturates);
  RUN_TEST(test_sleep_hooks_repair_invalid_state);
  RUN_TEST(test_action_names);
  return UNITY_END();
}
