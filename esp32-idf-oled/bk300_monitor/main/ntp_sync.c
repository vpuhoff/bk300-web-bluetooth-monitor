/**
 * WiFi + SNTP time synchronisation for BK300 monitor.
 *
 * Called from a FreeRTOS task on double BOOT-button press.
 * WiFi is brought up, SNTP syncs the RTC, WiFi is torn down.
 * BLE must be stopped by caller before ntp_sync_run() and restarted after.
 */

#include "ntp_sync.h"

#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define BK300_FALLBACK_EPOCH 1772582400UL

static const char *TAG = "ntp_sync";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_events = NULL;
static int s_retry_count = 0;
#define WIFI_MAX_RETRY 5

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data) {
  if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (s_retry_count < WIFI_MAX_RETRY) {
      esp_wifi_connect();
      s_retry_count++;
      ESP_LOGI(TAG, "WiFi retry %d/%d", s_retry_count, WIFI_MAX_RETRY);
    } else {
      xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
      ESP_LOGW(TAG, "WiFi connect failed");
    }
  } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "WiFi connected, IP=" IPSTR, IP2STR(&ev->ip_info.ip));
    s_retry_count = 0;
    xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
  }
}

static bool wifi_start(void) {
  s_retry_count = 0;
  s_wifi_events = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                       wifi_event_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                       wifi_event_handler, NULL, NULL));

  wifi_config_t wcfg = {0};
  strlcpy((char *)wcfg.sta.ssid, CONFIG_BK300_WIFI_SSID, sizeof(wcfg.sta.ssid));
  strlcpy((char *)wcfg.sta.password, CONFIG_BK300_WIFI_PASS, sizeof(wcfg.sta.password));
  wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
  ESP_ERROR_CHECK(esp_wifi_start());

  EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                          WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                          pdFALSE, pdFALSE,
                                          pdMS_TO_TICKS(15000));
  if (bits & WIFI_CONNECTED_BIT) {
    return true;
  }
  ESP_LOGW(TAG, "WiFi not connected (bits=0x%lx)", (unsigned long)bits);
  return false;
}

static void wifi_stop(void) {
  esp_wifi_disconnect();
  esp_wifi_stop();
  esp_wifi_deinit();
  esp_event_loop_delete_default();
  if (s_wifi_events) {
    vEventGroupDelete(s_wifi_events);
    s_wifi_events = NULL;
  }
}

bool ntp_sync_time_is_valid(void) {
  time_t now = time(NULL);
  struct tm t;
  gmtime_r(&now, &t);
  return (t.tm_year + 1900) >= 2024;
}

time_t ntp_sync_get_epoch(void) {
  if (ntp_sync_time_is_valid()) return time(NULL);
  return (time_t)BK300_FALLBACK_EPOCH;
}

bool ntp_sync_run(void) {
  ESP_LOGI(TAG, "Starting NTP sync (SSID='%s' NTP='%s')",
           CONFIG_BK300_WIFI_SSID, CONFIG_BK300_NTP_SERVER);

  if (!wifi_start()) {
    ESP_LOGW(TAG, "WiFi failed — NTP sync aborted");
    wifi_stop();
    return false;
  }

  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, CONFIG_BK300_NTP_SERVER);
  esp_sntp_init();

  int timeout_s = CONFIG_BK300_NTP_TIMEOUT_S;
  bool synced = false;
  for (int i = 0; i < timeout_s * 2; i++) {
    if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      synced = true;
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  esp_sntp_stop();

  if (synced) {
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    ESP_LOGI(TAG, "NTP sync ok: epoch=%lld  %04d-%02d-%02d %02d:%02d:%02d UTC",
             (long long)now,
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
  } else {
    ESP_LOGW(TAG, "NTP sync timeout after %ds", timeout_s);
  }

  wifi_stop();
  return synced;
}
