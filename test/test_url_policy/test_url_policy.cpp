#include <unity.h>
#include <url_policy.h>
#include <string.h>

static const char *BASE = "https://display.lishuyu.app";

void setUp(void)
{
}

void tearDown(void)
{
}

void test_extract_host_basic(void)
{
  char host[64];
  TEST_ASSERT_TRUE(url_extract_host("https://display.lishuyu.app/fw/1.7.1.bin", host, sizeof(host)));
  TEST_ASSERT_EQUAL_STRING("display.lishuyu.app", host);
  TEST_ASSERT_TRUE(url_extract_host("http://192.168.1.5:2300", host, sizeof(host)));
  TEST_ASSERT_EQUAL_STRING("192.168.1.5", host);
  TEST_ASSERT_TRUE(url_extract_host("https://Display.LISHUYU.app:443?x=1", host, sizeof(host)));
  TEST_ASSERT_EQUAL_STRING("display.lishuyu.app", host);
  TEST_ASSERT_TRUE(url_extract_host("https://display.lishuyu.app#frag", host, sizeof(host)));
  TEST_ASSERT_EQUAL_STRING("display.lishuyu.app", host);
}

void test_extract_host_rejects_malformed(void)
{
  char host[64];
  TEST_ASSERT_FALSE(url_extract_host("ftp://display.lishuyu.app/", host, sizeof(host)));
  TEST_ASSERT_EQUAL_STRING("", host);
  TEST_ASSERT_FALSE(url_extract_host("display.lishuyu.app/fw.bin", host, sizeof(host)));
  TEST_ASSERT_FALSE(url_extract_host("https:///fw.bin", host, sizeof(host)));
  TEST_ASSERT_FALSE(url_extract_host("https://:443/fw.bin", host, sizeof(host)));
  TEST_ASSERT_FALSE(url_extract_host("https://display.lishuyu.app@evil.example/fw.bin", host, sizeof(host)));
  TEST_ASSERT_FALSE(url_extract_host("https://evil.example\\@display.lishuyu.app/", host, sizeof(host)));
  TEST_ASSERT_FALSE(url_extract_host(nullptr, host, sizeof(host)));
  TEST_ASSERT_FALSE(url_extract_host("https://a.b", nullptr, 0));
}

void test_extract_host_too_long(void)
{
  char host[8];
  TEST_ASSERT_TRUE(url_extract_host("https://1234567/", host, sizeof(host)));
  TEST_ASSERT_EQUAL_STRING("1234567", host);
  TEST_ASSERT_FALSE(url_extract_host("https://12345678/", host, sizeof(host)));
  TEST_ASSERT_EQUAL_STRING("", host);
}

void test_firmware_source_trusted(void)
{
  TEST_ASSERT_TRUE(url_is_trusted_firmware_source("https://display.lishuyu.app/firmware/1.7.1.bin", BASE));
  TEST_ASSERT_TRUE(url_is_trusted_firmware_source("https://DISPLAY.lishuyu.app:443/fw.bin", BASE));
  TEST_ASSERT_TRUE(url_is_trusted_firmware_source("https://display.lishuyu.app/fw.bin", "https://display.lishuyu.app/api/"));
}

void test_firmware_source_untrusted(void)
{
  // plain http: anyone on path can swap the image
  TEST_ASSERT_FALSE(url_is_trusted_firmware_source("http://display.lishuyu.app/fw.bin", BASE));
  // other hosts, including look-alikes
  TEST_ASSERT_FALSE(url_is_trusted_firmware_source("https://evil.example/fw.bin", BASE));
  TEST_ASSERT_FALSE(url_is_trusted_firmware_source("https://display.lishuyu.app.evil.example/fw.bin", BASE));
  TEST_ASSERT_FALSE(url_is_trusted_firmware_source("https://evil-display.lishuyu.app/fw.bin", BASE));
  TEST_ASSERT_FALSE(url_is_trusted_firmware_source("https://lishuyu.app/fw.bin", BASE));
  TEST_ASSERT_FALSE(url_is_trusted_firmware_source("https://display.lishuyu.app@evil.example/fw.bin", BASE));
  TEST_ASSERT_FALSE(url_is_trusted_firmware_source("", BASE));
  TEST_ASSERT_FALSE(url_is_trusted_firmware_source(nullptr, BASE));
  TEST_ASSERT_FALSE(url_is_trusted_firmware_source("https://display.lishuyu.app/fw.bin", ""));
}

int main(int argc, char **argv)
{
  UNITY_BEGIN();
  RUN_TEST(test_extract_host_basic);
  RUN_TEST(test_extract_host_rejects_malformed);
  RUN_TEST(test_extract_host_too_long);
  RUN_TEST(test_firmware_source_trusted);
  RUN_TEST(test_firmware_source_untrusted);
  return UNITY_END();
}
