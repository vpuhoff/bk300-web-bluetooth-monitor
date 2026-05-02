/**
 * Точка входа: NVS, OLED, стек BT, регистрация колбэков BK300-драйвера.
 * Вся логика GAP/GATTC — в bk300_driver.c.
 *
 * Двойное нажатие BOOT (GPIO0) запускает WiFi+NTP синхронизацию:
 *   BLE останавливается → WiFi подключается → SNTP → WiFi выключается → BLE снова.
 */
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gattc_api.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "bk300_driver.h"
#include "ntp_sync.h"
#include "oled.h"

static const char *TAG = "main";

static void ble_start(void) {
  esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_bt_controller_init(&cfg));
  ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
  ESP_ERROR_CHECK(esp_bluedroid_init());
  ESP_ERROR_CHECK(esp_bluedroid_enable());

#if defined(CONFIG_BK300_BT_VERBOSE_LOGS) && CONFIG_BK300_BT_VERBOSE_LOGS
  esp_log_level_set("BT_HCI", ESP_LOG_DEBUG);
  esp_log_level_set("BT_GATT", ESP_LOG_DEBUG);
  esp_log_level_set("BT_BTM", ESP_LOG_INFO);
  esp_log_level_set("BT_L2CAP", ESP_LOG_INFO);
#else
  esp_log_level_set("BT_HCI", ESP_LOG_WARN);
  esp_log_level_set("BT_GATT", ESP_LOG_WARN);
  esp_log_level_set("BT_BTM", ESP_LOG_WARN);
  esp_log_level_set("BT_L2CAP", ESP_LOG_WARN);
#endif

  ESP_ERROR_CHECK(esp_ble_gap_register_callback(bk300_driver_gap_event_handler));
  ESP_ERROR_CHECK(esp_ble_gattc_register_callback(bk300_driver_gattc_event_handler));
  ESP_ERROR_CHECK(esp_ble_gattc_app_register(BK300_DRIVER_GATTC_APP_ID));
  esp_ble_gatt_set_local_mtu(247);
}

static void ble_stop(void) {
  esp_ble_gattc_app_unregister(BK300_DRIVER_GATTC_APP_ID);
  esp_bluedroid_disable();
  esp_bluedroid_deinit();
  esp_bt_controller_disable();
  esp_bt_controller_deinit();
}

static void ntp_sync_task(void *arg) {
  ESP_LOGI(TAG, "NTP sync requested — stopping BLE");
  oled_set_status("NTP sync...");

  bk300_driver_stop();
  ble_stop();

  bool ok = ntp_sync_run();

  if (ok) {
    oled_set_status("Time synced!");
    ESP_LOGI(TAG, "NTP sync complete, epoch=%lld", (long long)ntp_sync_get_epoch());
  } else {
    oled_set_status("NTP failed");
    ESP_LOGW(TAG, "NTP sync failed, using fallback epoch");
  }

  vTaskDelay(pdMS_TO_TICKS(2000));

  ESP_LOGI(TAG, "Restarting BLE");
  oled_set_status("Init BLE");
  ble_start();
  bk300_driver_restart();

  vTaskDelete(NULL);
}

static void boot_button_task(void *arg) {
  gpio_config_t io = {
      .pin_bit_mask = 1ULL << CONFIG_BK300_BOOT_BUTTON_GPIO,
      .mode         = GPIO_MODE_INPUT,
      .pull_up_en   = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type    = GPIO_INTR_DISABLE,
  };
  gpio_config(&io);

  TickType_t first_press_tick = 0;
  bool waiting_second = false;

  while (1) {
    bool pressed = (gpio_get_level(CONFIG_BK300_BOOT_BUTTON_GPIO) == 0);

    if (pressed) {
      // debounce
      vTaskDelay(pdMS_TO_TICKS(50));
      if (gpio_get_level(CONFIG_BK300_BOOT_BUTTON_GPIO) != 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }

      // wait for release
      while (gpio_get_level(CONFIG_BK300_BOOT_BUTTON_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
      }

      TickType_t now = xTaskGetTickCount();

      if (!waiting_second) {
        first_press_tick = now;
        waiting_second = true;
        ESP_LOGI(TAG, "BOOT: first press, waiting for double-press...");
      } else {
        uint32_t delta_ms = (now - first_press_tick) * portTICK_PERIOD_MS;
        waiting_second = false;
        if (delta_ms <= CONFIG_BK300_BOOT_DOUBLE_PRESS_MS) {
          ESP_LOGI(TAG, "BOOT: double press detected (%ldms) — starting NTP sync", (long)delta_ms);
          xTaskCreate(ntp_sync_task, "ntp_sync", 8192, NULL, 5, NULL);
        } else {
          ESP_LOGI(TAG, "BOOT: second press too late (%ldms), reset counter", (long)delta_ms);
        }
      }
    }

    // сбрасываем ожидание если окно истекло
    if (waiting_second) {
      uint32_t elapsed = (xTaskGetTickCount() - first_press_tick) * portTICK_PERIOD_MS;
      if (elapsed > CONFIG_BK300_BOOT_DOUBLE_PRESS_MS) {
        waiting_second = false;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void app_main(void) {
  esp_err_t r = nvs_flash_init();
  if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    r = nvs_flash_init();
  }
  ESP_ERROR_CHECK(r);

  oled_init();
  oled_set_status("Init BLE");

  ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
  ble_start();

  bk300_driver_start_stats_task();

  xTaskCreate(boot_button_task, "boot_btn", 2048, NULL, 3, NULL);

  ESP_LOGI(TAG, "BK300 OLED monitor (ESP-IDF / Bluedroid) initialized.");
}
