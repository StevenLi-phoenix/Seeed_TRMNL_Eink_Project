#include <Arduino.h>
#include <WiFi.h>
#include <bl.h>
#include <trmnl_log.h>
#include <types.h>
#include <ArduinoLog.h>
#include <WifiCaptive.h>
#include <pins.h>
#include <config.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <display.h>
#include <stdlib.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <Preferences.h>
#include <cstdint>
#include "png.h"
#include <bmp.h>
#include <Update.h>
#include <math.h>
#include <filesystem.h>
#include "trmnl_log.h"
#include <stored_logs.h>
#include <button.h>
#include "api-client/submit_log.h"
#include <api-client/setup.h>
#include <special_function.h>
#include <api_response_parsing.h>
#include "logging_parcers.h"
#include <SPIFFS.h>
#include "http_client.h"
#include <api-client/display.h>
#include "driver/gpio.h"
#include <nvs.h>
#include <serialize_log.h>
#include <preferences_persistence.h>
#include "logo_small.h"
#include "loading.h"
#include <wifi-helpers.h>
#include <battery_estimate.h>
#include <boot_guard.h>
#include <battery_guard.h>
#include <esp_system.h>

bool pref_clear = false;
String new_filename = "";
ApiDisplayResult apiDisplayResult;
uint8_t *buffer = nullptr;
char filename[1024];      // image URL
char binUrl[1024];        // update URL
char message_buffer[128]; // message to show on the screen
uint32_t time_since_sleep;
image_err_e png_res = PNG_DECODE_ERR;
bmp_err_e bmp_res = BMP_NOT_BMP;
static float vBatt;
bool status = false;          // need to download a new image
bool update_firmware = false; // need to download a new firmware
bool reset_firmware = false;  // need to reset credentials
bool send_log = false;        // need to send logs
bool double_click = false;
bool log_retry = false;                                              // need to log connection retry
esp_sleep_wakeup_cause_t wakeup_reason = ESP_SLEEP_WAKEUP_UNDEFINED; // wake-up reason
MSG current_msg = NONE;
SPECIAL_FUNCTION special_function = SF_NONE;
RTC_DATA_ATTR uint8_t need_to_refresh_display = 1;
// Boot-loop guard state. RTC_NOINIT (not RTC_DATA) so it survives panic /
// WDT / brownout / software resets as well as deep sleep; magic + check word
// detect the random content after power-on or the first boot after flashing.
RTC_NOINIT_ATTR BootGuardState boot_guard_state;
static BootGuardDecision boot_guard_decision = {};
static esp_reset_reason_t boot_reset_reason = ESP_RST_UNKNOWN;
static bool ota_succeeded = false; // set by checkAndPerformFirmwareUpdate()

Preferences preferences;
PreferencesPersistence preferencesPersistence(preferences);
StoredLogs storedLogs(LOG_MAX_NOTES_NUMBER / 2, LOG_MAX_NOTES_NUMBER / 2, PREFERENCES_LOG_KEY, PREFERENCES_LOG_BUFFER_HEAD_KEY, preferencesPersistence);

static https_request_err_e downloadAndShow(); // download and show the image
static uint32_t downloadStream(WiFiClient *stream, int content_size, uint8_t *buffer, int buffer_size = 0);
static https_request_err_e handleApiDisplayResponse(ApiDisplayResponse &apiResponse);
static void getDeviceCredentials();                  // receiveing API key and Friendly ID
static bool performApiSetup();     // perform API setup call and return success
static void downloadSetupImage();                    // download and display setup image
static void resetDeviceCredentials(void);            // reset device credentials API key, Friendly ID, Wi-Fi SSID and password
static void checkAndPerformFirmwareUpdate(void);     // OTA update
static void goToSleep(void);                         // sleep preparing
static void deepSleepNow(uint32_t seconds, bool backoff); // arm wake sources + deep sleep (all paths)
static void guardDeepSleep(uint32_t seconds, bool backoff); // minimal sleep before WiFi/SPIFFS are up
static void restartIntentionally(const char *why);   // ESP.restart() the boot-loop guard won't count
static void bootGuardOnBoot(void);                   // count this boot, decide normal/backoff
static void bootGuardReport(void);                   // server-visible line after abnormal resets
static void batteryGuardCheck(void);                 // deep-discharge guard (no WiFi while critical)
static bool setClock(void);                          // clock synchronization
static float readBatteryVoltage(void);               // battery voltage reading
static void submitOrSaveLogString(const char *log_buffer, size_t size); // log sending
static void submitStoredLogs(void);
static void writeSpecialFunction(SPECIAL_FUNCTION function);
static void writeImageToFile(const char *name, uint8_t *in_buffer, size_t size);
static void showMessageWithLogo(MSG message_type);
static void showMessageWithLogo(MSG message_type, String friendly_id, bool id, const char *fw_version, String message);
static void showMessageWithLogo(MSG message_type, const ApiSetupResponse &apiResponse);
static void showRecoveryScreen(int battery_pct, float eta_days); // WiFi recovery-mode screen
static void wifiErrorDeepSleep();
static uint8_t *storedLogoOrDefault(int iType);
static bool saveCurrentFileName(String &name);
static bool checkCurrentFileName(String &newName);
static DeviceStatusStamp getDeviceStatusStamp();
void log_nvs_usage();


void wait_for_serial()
{
#ifdef WAIT_FOR_SERIAL
  for (int i = 10; i > 0 && !Serial; i--)
  {
    Log_info("## Waiting for serial.. %d", i);
    delay(1000);
  }
#endif
}

/**
 * @brief Function to init business logic module
 * @param none
 * @return none
 */
void bl_init(void)
{

  Serial.begin(115200);
  Log.begin(LOG_LEVEL_VERBOSE, &Serial);
  Log_info("BL init success");
  // Count this boot FIRST, before anything that could crash (display, SPIFFS,
  // WiFi, HTTP). Every intentional deep sleep resets the counter.
  bootGuardOnBoot();
  pins_init();
  vBatt = readBatteryVoltage(); // Read the battery voltage BEFORE WiFi is turned on

#if defined(BOARD_SEEED_XIAO_ESP32C3)
  delay(2000);

  if (digitalRead(PIN_INTERRUPT) == LOW) {
    Log_info("Boot button pressed during startup, resetting WiFi credentials...");
    WifiCaptivePortal.resetSettings();
    Log_info("WiFi credentials reset completed");
  }
#endif

  wakeup_reason = esp_sleep_get_wakeup_cause();

  if (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO || wakeup_reason == ESP_SLEEP_WAKEUP_EXT0 || wakeup_reason == ESP_SLEEP_WAKEUP_EXT1)
  {
    Log_info("GPIO wakeup detected (%d)", wakeup_reason);
    auto button = read_button_presses();
    wait_for_serial();
    Log_info("GPIO wakeup (%d) -> button was read (%s)", wakeup_reason, ButtonPressResultNames[button]);
    switch (button)
    {
    case LongPress:
      Log_info("WiFi reset");
      WifiCaptivePortal.resetSettings();
      break;
    case DoubleClick:
      double_click = true;
      break;
    case NoAction:
      break;
    case SoftReset:
      resetDeviceCredentials();
    }
    Log_info("button handling end");
  }
  else
  {
    wait_for_serial();
    Log_info("Non-GPIO wakeup (%d) -> didn't read buttons", wakeup_reason);
  }

  Log_info("preferences start");
  bool res = preferences.begin("data", false);
  if (res)
  {
    Log_info("preferences init success (%d free entries)", preferences.freeEntries());
    if (pref_clear)
    {
      res = preferences.clear(); // if needed to clear the saved data
      if (res)
        Log_info("preferences cleared success");
      else
        Log_fatal("preferences clearing error");
    }
  }
  else
  {
    Log_fatal("preferences init failed");
    ESP.restart();
  }
  Log_info("preferences end");

  // Battery protection, before the display, SPIFFS and WiFi come up:
  // 1) boot-loop guard: N boots in a row never reached deep sleep → back off
  //    without WiFi (the Sep 2026 incident: ~14k wakes/day drained the LiPo);
  // 2) deep-discharge guard: battery below BATTERY_CRITICAL_V → no WiFi.
  bootGuardReport();
  if (boot_guard_decision.action == BOOT_GUARD_BACKOFF)
  {
    Log_info("Boot-loop guard: skipping WiFi, backoff deep sleep %lu s", (unsigned long)boot_guard_decision.sleep_seconds);
    guardDeepSleep(boot_guard_decision.sleep_seconds, true); // does not return
  }
  batteryGuardCheck(); // does not return while the battery is critical

  if (double_click)
  { // special function reading
    if (preferences.isKey(PREFERENCES_SF_KEY))
    {
      Log.info("%s [%d]: SF saved. Reading...\r\n", __FILE__, __LINE__);
      special_function = (SPECIAL_FUNCTION)preferences.getUInt(PREFERENCES_SF_KEY, 0);
      Log.info("%s [%d]: Read special function - %d\r\n", __FILE__, __LINE__, special_function);
      switch (special_function)
      {
      case SF_IDENTIFY:
      {
        Log.info("%s [%d]: Identify special function...It will be handled with API ping...\r\n", __FILE__, __LINE__);
      }
      break;
      case SF_SLEEP:
      {
        Log.info("%s [%d]: Sleep special function...\r\n", __FILE__, __LINE__);
        // still in progress
      }
      break;
      case SF_ADD_WIFI:
      {
        Log.info("%s [%d]: Add WiFi function...\r\n", __FILE__, __LINE__);
        WifiCaptivePortal.startPortal();
      }
      break;
      case SF_RESTART_PLAYLIST:
      {
        Log.info("%s [%d]: Restart Playlist special function...It will be handled with API ping...\r\n", __FILE__, __LINE__);
      }
      break;
      case SF_REWIND:
      {
        Log.info("%s [%d]: Rewind special function...\r\n", __FILE__, __LINE__);
      }
      break;
      case SF_SEND_TO_ME:
      {
        Log.info("%s [%d]: Send to me special function...It will be handled with API ping...\r\n", __FILE__, __LINE__);
      }
      break;
      case SF_GUEST_MODE:
      {
        Log.info("%s [%d]: Guest Mode special function...It will be handled with API ping...\r\n", __FILE__, __LINE__);
      }
      break;
      default:
        break;
      }
    }
    else
    {
      Log_error("SF not saved");
    }
  }
  // EPD init
  // EPD clear
  Log.info("%s [%d]: Display init\r\n", __FILE__, __LINE__);
  display_init();

  // Mount SPIFFS
  filesystem_init();

  if (wakeup_reason != ESP_SLEEP_WAKEUP_TIMER)
  {
    Log.info("%s [%d]: Display TRMNL logo start\r\n", __FILE__, __LINE__);

  
    display_show_image(storedLogoOrDefault(1), DEFAULT_IMAGE_SIZE, false);


    need_to_refresh_display = 1;
    preferences.putBool(PREFERENCES_DEVICE_REGISTERED_KEY, false);
    Log.info("%s [%d]: Display TRMNL logo end\r\n", __FILE__, __LINE__);
    preferences.putString(PREFERENCES_FILENAME_KEY, "");
  }

  Log_info("Firmware version %s", FW_VERSION_STRING);
  Log_info("Arduino version %d.%d.%d", ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH);
  Log_info("ESP-IDF version %d.%d.%d", ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH);
  list_files();
  log_nvs_usage();

  WiFi.mode(WIFI_STA); // explicitly set mode, esp defaults to STA+AP

// uncomment this to hardcode WiFi credentials (useful for testing wifi errors, etc.)
// #define HARDCODED_WIFI
#ifdef HARDCODED_WIFI
  WifiCredentials hardcodedCreds = {.ssid = "ssid-goes-here", .pswd = "password-goes-here"};
  Log_info("Hardcoded WiFi: connecting to SSID '%s'", hardcodedCreds.ssid.c_str());
  auto connectResult = WifiCaptivePortal.connect(hardcodedCreds);
  Log_info("Hardcoded WiFi: connect result '%s'", wifiStatusStr(connectResult));
// goToSleep();
#else

  if (WifiCaptivePortal.isSaved())
  {
    // WiFi saved, connection
    Log.info("%s [%d]: WiFi saved\r\n", __FILE__, __LINE__);
    int connection_res = WifiCaptivePortal.autoConnect();

    Log.info("%s [%d]: Connection result: %d, WiFI Status: %d\r\n", __FILE__, __LINE__, connection_res, WiFi.status());

    // Check if connected
    if (connection_res)
    {
      String ip = String(WiFi.localIP());
      Log.info("%s [%d]:wifi_connection [DEBUG]: Connected: %s\r\n", __FILE__, __LINE__, ip.c_str());
      preferences.putInt(PREFERENCES_CONNECT_WIFI_RETRY_COUNT, 1);
      // ensure the next recovery episode redraws its screen from scratch
      preferences.remove(PREFERENCES_RECOVERY_DRAWN_PCT);
    }
    else
    {
      Log_fatal_submit("Connection failed! WL Status: %d", WiFi.status());

      uint8_t retry_count = preferences.getInt(PREFERENCES_CONNECT_WIFI_RETRY_COUNT, 1);

      if (retry_count >= WIFI_RECOVERY_PORTAL_RETRY_THRESHOLD)
      {
        // Recovery mode: every saved network kept failing through the
        // 60/180/300s fast retries. Open a time-limited hotspot each wake so
        // new credentials can be submitted from a phone — nothing is wiped.
        int battery_pct = battery_percent(vBatt);
        float eta_days = battery_recovery_eta_days(battery_pct, BATTERY_RECOVERY_FULL_DAYS);

        int last_drawn_pct = preferences.getInt(PREFERENCES_RECOVERY_DRAWN_PCT, -100);
        if (abs(battery_pct - last_drawn_pct) >= WIFI_RECOVERY_REDRAW_PCT_DELTA)
        {
          // e-ink keeps the image through deep sleep; skip the full-refresh
          // flash unless the battery gauge actually moved
          showRecoveryScreen(battery_pct, eta_days);
          current_msg = WIFI_RECOVERY;
          preferences.putInt(PREFERENCES_RECOVERY_DRAWN_PCT, battery_pct);
        }

        Log_info("Opening recovery portal for %lu ms (wifi retry %d, battery %d%%)",
                 (unsigned long)WIFI_RECOVERY_PORTAL_TIMEOUT_MS, retry_count, battery_pct);
        WifiCaptivePortal.setResetSettingsCallback(resetDeviceCredentials);
        if (WifiCaptivePortal.startPortal(WIFI_RECOVERY_PORTAL_TIMEOUT_MS))
        {
          Log_info("Recovery portal: new credentials accepted, back online");
          preferences.putInt(PREFERENCES_CONNECT_WIFI_RETRY_COUNT, 1);
          preferences.remove(PREFERENCES_RECOVERY_DRAWN_PCT);
          // fall through to the normal flow: clock sync, /api/display fetch
        }
        else
        {
          Log_info("Recovery portal timed out with no new credentials");
          // keep the existing backoff cycle (900s per wake at this point)
          wifiErrorDeepSleep();
        }
      }
      else
      {
        if (current_msg != WIFI_FAILED)
        {
          showMessageWithLogo(WIFI_FAILED);
          current_msg = WIFI_FAILED;
        }

        // Go to deep sleep
        wifiErrorDeepSleep();
      }
    }
  }
  else
  {
    // WiFi credentials are not saved - start captive portal
    Log.info("%s [%d]: WiFi NOT saved\r\n", __FILE__, __LINE__);

    Log_info("FW version %s", FW_VERSION_STRING);

    showMessageWithLogo(WIFI_CONNECT, "", false, FW_VERSION_STRING, "");
    WifiCaptivePortal.setResetSettingsCallback(resetDeviceCredentials);
    res = WifiCaptivePortal.startPortal();
    if (!res)
    {
      WiFi.disconnect(true);

      showMessageWithLogo(WIFI_FAILED);

      Log_error("Failed to connect or hit timeout");

      // Go to deep sleep
      wifiErrorDeepSleep();
    }
    Log.info("%s [%d]: WiFi connected\r\n", __FILE__, __LINE__);
    preferences.putInt(PREFERENCES_CONNECT_WIFI_RETRY_COUNT, 1);
  }

#endif

  // clock synchronization
  if (setClock())
  {
    time_since_sleep = preferences.getUInt(PREFERENCES_LAST_SLEEP_TIME, 0);
    time_since_sleep = time_since_sleep ? getTime() - time_since_sleep : 0; // may be can be used even if no sync
  }
  else
  {
    time_since_sleep = 0;
    Log.info("%s [%d]: Time wasn't synced.\r\n", __FILE__, __LINE__);
  }

  Log.info("%s [%d]: Time since last sleep: %d\r\n", __FILE__, __LINE__, time_since_sleep);

  if (!preferences.isKey(PREFERENCES_API_KEY) || !preferences.isKey(PREFERENCES_FRIENDLY_ID))
  {
    Log.info("%s [%d]: API key or friendly ID not saved\r\n", __FILE__, __LINE__);
    // lets get the api key and friendly ID
    getDeviceCredentials();
  }
  else
  {
    Log.info("%s [%d]: API key and friendly ID saved\r\n", __FILE__, __LINE__);
  }

  submitStoredLogs();

  log_retry = true;

  // OTA checking, image checking and drawing
  https_request_err_e request_result = downloadAndShow();
  Log.info("%s [%d]: request result - %d\r\n", __FILE__, __LINE__, request_result);

  if (request_result == HTTPS_IMAGE_FILE_TOO_BIG)
  {
    showMessageWithLogo(MSG_TOO_BIG);
  }

  if (!preferences.isKey(PREFERENCES_CONNECT_API_RETRY_COUNT))
  {
    preferences.putInt(PREFERENCES_CONNECT_API_RETRY_COUNT, 1);
  }

  if (request_result != HTTPS_SUCCESS && request_result != HTTPS_NO_ERR && request_result != HTTPS_NO_REGISTER && request_result != HTTPS_RESET && request_result != HTTPS_PLUGIN_NOT_ATTACHED)
  {
    uint8_t retries = preferences.getInt(PREFERENCES_CONNECT_API_RETRY_COUNT);

    switch (retries)
    {
    case 1:
      Log.info("%s [%d]: retry: %d - time to sleep: %d\r\n", __FILE__, __LINE__, retries, API_CONNECT_RETRY_TIME::API_FIRST_RETRY);
      res = preferences.putUInt(PREFERENCES_SLEEP_TIME_KEY, API_CONNECT_RETRY_TIME::API_FIRST_RETRY);
      preferences.putInt(PREFERENCES_CONNECT_API_RETRY_COUNT, ++retries);
      display_sleep();
      goToSleep();
      break;

    case 2:
      Log.info("%s [%d]: retry:%d - time to sleep: %d\r\n", __FILE__, __LINE__, retries, API_CONNECT_RETRY_TIME::API_SECOND_RETRY);
      res = preferences.putUInt(PREFERENCES_SLEEP_TIME_KEY, API_CONNECT_RETRY_TIME::API_SECOND_RETRY);
      preferences.putInt(PREFERENCES_CONNECT_API_RETRY_COUNT, ++retries);
      display_sleep();
      goToSleep();
      break;

    case 3:
      Log.info("%s [%d]: retry:%d - time to sleep: %d\r\n", __FILE__, __LINE__, retries, API_CONNECT_RETRY_TIME::API_THIRD_RETRY);
      res = preferences.putUInt(PREFERENCES_SLEEP_TIME_KEY, API_CONNECT_RETRY_TIME::API_THIRD_RETRY);
      preferences.putInt(PREFERENCES_CONNECT_API_RETRY_COUNT, ++retries);
      display_sleep();
      goToSleep();
      break;

    default:
      Log.info("%s [%d]: Max retries done. Time to sleep: %d\r\n", __FILE__, __LINE__, SLEEP_TIME_TO_SLEEP);
      preferences.putUInt(PREFERENCES_SLEEP_TIME_KEY, SLEEP_TIME_TO_SLEEP);
      preferences.putInt(PREFERENCES_CONNECT_API_RETRY_COUNT, ++retries);
      break;
    }
  }

  else
  {
    Log_info("Connection done successfully. Retries counter reset.");
    preferences.putInt(PREFERENCES_CONNECT_API_RETRY_COUNT, 1);
  }

  submitStoredLogs();

  if (request_result == HTTPS_NO_REGISTER && need_to_refresh_display == 1)
  {
    // show the image
    String friendly_id = preferences.getString(PREFERENCES_FRIENDLY_ID, PREFERENCES_FRIENDLY_ID_DEFAULT);
    showMessageWithLogo(FRIENDLY_ID, friendly_id, true, "", String(message_buffer));
    need_to_refresh_display = 0;
  }

  // reset checking
  if (request_result == HTTPS_RESET)
  {
    Log.info("%s [%d]: Device reseting...\r\n", __FILE__, __LINE__);
    resetDeviceCredentials();
  }

  // OTA update checking
  if (update_firmware)
  {
    checkAndPerformFirmwareUpdate();
  }

  // error handling
  switch (request_result)
  {
  case HTTPS_REQUEST_FAILED:
  {
    if (WiFi.RSSI() > WIFI_CONNECTION_RSSI)
    {
      showMessageWithLogo(API_ERROR);
    }
    else
    {
      showMessageWithLogo(WIFI_WEAK);
    }
  }
  break;
  case HTTPS_RESPONSE_CODE_INVALID:
  {
    showMessageWithLogo(WIFI_INTERNAL_ERROR);
  }
  break;
  case HTTPS_UNABLE_TO_CONNECT:
  {
    if (WiFi.RSSI() > WIFI_CONNECTION_RSSI)
    {
      showMessageWithLogo(API_ERROR);
    }
    else
    {
      showMessageWithLogo(WIFI_WEAK);
    }
  }
  break;
  case HTTPS_WRONG_IMAGE_FORMAT:
  {
    showMessageWithLogo(MSG_FORMAT_ERROR);
  }
  break;
  case HTTPS_WRONG_IMAGE_SIZE:
  {
    if (WiFi.RSSI() > WIFI_CONNECTION_RSSI)
    {
      showMessageWithLogo(API_SIZE_ERROR);
    }
    else
    {
      showMessageWithLogo(WIFI_WEAK);
    }
  }
  break;
  case HTTPS_CLIENT_FAILED:
  {
    showMessageWithLogo(WIFI_INTERNAL_ERROR);
  }
  break;
  case HTTPS_PLUGIN_NOT_ATTACHED:
  {
    if (preferences.getInt(PREFERENCES_SLEEP_TIME_KEY, 0) != SLEEP_TIME_WHILE_PLUGIN_NOT_ATTACHED)
    {
      Log.info("%s [%d]: write new refresh rate: %d\r\n", __FILE__, __LINE__, SLEEP_TIME_WHILE_PLUGIN_NOT_ATTACHED);
      preferences.putUInt(PREFERENCES_SLEEP_TIME_KEY, SLEEP_TIME_WHILE_PLUGIN_NOT_ATTACHED);
      Log.info("%s [%d]: written new refresh rate: %d\r\n", __FILE__, __LINE__, SLEEP_TIME_WHILE_PLUGIN_NOT_ATTACHED);
    }
  }
  break;
  default:
    break;
  }

  // display go to sleep
  display_sleep();
  if (!update_firmware)
    goToSleep();
  else if (ota_succeeded)
    restartIntentionally("OTA update applied"); // boot into the new image
  else
  {
    // keep upstream behavior (restart and retry), but this restart is counted
    // by the boot-loop guard so a failing OTA can't loop at full speed
    Log_error("OTA update failed; restarting (counted by boot-loop guard)");
    ESP.restart();
  }
}

/**
 * @brief Function to process business logic module
 * @param none
 * @return none
 */
void bl_process(void)
{
}

ApiDisplayInputs loadApiDisplayInputs(Preferences &preferences)
{
  ApiDisplayInputs inputs;

  inputs.baseUrl = preferences.getString(PREFERENCES_API_URL, API_BASE_URL);

  if (preferences.isKey(PREFERENCES_API_KEY))
  {
    inputs.apiKey = preferences.getString(PREFERENCES_API_KEY, PREFERENCES_API_KEY_DEFAULT);
    Log.info("%s [%d]: %s key exists. Value - %s\r\n", __FILE__, __LINE__, PREFERENCES_API_KEY, inputs.apiKey.c_str());
  }
  else
  {
    Log.info("%s [%d]: %s key not exists.\r\n", __FILE__, __LINE__, PREFERENCES_API_KEY);
  }

  if (preferences.isKey(PREFERENCES_FRIENDLY_ID))
  {
    inputs.friendlyId = preferences.getString(PREFERENCES_FRIENDLY_ID, PREFERENCES_FRIENDLY_ID_DEFAULT);
    Log.info("%s [%d]: %s key exists. Value - %s\r\n", __FILE__, __LINE__, PREFERENCES_FRIENDLY_ID, inputs.friendlyId);
  }
  else
  {
    Log.info("%s [%d]: %s key not exists.\r\n", __FILE__, __LINE__, PREFERENCES_FRIENDLY_ID);
  }

  inputs.refreshRate = SLEEP_TIME_TO_SLEEP;

  if (preferences.isKey(PREFERENCES_SLEEP_TIME_KEY))
  {
    inputs.refreshRate = preferences.getUInt(PREFERENCES_SLEEP_TIME_KEY, SLEEP_TIME_TO_SLEEP);
    Log.info("%s [%d]: %s key exists. Value - %d\r\n", __FILE__, __LINE__, PREFERENCES_SLEEP_TIME_KEY, inputs.refreshRate);
  }
  else
  {
    Log.info("%s [%d]: %s key not exists.\r\n", __FILE__, __LINE__, PREFERENCES_SLEEP_TIME_KEY);
  }

  inputs.macAddress = WiFi.macAddress();

  inputs.batteryVoltage = vBatt; //readBatteryVoltage();

  inputs.firmwareVersion = String(FW_VERSION_STRING);

  inputs.rssi = WiFi.RSSI();
  inputs.displayWidth = display_width();
  inputs.displayHeight = display_height();
  inputs.model = DEVICE_MODEL;
  inputs.specialFunction = special_function;

  return inputs;
}

/**
 * @brief Function to ping server and download and show the image if all is OK
 * @param url Server URL address
 * @return https_request_err_e error code
 */
static https_request_err_e downloadAndShow()
{
  IPAddress serverIP;
  String apiHostname = preferences.getString(PREFERENCES_API_URL, API_BASE_URL);
  apiHostname.replace("https://", "");
  apiHostname.replace("http://", "");
  apiHostname.replace("/", "");

  int colon = apiHostname.indexOf(':');
  if (colon != -1) {
    apiHostname = apiHostname.substring(0, colon);
  }

  for (int attempt = 1; attempt <= 5; ++attempt)
  {
    if (WiFi.hostByName(apiHostname.c_str(), serverIP) == 1)
    {
      Log.info("%s [%d]: Hostname resolved to %s on attempt %d\r\n", __FILE__, __LINE__, serverIP.toString().c_str(), attempt);
      break;
    }
    else
    {
      Log_error("Failed to resolve hostname on attempt %d", attempt);
      delay(2000);
    }
  }

  auto apiDisplayInputs = loadApiDisplayInputs(preferences);

  apiDisplayResult = fetchApiDisplay(apiDisplayInputs);

  if (apiDisplayResult.error != HTTPS_NO_ERR)
  {
    Log_error_submit("Error fetching API display: %d, detail: %s", apiDisplayResult.error, apiDisplayResult.error_detail.c_str());
    return apiDisplayResult.error;
  }

  https_request_err_e result = handleApiDisplayResponse(apiDisplayResult.response);

  auto withHttpResult = withHttp(
      filename,
      [&](HTTPClient *httpsp, HttpError error) -> https_request_err_e
      {
        if (error != HttpError::HTTPCLIENT_SUCCESS)
        {

          return HTTPS_UNABLE_TO_CONNECT;
        }

        HTTPClient &https = *httpsp;

        https.setTimeout(15000);
        https.setConnectTimeout(15000);

        if (status && !update_firmware && !reset_firmware)
        {
          status = false;

          // The timeout will be zero if no value was returned, and in that case we just use the default timeout.
          // Otherwise, we set the requested timeout.
          uint32_t requestedTimeout = apiDisplayResult.response.image_url_timeout;
          if (requestedTimeout > 0)
          {
            // Convert from seconds to milliseconds.
            // A uint32_t should be large enough not to worry about overflow for any reasonable timeout.
            requestedTimeout *= MS_TO_S_FACTOR;
            if (requestedTimeout > UINT16_MAX)
            {
              // To avoid surprising behaviour if the server returned a timeout of more than 65 seconds
              // we will send a log message back to the server and truncate the timeout to the maximum.
              Log_info_submit("Requested image URL timeout too large (%d ms). Using maximum of %d ms.", requestedTimeout, UINT16_MAX);
              https.setTimeout(UINT16_MAX);
            }
            else
            {
              https.setTimeout(uint16_t(requestedTimeout));
            }
          }

          const char *headers[] = {"Content-Type"};
          https.collectHeaders(headers, 1);
          // HTTP/1.0 rules out chunked transfer encoding: the body is read raw
          // through getStreamPtr(), which does not strip chunk framing.
          https.useHTTP10(true);
          Log_info("GET...");
          Log_info("RSSI: %d", WiFi.RSSI());
          // start connection and send HTTP header
          int httpCode = https.GET();
          int content_size = https.getSize();
          uint8_t *buffer_old = nullptr; // previous frame, restored into the EPD "old" plane for partial refresh
          int file_size_old = 0;

          // httpCode will be negative on error
          if (httpCode < 0)
          {
            Log_error_submit("[HTTPS] GET... failed, error: %d (%s)", httpCode, https.errorToString(httpCode).c_str());

            return HTTPS_REQUEST_FAILED;
          }

          // HTTP header has been send and Server response header has been handled
          Log.error("%s [%d]: [HTTPS] GET... code: %d\r\n", __FILE__, __LINE__, httpCode);
          Log.info("%s [%d]: RSSI: %d\r\n", __FILE__, __LINE__, WiFi.RSSI());
          // file found at server
          if (httpCode != HTTP_CODE_OK && httpCode != HTTP_CODE_MOVED_PERMANENTLY)
          {
            Log_error_submit("[HTTPS] GET... failed, code: %d (%s)", httpCode, https.errorToString(httpCode).c_str());
            return HTTPS_REQUEST_FAILED;
          }
          Log.info("%s [%d]: Content size: %d\r\n", __FILE__, __LINE__, https.getSize());

          uint32_t counter = 0;
          if (content_size > MAX_IMAGE_SIZE)
          {
            //Log_error_submit("Receiving failed; file size too big");
            Log.info("%s [%d]: Receiving failed; file size too big: %d\r\n", __FILE__, __LINE__, content_size);
            result = HTTPS_IMAGE_FILE_TOO_BIG;
            return HTTPS_REQUEST_FAILED;
          }
          WiFiClient *stream = https.getStreamPtr();
          Log.info("%s [%d]: RSSI: %d\r\n", __FILE__, __LINE__, WiFi.RSSI());
          Log.info("%s [%d]: Stream timeout: %d\r\n", __FILE__, __LINE__, stream->getTimeout());

          Log.info("%s [%d]: Stream available (may show as 0 and that's okay): %d\r\n", __FILE__, __LINE__, stream->available());

          bool isPNG = https.header("Content-Type") == "image/png";

          Log.info("%s [%d]: Starting a download at: %d\r\n", __FILE__, __LINE__, getTime());
          heap_caps_check_integrity_all(true);
          // No Content-Length (getSize() == -1): read until the server closes
          // the connection, into a buffer sized for the largest image we accept.
          bool bUnknownSize = content_size <= 0;
          int buffer_size = bUnknownSize ? MAX_IMAGE_SIZE : content_size;
          buffer = (uint8_t *)malloc(buffer_size);
          if (buffer == nullptr)
          {
            Log_error_submit("Image buffer allocation failed (%d bytes)", buffer_size);
            return HTTPS_REQUEST_FAILED;
          }

          counter = downloadStream(stream, content_size, buffer, buffer_size);
          if (bUnknownSize)
          {
            content_size = counter;
          }

          if (counter >= 2 && buffer[0] == 'B' && buffer[1] == 'M')
          {
            isPNG = false;
            Log.info("BMP file detected");
          }

          if (counter != content_size)
          {

            Log_error_submit("Receiving failed. Read: %d", counter);

            // display_show_msg(const_cast<uint8_t *>(default_icon), API_SIZE_ERROR);

            return HTTPS_WRONG_IMAGE_SIZE;
          }
          WiFi.disconnect(true); // no need for WiFi, save power starting here
          Log.info("%s [%d]: Received successfully; WiFi off; WiFi off\r\n", __FILE__, __LINE__);
          bool bmp_rename = false;

          if (filesystem_file_exists("/current.bmp") || filesystem_file_exists("/current.png"))
          {
            filesystem_file_delete("/last.bmp");
            filesystem_file_delete("/last.png");
            filesystem_file_rename("/current.png", "/last.png");
            filesystem_file_rename("/current.bmp", "/last.bmp");
            // Read the previous frame back so the EPD's "old" plane (cleared by
            // the panel deep-sleep command) can be restored, making a partial
            // refresh diff reliable. BMP only - that is what our server sends.
            if (!display_saved_frame_valid())
            {
              // Something else (loading logo, a message, a reset) was drawn
              // after /last.bmp was shown, so it no longer matches the panel.
              Log_info("Panel no longer shows /last.bmp; skipping old-frame restore");
            }
            else if (filesystem_file_exists("/last.bmp"))
            {
              buffer_old = display_read_file("/last.bmp", &file_size_old);
              if (buffer_old != nullptr)
              {
                Log_info("Read /last.bmp as old frame for partial refresh, size = %d", file_size_old);
              }
              else
              {
                Log_info("Failed to read /last.bmp; partial refresh will degrade to fast");
              }
            }
            else if (filesystem_file_exists("/last.png"))
            {
              Log_info("Old frame is PNG; partial refresh restore supports BMP only");
            }
          }

          bool image_reverse = false;
          if (isPNG)
          {
            writeImageToFile("/current.png", buffer, content_size);
            Log.info("%s [%d]: Decoding png\r\n", __FILE__, __LINE__);
            display_show_image(buffer, content_size, true, buffer_old, file_size_old);
//            delay(100);
//            free(buffer);
//            buffer = nullptr;
//            png_res = decodePNG("/current.png", decodedPng);
            png_res = PNG_NO_ERR; // DEBUG
          }
          else
          {
            bmp_res = parseBMPHeader(buffer, image_reverse);
            Log.info("%s [%d]: BMP Parsing result: %d\r\n", __FILE__, __LINE__, bmp_res);
          }
          Serial.println();
          String error = "";
          uint8_t *imagePointer = buffer;
//          uint8_t *imagePointer = (decodedPng == nullptr) ? buffer : decodedPng;
          bool lastImageExists = filesystem_file_exists("/last.bmp") || filesystem_file_exists("/last.png");

          switch (png_res)
          {
          case PNG_NO_ERR:
          {

           // Log.info("Free heap at before display - %d", ESP.getMaxAllocHeap());
           // display_show_image(imagePointer, image_reverse, isPNG);

            // Using filename from API response
            new_filename = apiDisplayResult.response.filename;

            // Print the extracted string
            Log.info("%s [%d]: New filename - %s\r\n", __FILE__, __LINE__, new_filename.c_str());

            bool res = saveCurrentFileName(new_filename);
            if (res)
              Log.info("%s [%d]: New filename saved\r\n", __FILE__, __LINE__);
            else
              Log.error("%s [%d]: New image name saving error!", __FILE__, __LINE__);

            if (result != HTTPS_PLUGIN_NOT_ATTACHED)
              result = HTTPS_SUCCESS;
          }
          break;
          case PNG_WRONG_FORMAT:
          {
            error = "Wrong image format. Did not pass signature check";
          }
          break;
          case PNG_BAD_SIZE:
          {
            error = "IMAGE width, height or size are invalid";
          }
          break;
          case PNG_DECODE_ERR:
          {
            error = "could not decode png image";
          }
          break;
          case PNG_MALLOC_FAILED:
          {
            error = "could not allocate memory for png image decoder";
          }
          break;
          default:
            break;
          }

          switch (bmp_res)
          {
          case BMP_NO_ERR:
          {
            Log.info("Free heap at before display - %d", ESP.getMaxAllocHeap());
            display_show_image(buffer, content_size, true, buffer_old, file_size_old);
            // Save AFTER displaying: display_show_image() embeds the refresh
            // timestamp into the buffer (returned in raw bottom-up orientation),
            // and /current.bmp must match the panel content for the next
            // partial-refresh old-frame restore to be artifact-free.
            if (!filesystem_file_exists("/current.png"))
            {
              writeImageToFile("/current.bmp", buffer, content_size);
              display_set_saved_frame_valid(true);
            }

            // Using filename from API response
            new_filename = apiDisplayResult.response.filename;

            // Print the extracted string
            Log.info("%s [%d]: New filename - %s\r\n", __FILE__, __LINE__, new_filename.c_str());

            bool res = saveCurrentFileName(new_filename);
            if (res)
              Log.info("%s [%d]: New filename saved\r\n", __FILE__, __LINE__);
            else
              Log.error("%s [%d]: New image name saving error!", __FILE__, __LINE__);

            if (result != HTTPS_PLUGIN_NOT_ATTACHED)
              result = HTTPS_SUCCESS;
          }
          break;
          case BMP_NOT_BMP:
          {
            error = "First two header bytes are invalid!";
          }
          break;
          case BMP_BAD_SIZE:
          {
            error = "BMP width, height or size are invalid";
          }
          break;
          case BMP_COLOR_SCHEME_FAILED:
          {
            error = "BMP color scheme is invalid";
          }
          break;
          case BMP_INVALID_OFFSET:
          {
            error = "BMP header offset is invalid";
          }
          break;
          default:
            break;
          }

          if (buffer_old != nullptr)
          {
            free(buffer_old); // old frame was already sent to the EPD (or skipped)
            buffer_old = nullptr;
          }

          if (isPNG && png_res != PNG_NO_ERR)
          {
            filesystem_file_delete("/current.png");
            Log_error_submit("error parsing image file - %s", error.c_str());

            return HTTPS_WRONG_IMAGE_FORMAT;
          }
        }

        return result;
      });

  if (result == HTTPS_UNABLE_TO_CONNECT)
  {
    Log_error_submit("unable to connect");
  }

  if (send_log)
  {
    send_log = false;
  }

  Log_info("Returned result - %d", result);

  return result;
}

/**
 * @brief Read an HTTP body into buffer.
 * @param content_size expected body length; <= 0 means unknown (no
 *        Content-Length), in which case reading stops when the server closes
 *        the connection or buffer_size bytes have been read
 * @param buffer_size capacity of buffer; only used when content_size <= 0
 */
uint32_t downloadStream(WiFiClient *stream, int content_size, uint8_t *buffer, int buffer_size)
{
  int iteration_counter = 0;
  bool bUnknownSize = content_size <= 0;
  int target = bUnknownSize ? buffer_size : content_size;
  unsigned long download_start = millis();
  int counter = 0;
  while (counter < target && millis() - download_start < 10000)
  {
    int available = stream->available();
    if (available > 0)
    {
      Log.info("%s [%d]: Downloading... Available bytes: %d\r\n", __FILE__, __LINE__, available);
      int remaining = target - counter;
      counter += stream->readBytes(buffer + counter, available < remaining ? available : remaining);
      iteration_counter++;
    }
    else if (bUnknownSize && !stream->connected())
    {
      break; // server closed the connection: body complete
    }
    delay(10);
  }
  if (bUnknownSize && counter >= target && stream->available() > 0)
  {
    Log_error("Image larger than %d bytes; truncated", buffer_size);
  }

  Log_info("Download end: %d/%d bytes in %d ms (%d iterations)", counter, content_size, millis() - download_start, iteration_counter);
  return counter;
}

https_request_err_e handleApiDisplayResponse(ApiDisplayResponse &apiResponse)
{
  https_request_err_e result = HTTPS_NO_ERR;
  int file_size = 0;

  if (special_function == SF_NONE)
  {
    uint64_t request_status = apiResponse.status;
    Log.info("%s [%d]: status: %d\r\n", __FILE__, __LINE__, request_status);
    switch (request_status)
    {
    case 0:
    {
      String image_url = apiResponse.image_url;
      update_firmware = apiResponse.update_firmware;
      String firmware_url = apiResponse.firmware_url;
      uint64_t rate = apiResponse.refresh_rate;
      reset_firmware = apiResponse.reset_firmware;

      bool sleep_5_seconds = false;

      writeSpecialFunction(apiResponse.special_function);

      if (update_firmware)
      {
        Log.info("%s [%d]: update firmware. Check URL\r\n", __FILE__, __LINE__);
        if (firmware_url.length() == 0)
        {
          Log.error("%s [%d]: Empty URL\r\n", __FILE__, __LINE__);
          update_firmware = false;
        }
      }
      if (image_url.length() > 0)
      {
        Log.info("%s [%d]: image_url: %s\r\n", __FILE__, __LINE__, image_url.c_str());
        Log.info("%s [%d]: image url end with: %d\r\n", __FILE__, __LINE__, image_url.endsWith("/setup-logo.bmp"));

        image_url.toCharArray(filename, image_url.length() + 1);
        // check if plugin is applied
        bool flag = preferences.getBool(PREFERENCES_DEVICE_REGISTERED_KEY, false);
        Log.info("%s [%d]: flag: %d\r\n", __FILE__, __LINE__, flag);

        if (apiResponse.filename == "empty_state")
        {
          Log.info("%s [%d]: End with empty_state\r\n", __FILE__, __LINE__);
          if (!flag)
          {
            // draw received logo
            status = true;
            // set flag to true
            if (preferences.getBool(PREFERENCES_DEVICE_REGISTERED_KEY, false) != true) // check the flag to avoid the re-writing
            {
              bool res = preferences.putBool(PREFERENCES_DEVICE_REGISTERED_KEY, true);
              if (res)
                Log.info("%s [%d]: Flag written true successfully\r\n", __FILE__, __LINE__);
              else
                Log.error("%s [%d]: FLag writing failed\r\n", __FILE__, __LINE__);
            }
          }
          else
          {
            // don't draw received logo
            status = false;
          }
          // sleep 5 seconds
          sleep_5_seconds = true;
        }
        else
        {
          Log.info("%s [%d]: End with NO empty_state\r\n", __FILE__, __LINE__);
          if (flag)
          {
            if (preferences.getBool(PREFERENCES_DEVICE_REGISTERED_KEY, false) != false) // check the flag to avoid the re-writing
            {
              bool res = preferences.putBool(PREFERENCES_DEVICE_REGISTERED_KEY, false);
              if (res)
                Log.info("%s [%d]: Flag written false successfully\r\n", __FILE__, __LINE__);
              else
                Log.error("%s [%d]: FLag writing failed\r\n", __FILE__, __LINE__);
            }
          }
          // Using filename from API response
          new_filename = apiResponse.filename;

          // Print the extracted string
          Log.info("%s [%d]: New filename - %s\r\n", __FILE__, __LINE__, new_filename.c_str());
          if (!checkCurrentFileName(new_filename))
          {
            Log.info("%s [%d]: New image. Show it.\r\n", __FILE__, __LINE__);
            status = true;
          }
          else
          {
            Log.info("%s [%d]: Old image. No needed to show it.\r\n", __FILE__, __LINE__);
            status = false;
            result = HTTPS_SUCCESS;
          }
        }
      }
      Log.info("%s [%d]: update_firmware: %d\r\n", __FILE__, __LINE__, update_firmware);
      if (firmware_url.length() > 0)
      {
        Log.info("%s [%d]: firmware_url: %s\r\n", __FILE__, __LINE__, firmware_url.c_str());
        firmware_url.toCharArray(binUrl, firmware_url.length() + 1);
      }
      Log.info("%s [%d]: refresh_rate: %d\r\n", __FILE__, __LINE__, rate);
      if (rate != preferences.getUInt(PREFERENCES_SLEEP_TIME_KEY, SLEEP_TIME_TO_SLEEP))
      {
        Log.info("%s [%d]: write new refresh rate: %d\r\n", __FILE__, __LINE__, rate);
        preferences.putUInt(PREFERENCES_SLEEP_TIME_KEY, rate);
        Log.info("%s [%d]: written new refresh rate: %d\r\n", __FILE__, __LINE__, result);
      }

      if (reset_firmware)
      {
        Log.info("%s [%d]: Reset status is true\r\n", __FILE__, __LINE__);
      }

      if (update_firmware)
        result = HTTPS_SUCCESS;
      if (reset_firmware)
        result = HTTPS_RESET;
      if (sleep_5_seconds)
        result = HTTPS_PLUGIN_NOT_ATTACHED;
      Log.info("%s [%d]: result - %d\r\n", __FILE__, __LINE__, result);
    }
    break;
    case 202:
    {
      result = HTTPS_NO_REGISTER;
      Log.info("%s [%d]: write new refresh rate: %d\r\n", __FILE__, __LINE__, SLEEP_TIME_WHILE_NOT_CONNECTED);
      size_t result = preferences.putUInt(PREFERENCES_SLEEP_TIME_KEY, SLEEP_TIME_WHILE_NOT_CONNECTED);
      Log.info("%s [%d]: written new refresh rate: %d\r\n", __FILE__, __LINE__, result);
      status = false;
    }
    break;
    case 500:
    {
      result = HTTPS_RESET;
      Log.info("%s [%d]: write new refresh rate: %d\r\n", __FILE__, __LINE__, SLEEP_TIME_WHILE_NOT_CONNECTED);
      preferences.putUInt(PREFERENCES_SLEEP_TIME_KEY, SLEEP_TIME_WHILE_NOT_CONNECTED);
      Log.info("%s [%d]: written new refresh rate: %d\r\n", __FILE__, __LINE__, result);
      status = false;
    }
    break;

    default:
      break;
    }
  }
  else if (special_function != SF_NONE)
  {
    uint64_t request_status = apiResponse.status;
    Log.info("%s [%d]: status: %d\r\n", __FILE__, __LINE__, request_status);
    switch (request_status)
    {
    case 0:
    {
      switch (special_function)
      {
      case SF_IDENTIFY:
      {
        String action = apiResponse.action;
        if (action.equals("identify"))
        {
          Log.info("%s [%d]:Identify success\r\n", __FILE__, __LINE__);
          String image_url = apiResponse.image_url;
          if (image_url.length() > 0)
          {
            Log.info("%s [%d]: image_url: %s\r\n", __FILE__, __LINE__, image_url.c_str());
            Log.info("%s [%d]: image url end with: %d\r\n", __FILE__, __LINE__, image_url.endsWith("/setup-logo.bmp"));

            image_url.toCharArray(filename, image_url.length() + 1);
            // check if plugin is applied
            bool flag = preferences.getBool(PREFERENCES_DEVICE_REGISTERED_KEY, false);
            Log.info("%s [%d]: flag: %d\r\n", __FILE__, __LINE__, flag);

            if (apiResponse.filename == "empty_state")
            {
              Log.info("%s [%d]: End with empty_state\r\n", __FILE__, __LINE__);
              if (!flag)
              {
                // draw received logo
                status = true;
                // set flag to true
                if (preferences.getBool(PREFERENCES_DEVICE_REGISTERED_KEY, false) != true) // check the flag to avoid the re-writing
                {
                  bool res = preferences.putBool(PREFERENCES_DEVICE_REGISTERED_KEY, true);
                  if (res)
                    Log.info("%s [%d]: Flag written true successfully\r\n", __FILE__, __LINE__);
                  else
                    Log.error("%s [%d]: FLag writing failed\r\n", __FILE__, __LINE__);
                }
              }
              else
              {
                status = false;
              }
            }
            else
            {
              Log.info("%s [%d]: End with NO empty_state\r\n", __FILE__, __LINE__);
              if (flag)
              {
                if (preferences.getBool(PREFERENCES_DEVICE_REGISTERED_KEY, false) != false) // check the flag to avoid the re-writing
                {
                  bool res = preferences.putBool(PREFERENCES_DEVICE_REGISTERED_KEY, false);
                  if (res)
                    Log.info("%s [%d]: Flag written false successfully\r\n", __FILE__, __LINE__);
                  else
                    Log.error("%s [%d]: FLag writing failed\r\n", __FILE__, __LINE__);
                }
              }
              status = true;
            }
          }
        }
        else
        {
          Log.error("%s [%d]: identify failed\r\n", __FILE__, __LINE__);
        }
      }
      break;
      case SF_SLEEP:
      {
        String action = apiResponse.action;
        if (action.equals("sleep"))
        {
          uint64_t rate = apiResponse.refresh_rate;
          Log.info("%s [%d]: refresh_rate: %d\r\n", __FILE__, __LINE__, rate);
          if (rate != preferences.getUInt(PREFERENCES_SLEEP_TIME_KEY, SLEEP_TIME_TO_SLEEP))
          {
            Log.info("%s [%d]: write new refresh rate: %d\r\n", __FILE__, __LINE__, rate);
            preferences.putUInt(PREFERENCES_SLEEP_TIME_KEY, rate);
            Log.info("%s [%d]: written new refresh rate: %d\r\n", __FILE__, __LINE__, result);
          }
          status = false;
          result = HTTPS_SUCCESS;
          Log.info("%s [%d]: sleep success\r\n", __FILE__, __LINE__);
        }
        else
        {
          Log.error("%s [%d]: sleep failed\r\n", __FILE__, __LINE__);
          // need to add error
        }
      }
      break;
      case SF_ADD_WIFI:
      {
        String action = apiResponse.action;
        if (action.equals("add_wifi"))
        {
          status = false;
          result = HTTPS_SUCCESS;
          Log.info("%s [%d]: Add wifi success\r\n", __FILE__, __LINE__);
        }
        else
        {
          Log.error("%s [%d]: Add wifi failed\r\n", __FILE__, __LINE__);
        }
      }
      break;
      case SF_RESTART_PLAYLIST:
      {
        String action = apiResponse.action;
        if (action.equals("restart_playlist"))
        {
          Log.info("%s [%d]:Restart playlist success\r\n", __FILE__, __LINE__);
          String image_url = apiResponse.image_url;
          if (image_url.length() > 0)
          {
            Log.info("%s [%d]: image_url: %s\r\n", __FILE__, __LINE__, image_url.c_str());
            Log.info("%s [%d]: image url end with: %d\r\n", __FILE__, __LINE__, image_url.endsWith("/setup-logo.bmp"));

            image_url.toCharArray(filename, image_url.length() + 1);
            // check if plugin is applied
            bool flag = preferences.getBool(PREFERENCES_DEVICE_REGISTERED_KEY, false);
            Log.info("%s [%d]: flag: %d\r\n", __FILE__, __LINE__, flag);

            if (apiResponse.filename == "empty_state")
            {
              Log.info("%s [%d]: End with empty_state\r\n", __FILE__, __LINE__);
              if (!flag)
              {
                // draw received logo
                status = true;
                // set flag to true
                if (preferences.getBool(PREFERENCES_DEVICE_REGISTERED_KEY, false) != true) // check the flag to avoid the re-writing
                {
                  bool res = preferences.putBool(PREFERENCES_DEVICE_REGISTERED_KEY, true);
                  if (res)
                    Log.info("%s [%d]: Flag written true successfully\r\n", __FILE__, __LINE__);
                  else
                    Log.error("%s [%d]: FLag writing failed\r\n", __FILE__, __LINE__);
                }
              }
              else
              {
                // don't draw received logo
                status = false;
              }
            }
            else
            {
              Log.info("%s [%d]: End with NO empty_state\r\n", __FILE__, __LINE__);
              if (flag)
              {
                if (preferences.getBool(PREFERENCES_DEVICE_REGISTERED_KEY, false) != false) // check the flag to avoid the re-writing
                {
                  bool res = preferences.putBool(PREFERENCES_DEVICE_REGISTERED_KEY, false);
                  if (res)
                    Log.info("%s [%d]: Flag written false successfully\r\n", __FILE__, __LINE__);
                  else
                    Log.error("%s [%d]: FLag writing failed\r\n", __FILE__, __LINE__);
                }
              }
              status = true;
            }
          }
        }
        else
        {
          Log.error("%s [%d]: Restart playlist failed\r\n", __FILE__, __LINE__);
        }
      }
      break;
      case SF_REWIND:
      {
        String action = apiResponse.action;
        if (action.equals("rewind"))
        {
          bool isPNG = false;
          status = false;
          result = HTTPS_SUCCESS;
          Log.info("%s [%d]: rewind success\r\n", __FILE__, __LINE__);

          bool image_reverse = false;
          bool file_check_bmp = true;
          image_err_e image_proccess_response = PNG_WRONG_FORMAT;
          bmp_err_e bmp_proccess_response = BMP_NOT_BMP;

          // showMessageWithLogo(MSG_FORMAT_ERROR);
          String last_dot_file = filesystem_file_exists("/last.bmp") ? "/last.bmp" : "/last.png";
          if (last_dot_file == "/last.bmp")
          {
            Log.info("Rewind BMP\n\r");
            buffer = (uint8_t *)malloc(DISPLAY_BMP_IMAGE_SIZE);
            file_check_bmp = filesystem_read_from_file(last_dot_file.c_str(), buffer, DISPLAY_BMP_IMAGE_SIZE);
            bmp_proccess_response = parseBMPHeader(buffer, image_reverse);
          }
          else if (last_dot_file == "/last.png")
          {
            isPNG = true;
            Log.info("Rewind PNG\n\r");
            buffer = display_read_file(last_dot_file.c_str(), &file_size);
            image_proccess_response = PNG_NO_ERR; // DEBUG
          }

          if (file_check_bmp)
          {
            switch (image_proccess_response)
            {
            case PNG_NO_ERR:
            {
              Log.info("Showing image\n\r");
              display_show_image(buffer, file_size, true);
              need_to_refresh_display = 1;
            }
            break;
            default:
            {
            }
            break;
            }
            switch (bmp_proccess_response)
            {
            case BMP_NO_ERR:
            {
              Log.info("Showing image\n\r");
              display_show_image(buffer, DISPLAY_BMP_IMAGE_SIZE, true);
              need_to_refresh_display = 1;
            }
            break;
            default:
            {
            }
            break;
            }
          }
          else
          {
            free(buffer);
            buffer = nullptr;
            showMessageWithLogo(MSG_FORMAT_ERROR);
          }
        }
        else
        {
          Log.error("%s [%d]: rewind failed\r\n", __FILE__, __LINE__);
        }
      }
      break;
      case SF_SEND_TO_ME:
      {
        String action = apiResponse.action;

        if (action.equals("send_to_me"))
        {
          bool isPNG = false;
          status = false;
          result = HTTPS_SUCCESS;
          Log.info("%s [%d]: send_to_me success\r\n", __FILE__, __LINE__);

          bool image_reverse = false;

          if (!filesystem_file_exists("/current.bmp") && !filesystem_file_exists("/current.png"))
          {
            Log.info("%s [%d]: No current image!\r\n", __FILE__, __LINE__);
            free(buffer);
            buffer = nullptr;
            return HTTPS_WRONG_IMAGE_FORMAT;
          }

          if (filesystem_file_exists("/current.bmp"))
          {
            Log.info("%s [%d]: send_to_me BMP\r\n", __FILE__, __LINE__);
            buffer = (uint8_t *)malloc(DISPLAY_BMP_IMAGE_SIZE);

            if (!filesystem_read_from_file("/current.bmp", buffer, DISPLAY_BMP_IMAGE_SIZE))
            {
              free(buffer);
              buffer = nullptr;
              Log_error_submit("Error reading image!");
              return HTTPS_WRONG_IMAGE_FORMAT;
            }

            bmp_err_e bmp_parse_result = parseBMPHeader(buffer, image_reverse);
            if (bmp_parse_result != BMP_NO_ERR)
            {
              free(buffer);
              buffer = nullptr;
              Log_error_submit("Error parsing BMP header, code: %d", bmp_parse_result);
              return HTTPS_WRONG_IMAGE_FORMAT;
            }
          }
          else if (filesystem_file_exists("/current.png"))
          {
            Log.info("%s [%d]: send_to_me PNG\r\n", __FILE__, __LINE__);
            image_err_e png_parse_result = PNG_NO_ERR; // DEBUG
            buffer = display_read_file("/current.png", &file_size);
            // No old frame is passed here, so display_show_image() will degrade
            // a partial refresh to a fast refresh automatically.
            if (png_parse_result != PNG_NO_ERR)
            {
              Log_error_submit("Error parsing PNG header, code: %d", png_parse_result);
              free(buffer);
              buffer = nullptr;
              return HTTPS_WRONG_IMAGE_FORMAT;
            }
          }

          Log.info("Showing image\n\r");
          display_show_image(buffer, file_size, true);
          need_to_refresh_display = 1;

          free(buffer);
          buffer = nullptr;
        }
        else
        {
          Log.error("%s [%d]: send_to_me failed\r\n", __FILE__, __LINE__);
        }
      }
      break;
      case SF_GUEST_MODE:
      {
        String action = apiResponse.action;
        if (action.equals("guest_mode"))
        {
          Log.info("%s [%d]:Guest Mode success\r\n", __FILE__, __LINE__);
          String image_url = apiResponse.image_url;
          if (image_url.length() > 0)
          {
            Log.info("%s [%d]: image_url: %s\r\n", __FILE__, __LINE__, image_url.c_str());
            Log.info("%s [%d]: image url end with: %d\r\n", __FILE__, __LINE__, image_url.endsWith("/setup-logo.bmp"));

            image_url.toCharArray(filename, image_url.length() + 1);
            // check if plugin is applied
            bool flag = preferences.getBool(PREFERENCES_DEVICE_REGISTERED_KEY, false);
            Log.info("%s [%d]: flag: %d\r\n", __FILE__, __LINE__, flag);

            if (apiResponse.filename == "empty_state")
            {
              Log.info("%s [%d]: End with empty_state\r\n", __FILE__, __LINE__);
              if (!flag)
              {
                // draw received logo
                status = true;
                // set flag to true
                if (preferences.getBool(PREFERENCES_DEVICE_REGISTERED_KEY, false) != true) // check the flag to avoid the re-writing
                {
                  bool res = preferences.putBool(PREFERENCES_DEVICE_REGISTERED_KEY, true);
                  if (res)
                    Log.info("%s [%d]: Flag written true successfully\r\n", __FILE__, __LINE__);
                  else
                    Log.error("%s [%d]: FLag writing failed\r\n", __FILE__, __LINE__);
                }
              }
              else
              {
                // don't draw received logo
                status = false;
              }
            }
            else
            {
              Log.info("%s [%d]: End with NO empty_state\r\n", __FILE__, __LINE__);
              if (flag)
              {
                if (preferences.getBool(PREFERENCES_DEVICE_REGISTERED_KEY, false) != false) // check the flag to avoid the re-writing
                {
                  bool res = preferences.putBool(PREFERENCES_DEVICE_REGISTERED_KEY, false);
                  if (res)
                    Log.info("%s [%d]: Flag written false successfully\r\n", __FILE__, __LINE__);
                  else
                    Log.error("%s [%d]: FLag writing failed\r\n", __FILE__, __LINE__);
                }
              }
              status = true;
            }
          }
        }
        else
        {
          Log.error("%s [%d]: Guest Mode failed\r\n", __FILE__, __LINE__);
        }
      }
      break;
      default:
        break;
      }
    }
    break;
    case 202:
    {
      result = HTTPS_NO_REGISTER;
      Log.info("%s [%d]: write new refresh rate: %d\r\n", __FILE__, __LINE__, SLEEP_TIME_WHILE_NOT_CONNECTED);
      preferences.putUInt(PREFERENCES_SLEEP_TIME_KEY, SLEEP_TIME_WHILE_NOT_CONNECTED);
      Log.info("%s [%d]: written new refresh rate: %d\r\n", __FILE__, __LINE__, result);
      status = false;
    }
    break;
    case 500:
    {
      result = HTTPS_RESET;
      Log.info("%s [%d]: write new refresh rate: %d\r\n", __FILE__, __LINE__, SLEEP_TIME_WHILE_NOT_CONNECTED);
      preferences.putUInt(PREFERENCES_SLEEP_TIME_KEY, SLEEP_TIME_WHILE_NOT_CONNECTED);
      Log.info("%s [%d]: written new refresh rate: %d\r\n", __FILE__, __LINE__, result);
      status = false;
    }
    break;

    default:
      break;
    }
  }
  return result;
}

/**
 * @brief Performs API setup call to get credentials and image URL
 * @return true if should continue to image download, false otherwise
 */
static bool performApiSetup()
{
  // Set up the API inputs
  ApiSetupInputs inputs;
  inputs.baseUrl = preferences.getString(PREFERENCES_API_URL, API_BASE_URL);
  inputs.macAddress = WiFi.macAddress();
  inputs.firmwareVersion = FW_VERSION_STRING;

  Log.info("%s [%d]: [HTTPS] begin /api/setup/ ...\r\n", __FILE__, __LINE__);
  Log.info("%s [%d]: RSSI: %d\r\n", __FILE__, __LINE__, WiFi.RSSI());
  Log.info("%s [%d]: Device MAC address: %s\r\n", __FILE__, __LINE__, WiFi.macAddress().c_str());

  // Call the API client
  ApiSetupResult result = fetchApiSetup(inputs);

  // Handle connection errors
  if (result.error == HTTPS_UNABLE_TO_CONNECT)
  {
    showMessageWithLogo(WIFI_INTERNAL_ERROR);
    Log_error_submit("[HTTPS] %s", result.error_detail.c_str());
    return false;
  }

  // Handle JSON parsing errors
  if (result.error == HTTPS_JSON_PARSING_ERR)
  {
    Log.error("%s [%d]: JSON deserialization error.\r\n", __FILE__, __LINE__);
    return false;
  }

  // Handle HTTP request errors
  if (result.error != HTTPS_NO_ERR)
  {
    if (WiFi.RSSI() > WIFI_CONNECTION_RSSI)
    {
      showMessageWithLogo(API_ERROR);
    }
    else
    {
      showMessageWithLogo(WIFI_WEAK);
    }
    Log_error_submit("[HTTPS] Request failed: %s", result.error_detail.c_str());
    return false;
  }

  // Process the successful response
  auto &apiResponse = result.response;
  uint16_t url_status = apiResponse.status;

  Log.info("%s [%d]: GET... code: %d\r\n", __FILE__, __LINE__, url_status);

  if (url_status == 200)
  {
    status = true;
    Log.info("%s [%d]: status OK.\r\n", __FILE__, __LINE__);

    String api_key = apiResponse.api_key;
    Log.info("%s [%d]: API key - %s\r\n", __FILE__, __LINE__, api_key.c_str());
    size_t res = preferences.putString(PREFERENCES_API_KEY, api_key);
    Log.info("%s [%d]: api key saved in the preferences - %d\r\n", __FILE__, __LINE__, res);

    String friendly_id = apiResponse.friendly_id;
    Log.info("%s [%d]: friendly ID - %s\r\n", __FILE__, __LINE__, friendly_id.c_str());
    res = preferences.putString(PREFERENCES_FRIENDLY_ID, friendly_id);
    Log.info("%s [%d]: friendly ID saved in the preferences - %d\r\n", __FILE__, __LINE__, res);

    String image_url = apiResponse.image_url;
    Log.info("%s [%d]: image_url - %s\r\n", __FILE__, __LINE__, image_url.c_str());
    image_url.toCharArray(filename, image_url.length() + 1);

    String message_str = apiResponse.message;
    Log.info("%s [%d]: message - %s\r\n", __FILE__, __LINE__, message_str.c_str());
    message_str.toCharArray(message_buffer, message_str.length() + 1);

    Log.info("%s [%d]: status - %d\r\n", __FILE__, __LINE__, status);
    return true;
  }
  else if (url_status == 404)
  {
    Log_info("MAC Address is not registered on server");

    showMessageWithLogo(MAC_NOT_REGISTERED, apiResponse);

    preferences.putUInt(PREFERENCES_SLEEP_TIME_KEY, SLEEP_TIME_TO_SLEEP);

    display_sleep();
    goToSleep();
    return false;
  }
  else
  {
    Log.info("%s [%d]: status FAIL.\r\n", __FILE__, __LINE__);
    status = false;
    return false;
  }
}

/**
 * @brief Downloads and displays the setup image from the API response
 * @return none
 */
static void downloadSetupImage()
{
  status = false;
  Log.info("%s [%d]: filename - %s\r\n", __FILE__, __LINE__, filename);

  withHttp(filename, [&](HTTPClient *https, HttpError error) -> bool
           {
    if (error != HttpError::HTTPCLIENT_SUCCESS)
    {
      if (WiFi.RSSI() > WIFI_CONNECTION_RSSI)
      {
        showMessageWithLogo(API_ERROR);
      }
      else
      {
        showMessageWithLogo(WIFI_WEAK);
      }
      Log_error_submit("[HTTPS] Unable to connect");
      return false;
    }

    https->setTimeout(15000);
    https->setConnectTimeout(15000);

    Log.info("%s [%d]: [HTTPS] Request to %s\r\n", __FILE__, __LINE__, filename);
    Log.info("%s [%d]: [HTTPS] GET..\r\n", __FILE__, __LINE__);

    int httpCode = https->GET();

    // httpCode will be negative on error
    if (httpCode <= 0)
    {
      if (WiFi.RSSI() > WIFI_CONNECTION_RSSI)
      {
        showMessageWithLogo(API_ERROR);
      }
      else
      {
        showMessageWithLogo(WIFI_WEAK);
      }
      Log_error_submit("[HTTPS] GET... failed, error: %s", https->errorToString(httpCode).c_str());
      return false;
    }

    // HTTP header has been send and Server response header has been handled
    Log.error("%s [%d]: [HTTPS] GET... code: %d\r\n", __FILE__, __LINE__, httpCode);
    
    // file found at server
    if (httpCode != HTTP_CODE_OK && httpCode != HTTP_CODE_MOVED_PERMANENTLY)
    {
      if (WiFi.RSSI() > WIFI_CONNECTION_RSSI)
      {
        showMessageWithLogo(API_ERROR);
      }
      else
      {
        showMessageWithLogo(WIFI_WEAK);
      }
      Log_error_submit("[HTTPS] GET... failed, error: %s", https->errorToString(httpCode).c_str());
      return false;
    }

    Log.info("%s [%d]: Content size: %d\r\n", __FILE__, __LINE__, https->getSize());

    WiFiClient *stream = https->getStreamPtr();

    uint32_t counter = 0;
    // Read and save BMP data to buffer
    buffer = (uint8_t *)malloc(https->getSize());
    if (stream->available() && https->getSize() == DISPLAY_BMP_IMAGE_SIZE)
    {
      counter = downloadStream(stream, DISPLAY_BMP_IMAGE_SIZE, buffer);
    }
    
    if (counter == DISPLAY_BMP_IMAGE_SIZE)
    {
      Log.info("%s [%d]: Received successfully\r\n", __FILE__, __LINE__);

      writeImageToFile("/logo.bmp", buffer, DEFAULT_IMAGE_SIZE);

      // show the image
      String friendly_id = preferences.getString(PREFERENCES_FRIENDLY_ID, PREFERENCES_FRIENDLY_ID_DEFAULT);
      display_show_msg(storedLogoOrDefault(0), FRIENDLY_ID, friendly_id, true, "", String(message_buffer));
      need_to_refresh_display = 0;
    }
    else
    {
      free(buffer);
      buffer = nullptr;
      if (WiFi.RSSI() > WIFI_CONNECTION_RSSI)
      {
        showMessageWithLogo(API_SIZE_ERROR);
      }
      else
      {
        showMessageWithLogo(WIFI_WEAK);
      }
      Log_error_submit("Receiving failed. Read: %d", counter);
    }
    
    return true; });
}

/**
 * @brief Function to getting the friendly id and API key
 * @return none
 */
static void getDeviceCredentials()
{
  bool shouldDownloadImage = performApiSetup();

  Log.info("%s [%d]: status - %d\r\n", __FILE__, __LINE__, status);
  if (shouldDownloadImage)
  {
    downloadSetupImage();
  }
}

/**
 * @brief Function to reset the friendly id, API key, WiFi SSID and password
 * @param url Server URL address
 * @return none
 */
static void resetDeviceCredentials(void)
{
  Log.info("%s [%d]: The device will be reset now...\r\n", __FILE__, __LINE__);
  Log.info("%s [%d]: WiFi reseting...\r\n", __FILE__, __LINE__);
  WifiCaptivePortal.resetSettings();
  need_to_refresh_display = 1;
  bool res = preferences.clear();
  if (res)
    Log.info("%s [%d]: The device reset success. Restarting...\r\n", __FILE__, __LINE__);
  else
    Log.error("%s [%d]: The device reseting error. The device will be reset now...\r\n", __FILE__, __LINE__);
  preferences.end();
  restartIntentionally("credentials reset");
}

/**
 * @brief Function to check and performing OTA update
 * @param none
 * @return none
 */
static void checkAndPerformFirmwareUpdate(void)
{

  withHttp(binUrl, [&](HTTPClient *https, HttpError errorCode) -> bool
           {
             if (errorCode != HttpError::HTTPCLIENT_SUCCESS || !https)
             {
               Log.fatal("%s [%d]: Unable to connect for firmware update\r\n", __FILE__, __LINE__);
               if (WiFi.RSSI() > WIFI_CONNECTION_RSSI)
               {
                 showMessageWithLogo(API_ERROR);
               }
               else
               {
                 showMessageWithLogo(WIFI_WEAK);
               }
             }

             int httpCode = https->GET();
             if (httpCode == HTTP_CODE_OK)
             {
               Log.info("%s [%d]: Downloading .bin file...\r\n", __FILE__, __LINE__);

               size_t contentLength = https->getSize();
               // Perform firmware update
               if (Update.begin(contentLength))
               {
                 Log.info("%s [%d]: Firmware update start\r\n", __FILE__, __LINE__);
                 showMessageWithLogo(FW_UPDATE);

                 if (Update.writeStream(https->getStream()))
                 {
                   if (Update.end(true))
                   {
                     Log.info("%s [%d]: Firmware update successful. Rebooting...\r\n", __FILE__, __LINE__);
                     ota_succeeded = true;
                     showMessageWithLogo(FW_UPDATE_SUCCESS);
                   }
                   else
                   {
                     Log.fatal("%s [%d]: Firmware update failed!\r\n", __FILE__, __LINE__);
                     showMessageWithLogo(FW_UPDATE_FAILED);
                   }
                 }
                 else
                 {
                   Log.fatal("%s [%d]: Write to firmware update stream failed!\r\n", __FILE__, __LINE__);
                   showMessageWithLogo(FW_UPDATE_FAILED);
                 }
               }
               else
               {
                 Log.fatal("%s [%d]: Begin firmware update failed!\r\n", __FILE__, __LINE__);
                 showMessageWithLogo(FW_UPDATE_FAILED);
               }
             }
             return true; });
}

/**
 * @brief Function to sleep preparing and go to sleep
 * @param none
 * @return none
 */
static void goToSleep(void)
{
  submitStoredLogs();
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.disconnect();
  }
  WiFi.mode(WIFI_OFF); 
  filesystem_deinit();
  uint32_t time_to_sleep = SLEEP_TIME_TO_SLEEP;
  if (preferences.isKey(PREFERENCES_SLEEP_TIME_KEY))
    time_to_sleep = preferences.getUInt(PREFERENCES_SLEEP_TIME_KEY, SLEEP_TIME_TO_SLEEP);
  Log.info("%s [%d]: time to sleep - %d\r\n", __FILE__, __LINE__, time_to_sleep);
  preferences.putUInt(PREFERENCES_LAST_SLEEP_TIME, getTime());
  preferences.end();
  deepSleepNow(time_to_sleep, false);
}

/**
 * @brief Minimal deep sleep for the boot-loop and battery guards: runs before
 *        WiFi and SPIFFS are started, so only NVS needs closing.
 * @param seconds sleep duration
 * @param backoff true for a boot-loop backoff sleep (keeps the guard counter)
 */
static void guardDeepSleep(uint32_t seconds, bool backoff)
{
  preferences.end();
  deepSleepNow(seconds, backoff);
}

/**
 * @brief Arm the timer + button wake sources and enter deep sleep. EVERY deep
 *        sleep goes through here so the boot-loop guard sees it: an
 *        intentional sleep clears the guard, a backoff sleep advances it.
 *        Called after preferences.end(): log at INFO only (serial; ERROR logs
 *        would try to store into the closed NVS).
 */
static void deepSleepNow(uint32_t seconds, bool backoff)
{
  if (backoff)
    boot_guard_on_backoff_sleep(&boot_guard_state);
  else
    boot_guard_on_intentional_sleep(&boot_guard_state);
  Log_info("Deep sleep %lu s (%s); boot guard boots=%lu step=%lu flags=0x%lx",
           (unsigned long)seconds, backoff ? "boot-loop backoff" : "intentional",
           (unsigned long)boot_guard_state.boots, (unsigned long)boot_guard_state.backoff_step,
           (unsigned long)boot_guard_state.flags);
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * SLEEP_uS_TO_S_FACTOR);
  // Configure GPIO pin for wakeup
#if CONFIG_IDF_TARGET_ESP32
  #define BUTTON_PIN_BITMASK(GPIO) (1ULL << GPIO)  // 2 ^ GPIO_NUMBER in hex
  esp_sleep_enable_ext1_wakeup(BUTTON_PIN_BITMASK(PIN_INTERRUPT), ESP_EXT1_WAKEUP_ALL_LOW);
#elif CONFIG_IDF_TARGET_ESP32C3
  esp_deep_sleep_enable_gpio_wakeup(1 << PIN_INTERRUPT, ESP_GPIO_WAKEUP_GPIO_LOW);
#elif CONFIG_IDF_TARGET_ESP32S3
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_INTERRUPT, 0);
#else
#error "Unsupported ESP32 target for GPIO wakeup configuration"
#endif
  esp_deep_sleep_start();
}

/**
 * @brief ESP.restart() for deliberate reboots (credential reset, applied OTA):
 *        the boot-loop guard forgives the next boot (capped at
 *        BOOT_GUARD_MAX_FORGIVEN_RESTARTS in a row, so a restart loop is
 *        still caught).
 */
static void restartIntentionally(const char *why)
{
  boot_guard_on_intentional_restart(&boot_guard_state);
  Log_info("Intentional restart (%s); boot guard won't count the next boot", why);
  ESP.restart();
}

static const char *resetReasonName(esp_reset_reason_t reason)
{
  switch (reason)
  {
  case ESP_RST_POWERON:
    return "POWERON";
  case ESP_RST_EXT:
    return "EXT";
  case ESP_RST_SW:
    return "SW";
  case ESP_RST_PANIC:
    return "PANIC";
  case ESP_RST_INT_WDT:
    return "INT_WDT";
  case ESP_RST_TASK_WDT:
    return "TASK_WDT";
  case ESP_RST_WDT:
    return "WDT";
  case ESP_RST_DEEPSLEEP:
    return "DEEPSLEEP";
  case ESP_RST_BROWNOUT:
    return "BROWNOUT";
  case ESP_RST_SDIO:
    return "SDIO";
  default:
    return "UNKNOWN";
  }
}

static BootGuardResetKind classifyResetReason(esp_reset_reason_t reason)
{
  switch (reason)
  {
  case ESP_RST_POWERON:
  case ESP_RST_EXT: // reset pin: a deliberate fresh start, like power-on
    return BOOT_RESET_POWER_ON;
  case ESP_RST_DEEPSLEEP:
    return BOOT_RESET_DEEP_SLEEP;
  case ESP_RST_SW:
    return BOOT_RESET_SOFTWARE;
  default: // panic, watchdogs, brownout, unknown (incl. USB-JTAG reset on IDF 4.4)
    return BOOT_RESET_ABNORMAL;
  }
}

/**
 * @brief Count this boot in the RTC_NOINIT boot-loop guard and decide whether
 *        it may run normally. Runs before NVS is open: serial logging only.
 */
static void bootGuardOnBoot(void)
{
  boot_reset_reason = esp_reset_reason();
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool button_wake = cause == ESP_SLEEP_WAKEUP_GPIO || cause == ESP_SLEEP_WAKEUP_EXT0 || cause == ESP_SLEEP_WAKEUP_EXT1;
  boot_guard_decision = boot_guard_on_boot(&boot_guard_state, classifyResetReason(boot_reset_reason),
                                           (uint32_t)boot_reset_reason, button_wake);
  Log_info("Boot guard: reset=%s(%d) wake=%d button=%d reinit=%d forgiven=%d boots=%lu step=%lu last_abnormal=%s -> %s (sleep %lu s)",
           resetReasonName(boot_reset_reason), (int)boot_reset_reason, (int)cause, button_wake ? 1 : 0,
           boot_guard_decision.reinitialized ? 1 : 0, boot_guard_decision.forgiven_restart ? 1 : 0,
           (unsigned long)boot_guard_decision.boots, (unsigned long)boot_guard_state.backoff_step,
           resetReasonName((esp_reset_reason_t)boot_guard_state.last_abnormal_reason),
           boot_guard_action_name(boot_guard_decision.action), (unsigned long)boot_guard_decision.sleep_seconds);
}

/**
 * @brief After abnormal resets, leave one server-visible line per wake. Uses
 *        Log_error: app_logger only stores/submits logs >= ERROR (Log_info_*
 *        never reaches the server). The line is stored in NVS now and flushed
 *        by submitStoredLogs() as soon as a wake gets online (this one, or the
 *        next allowed attempt), so it also survives a crash before WiFi.
 *        Needs preferences open.
 */
static void bootGuardReport(void)
{
  if (!boot_guard_decision.abnormal)
    return;
  Log_error("Boot-loop guard: %lu boots in a row without deep sleep; reset=%s, last abnormal reset=%s, backoff step %lu -> %s (sleep %lu s)",
            (unsigned long)boot_guard_decision.boots, resetReasonName(boot_reset_reason),
            resetReasonName((esp_reset_reason_t)boot_guard_state.last_abnormal_reason),
            (unsigned long)boot_guard_state.backoff_step, boot_guard_action_name(boot_guard_decision.action),
            (unsigned long)boot_guard_decision.sleep_seconds);
}

/**
 * @brief Deep-discharge guard. vBatt was read before WiFi. While the battery
 *        is critical: no WiFi, a one-time "battery empty" screen (NVS flag, so
 *        it is not redrawn every wake), deep sleep BATTERY_CRITICAL_RECHECK_S
 *        and re-check; resume at BATTERY_CRITICAL_V + hysteresis. Readings
 *        below BATTERY_INVALID_BELOW_V are garbage / no battery and never
 *        hold the device. Returns only when the device may run normally.
 */
static void batteryGuardCheck(void)
{
#ifdef FAKE_BATTERY_VOLTAGE
  Log_info("Battery guard: skipped (FAKE_BATTERY_VOLTAGE build)");
#else
  const int critical_mv = battery_volts_to_mv(BATTERY_CRITICAL_V);
  const int hysteresis_mv = battery_volts_to_mv(BATTERY_CRITICAL_HYSTERESIS_V);
  const int invalid_mv = battery_volts_to_mv(BATTERY_INVALID_BELOW_V);
  bool in_critical = preferences.getBool(PREFERENCES_BATTERY_CRITICAL, false);
  int vbatt_mv = battery_volts_to_mv(vBatt);
  BatteryGuardAction action = battery_guard_decide(vbatt_mv, in_critical, critical_mv, hysteresis_mv, invalid_mv);
  Log_info("Battery guard: vbatt=%d mV in_critical=%d (enter<%d, resume>=%d, invalid<%d mV) -> %s",
           vbatt_mv, in_critical ? 1 : 0, critical_mv, critical_mv + hysteresis_mv, invalid_mv,
           battery_guard_action_name(action));

  if (action == BATTERY_GUARD_ENTER)
  {
    // one ADC glitch must not pause the device: confirm with a second reading
    float confirm_v = readBatteryVoltage();
    int confirm_mv = battery_volts_to_mv(confirm_v);
    BatteryGuardAction confirm = battery_guard_decide(confirm_mv, false, critical_mv, hysteresis_mv, invalid_mv);
    Log_info("Battery guard: confirm reading %d mV -> %s", confirm_mv, battery_guard_action_name(confirm));
    if (confirm != BATTERY_GUARD_ENTER)
    {
      Log_info("Battery guard: low reading not confirmed; running normally");
      vBatt = confirm_v; // report the plausible reading in telemetry
      return;
    }
  }

  switch (action)
  {
  case BATTERY_GUARD_NORMAL:
    return;
  case BATTERY_GUARD_INVALID:
    if (in_critical)
    {
      // the MCU runs, so it is externally powered (battery likely detached)
      preferences.remove(PREFERENCES_BATTERY_CRITICAL);
      Log_error("Battery guard released: invalid reading %d mV (no battery / external power); resuming", vbatt_mv);
    }
    return;
  case BATTERY_GUARD_RELEASE:
    preferences.remove(PREFERENCES_BATTERY_CRITICAL);
    Log_error("Battery guard released at %d mV (>= %d mV); resuming WiFi", vbatt_mv, critical_mv + hysteresis_mv);
    return;
  case BATTERY_GUARD_ENTER:
    // flag BEFORE drawing: if drawing crashes, the next boot holds without
    // redrawing instead of looping on the screen
    preferences.putBool(PREFERENCES_BATTERY_CRITICAL, true);
    // force the first fetch after recovery to redraw (same as a non-timer wake)
    preferences.putString(PREFERENCES_FILENAME_KEY, "");
    Log_error("Battery critical: %d mV < %d mV; WiFi paused, re-check every %d s until >= %d mV",
              vbatt_mv, critical_mv, BATTERY_CRITICAL_RECHECK_S, critical_mv + hysteresis_mv);
    display_init();
    showMessageWithLogo(BATTERY_CRITICAL);
    display_sleep();
    break;
  case BATTERY_GUARD_HOLD:
    Log_info("Battery guard: still critical, screen already drawn");
    break;
  }
  Log_info("Battery guard: deep sleep %d s without WiFi", BATTERY_CRITICAL_RECHECK_S);
  guardDeepSleep(BATTERY_CRITICAL_RECHECK_S, false); // intentional sleep: clears the boot-loop guard
#endif
}

// Not sure if WiFiClientSecure checks the validity date of the certificate.
// Setting clock just to be sure...
/**
 * @brief Function to clock synchronization
 * @param none
 * @return none
 */
static bool setClock()
{
  bool sync_status = false;
  struct tm timeinfo;

  // gmtOffset shifts getLocalTime() for the on-screen timestamps; epoch
  // values (getTime, telemetry) remain UTC regardless
  configTime(TIME_GMT_OFFSET_SEC, TIME_DST_OFFSET_SEC, "time.google.com", "time.cloudflare.com");
  Log.info("%s [%d]: Time synchronization...\r\n", __FILE__, __LINE__);

  // Wait for time to be set
  if (getLocalTime(&timeinfo))
  {
    sync_status = true;
    Log.info("%s [%d]: Time synchronization succeed!\r\n", __FILE__, __LINE__);
  }
  else
  {
    Log.info("%s [%d]: Time synchronization failed...\r\n", __FILE__, __LINE__);
  }

  Log.info("%s [%d]: Current time - %s\r\n", __FILE__, __LINE__, asctime(&timeinfo));

  return sync_status;
}

/**
 * @brief Function to read the battery voltage
 * @param none
 * @return float voltage in Volts
 */
static float readBatteryVoltage(void)
{
#ifdef FAKE_BATTERY_VOLTAGE
  Log.warning("%s [%d]: FAKE_BATTERY_VOLTAGE is defined. Returning 4.2V.\r\n", __FILE__, __LINE__);
  return 4.2f;
#else
  #if defined(BOARD_XIAO_EPAPER_DISPLAY) || defined(BOARD_SEEED_RETERMINAL_E1001)
    pinMode(PIN_VBAT_SWITCH, OUTPUT);
    digitalWrite(PIN_VBAT_SWITCH, VBAT_SWITCH_LEVEL);
    delay(10); // Wait for the switch to stabilize
  #endif
    Log.info("%s [%d]: Battery voltage reading...\r\n", __FILE__, __LINE__);
    int32_t adc;
    int32_t sensorValue;

    adc = 0;
    analogRead(PIN_BATTERY); // This is needed to properly initialize the ADC BEFORE calling analogReadMilliVolts()
    for (uint8_t i = 0; i < 8; i++) {
      adc += analogReadMilliVolts(PIN_BATTERY);
    }
  #if defined(BOARD_XIAO_EPAPER_DISPLAY) || defined(BOARD_SEEED_RETERMINAL_E1001)
    digitalWrite(PIN_VBAT_SWITCH, (VBAT_SWITCH_LEVEL == HIGH ? LOW : HIGH));
  #endif
    sensorValue = (adc / 8) * 2;
    Log.info("%s [%d]: Battery sensorValue = %d\r\n", __FILE__, __LINE__, (int)sensorValue);
    float voltage = sensorValue / 1000.0;
    return voltage;
#endif // FAKE_BATTERY_VOLTAGE
}

/**
 * @brief Function to submit a log string to the API
 * @param log_buffer pointer to the buffer that contains log note
 * @return bool true if successful, false if failed
 */
bool submitLogString(const char *log_buffer)
{
  String api_key = "";
  if (preferences.isKey(PREFERENCES_API_KEY))
  {
    api_key = preferences.getString(PREFERENCES_API_KEY, PREFERENCES_API_KEY_DEFAULT);
    Log_info("%s key exists. Value - %s", PREFERENCES_API_KEY, api_key.c_str());
  }
  else
  {
    Log_info("%s key not exists.", PREFERENCES_API_KEY);
    return false;
  }

  LogApiInput input{api_key, log_buffer};
  return submitLogToApi(input, preferences.getString(PREFERENCES_API_URL, API_BASE_URL).c_str());
}

/**
 * @brief Function to store a log string locally
 * @param log_buffer pointer to the buffer that contains log note
 * @return bool true if successful, false if failed
 */
bool storeLogString(const char *log_buffer)
{
  LogStoreResult store_result = storedLogs.store_log(String(log_buffer));
  if (store_result.status != LogStoreResult::SUCCESS)
  {
    Log_error("Failed to store log: %s", store_result.message);
    return false;
  }
  return true;
}


uint32_t getTime(void)
{
  time_t now;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 200))
  {
    Log.info("%s [%d]: Failed to obtain time. \r\n", __FILE__, __LINE__);
    return (0);
  }
  time(&now);
  return now;
}

static void submitStoredLogs(void)
{
  if (WiFi.isConnected() == false)
  {
    Log_info("WiFi not connected; not submitting stored logs.");
    return;
  }
  String log = storedLogs.gather_stored_logs();

  String api_key = "";
  if (preferences.isKey(PREFERENCES_API_KEY))
  {
    api_key = preferences.getString(PREFERENCES_API_KEY, PREFERENCES_API_KEY_DEFAULT);
    Log.info("%s [%d]: %s key exists. Value - %s\r\n", __FILE__, __LINE__, PREFERENCES_API_KEY, api_key.c_str());
  }
  else
  {
    Log.error("%s [%d]: %s key not exists.\r\n", __FILE__, __LINE__, PREFERENCES_API_KEY);
  }

  bool submitLogToApiResult = false;
  if (log.length() > 0)
  {
    Log.info("%s [%d]: log string - %s\r\n", __FILE__, __LINE__, log.c_str());
    Log.info("%s [%d]: need to send the log\r\n", __FILE__, __LINE__);

    LogApiInput input{api_key, log.c_str()};
    submitLogToApiResult = submitLogToApi(input, preferences.getString(PREFERENCES_API_URL, API_BASE_URL).c_str());
  }
  else
  {
    Log.info("%s [%d]: no needed to send the log\r\n", __FILE__, __LINE__);
  }
  if (submitLogToApiResult == true)
  {
    storedLogs.clear_stored_logs();
  }
}

static void writeImageToFile(const char *name, uint8_t *in_buffer, size_t size)
{
  size_t res = filesystem_write_to_file(name, in_buffer, size);
  if (res != size)
  {
    Log_error_submit("File writing ERROR. Result - %d", res);
  }
  else
  {
    Log.info("%s [%d]: file %s writing success - %d bytes\r\n", __FILE__, __LINE__, name, res);
  }
}

static void writeSpecialFunction(SPECIAL_FUNCTION function)
{
  if (preferences.isKey(PREFERENCES_SF_KEY))
  {
    Log.info("%s [%d]: SF saved. Reading...\r\n", __FILE__, __LINE__);
    if ((SPECIAL_FUNCTION)preferences.getUInt(PREFERENCES_SF_KEY, 0) == function)
    {
      Log.info("%s [%d]: No needed to re-write\r\n", __FILE__, __LINE__);
    }
    else
    {
      Log.info("%s [%d]: Writing new special function\r\n", __FILE__, __LINE__);
      bool res = preferences.putUInt(PREFERENCES_SF_KEY, function);
      if (res)
        Log.info("%s [%d]: Written new special function successfully\r\n", __FILE__, __LINE__);
      else
        Log.error("%s [%d]: Writing new special function failed\r\n", __FILE__, __LINE__);
    }
  }
  else
  {
    Log.error("%s [%d]: SF not saved\r\n", __FILE__, __LINE__);
    bool res = preferences.putUInt(PREFERENCES_SF_KEY, function);
    if (res)
      Log.info("%s [%d]: Written new special function successfully\r\n", __FILE__, __LINE__);
    else
      Log.error("%s [%d]: Writing new special function failed\r\n", __FILE__, __LINE__);
  }
}

static void showMessageWithLogo(MSG message_type) {
  display_show_msg(storedLogoOrDefault(0), message_type);
  need_to_refresh_display = 1;
  preferences.putBool(PREFERENCES_DEVICE_REGISTERED_KEY, false);
}

static void showRecoveryScreen(int battery_pct, float eta_days)
{
  display_show_recovery(storedLogoOrDefault(0), battery_pct, vBatt, eta_days);
  need_to_refresh_display = 1;
  preferences.putBool(PREFERENCES_DEVICE_REGISTERED_KEY, false);
}

static void showMessageWithLogo(MSG message_type, String friendly_id, bool id, const char *fw_version, String message)
{
  display_show_msg(storedLogoOrDefault(0), message_type, friendly_id, id, fw_version, message);
  need_to_refresh_display = 1;
  preferences.putBool(PREFERENCES_DEVICE_REGISTERED_KEY, false);
}

/**
 * @brief Show a message with the logo using data from API setup response
 * @param message_type Type of message to display
 * @param apiResponse The API setup response containing the message
 * @return none
 */
static void showMessageWithLogo(MSG message_type, const ApiSetupResponse &apiResponse)
{
  display_show_msg(storedLogoOrDefault(0), message_type, "", false, "", apiResponse.message);
  need_to_refresh_display = 1;
  preferences.putBool(PREFERENCES_DEVICE_REGISTERED_KEY, false);
}

// 0 = larger glyph, centered for message screens
// 1 = small glyph, set in lower-right corner for loading screen
static uint8_t *storedLogoOrDefault(int iType)
{
//  if (filesystem_read_from_file("/logo.bmp", buffer, DEFAULT_IMAGE_SIZE))
//  {
//    return buffer;
//  }
  if (iType == 0) {
    return const_cast<uint8_t *>(logo_small);
  } else {
    // Force the loading screen to always use the slower update method because
    // we don't know (yet) if the panel can handle the faster update modes
    apiDisplayResult.response.maximum_compatibility = true;
    return const_cast<uint8_t *>(loading);
  }
}

static bool saveCurrentFileName(String &name)
{
  if (!preferences.getString(PREFERENCES_FILENAME_KEY, "").equals(name))
  {
    Log.info("%s [%d]: New filename:  - %s\r\n", __FILE__, __LINE__, name.c_str());
    size_t res = preferences.putString(PREFERENCES_FILENAME_KEY, name);
    if (res > 0)
    {
      Log.info("%s [%d]: New filename saved in the preferences - %d\r\n", __FILE__, __LINE__, res);
      return true;
    }
    else
    {
      Log.error("%s [%d]: New filename saving error!\r\n", __FILE__, __LINE__);
      return false;
    }
  }
  else
  {
    Log.info("%s [%d]: No needed to re-write\r\n", __FILE__, __LINE__);
    return true;
  }
}

static bool checkCurrentFileName(String &newName)
{
  String currentFilename = preferences.getString(PREFERENCES_FILENAME_KEY, "");

  Log.error("%s [%d]: Current filename: %s\r\n", __FILE__, __LINE__, currentFilename);

  if (currentFilename.equals(newName))
  {
    Log.info("%s [%d]: Current filename equals to the new filename\r\n", __FILE__, __LINE__);
    return true;
  }
  else
  {
    Log.error("%s [%d]: Current filename doesn't equal to the new filename\r\n", __FILE__, __LINE__);
    return false;
  }
}

static void wifiErrorDeepSleep()
{
  if (!preferences.isKey(PREFERENCES_CONNECT_WIFI_RETRY_COUNT))
  {
    preferences.putInt(PREFERENCES_CONNECT_WIFI_RETRY_COUNT, 1);
  }

  uint8_t retry_count = preferences.getInt(PREFERENCES_CONNECT_WIFI_RETRY_COUNT);

  Log_info("WIFI connection failed! Retry count: %d \n", retry_count);

  switch (retry_count)
  {
  case 1:
    preferences.putUInt(PREFERENCES_SLEEP_TIME_KEY, WIFI_CONNECT_RETRY_TIME::WIFI_FIRST_RETRY);
    break;

  case 2:
    preferences.putUInt(PREFERENCES_SLEEP_TIME_KEY, WIFI_CONNECT_RETRY_TIME::WIFI_SECOND_RETRY);
    break;

  case 3:
    preferences.putUInt(PREFERENCES_SLEEP_TIME_KEY, WIFI_CONNECT_RETRY_TIME::WIFI_THIRD_RETRY);
    break;

  default:
    preferences.putUInt(PREFERENCES_SLEEP_TIME_KEY, SLEEP_TIME_TO_SLEEP);
    break;
  }
  retry_count++;
  preferences.putInt(PREFERENCES_CONNECT_WIFI_RETRY_COUNT, retry_count);

  display_sleep();
  goToSleep();
}

DeviceStatusStamp getDeviceStatusStamp()
{
  DeviceStatusStamp deviceStatus = {};

  deviceStatus.wifi_rssi_level = WiFi.RSSI();
  strncpy(deviceStatus.wifi_status, wifiStatusStr(WiFi.status()), sizeof(deviceStatus.wifi_status) - 1);
  deviceStatus.refresh_rate = preferences.getUInt(PREFERENCES_SLEEP_TIME_KEY);
  deviceStatus.time_since_last_sleep = time_since_sleep;
  snprintf(deviceStatus.current_fw_version, sizeof(deviceStatus.current_fw_version), "%s", FW_VERSION_STRING);
  parseSpecialFunctionToStr(deviceStatus.special_function, sizeof(deviceStatus.special_function), special_function);
  deviceStatus.battery_voltage = vBatt; //readBatteryVoltage()
  parseWakeupReasonToStr(deviceStatus.wakeup_reason, sizeof(deviceStatus.wakeup_reason), esp_sleep_get_wakeup_cause());
  deviceStatus.free_heap_size = ESP.getFreeHeap();
  deviceStatus.max_alloc_size = ESP.getMaxAllocHeap();

  return deviceStatus;
}

void logWithAction(LogAction action, const char *message, time_t time, int line, const char *file)
{
  uint32_t log_id = preferences.getUInt(PREFERENCES_LOG_ID_KEY, 1);

  LogWithDetails input = {
      .deviceStatusStamp = getDeviceStatusStamp(),
      .timestamp = time,
      .codeline = line,
      .sourceFile = file,
      .logMessage = message,
      .logId = log_id,
      .filenameCurrent = preferences.getString(PREFERENCES_FILENAME_KEY, ""),
      .filenameNew = new_filename,
      .logRetry = log_retry,
      .retryAttempt = log_retry ? preferences.getInt(PREFERENCES_CONNECT_API_RETRY_COUNT) : 0};

  String json_string = serialize_log(input);

  switch (action)
  {
    case LOG_ACTION_STORE:
      storeLogString(json_string.c_str());
      break;
    case LOG_ACTION_SUBMIT:
      submitLogString(json_string.c_str());
      break;
    case LOG_ACTION_SUBMIT_OR_STORE:
      if (!submitLogString(json_string.c_str()))
      {
        Log_info("Was unable to send log to API; saving locally for later.");
        storeLogString(json_string.c_str());
      }
      break;
  }

  preferences.putUInt(PREFERENCES_LOG_ID_KEY, ++log_id);
}

void log_nvs_usage()
{
  nvs_stats_t nv;
  esp_err_t ret = nvs_get_stats(NULL, &nv);
  if (ret == ESP_OK)
  {
    float percent = (float)nv.used_entries / (float)nv.total_entries * 100.0f;
    char percent_str[16];
    dtostrf(percent, 0, 2, percent_str); // 2 decimal places
    Log_info("NVS Usage: %d/%d entries (%s%%)", nv.used_entries, nv.total_entries, percent_str);
  }
  else
  {
    Log_error("Failed to get NVS stats: %s", esp_err_to_name(ret));
  }
}
