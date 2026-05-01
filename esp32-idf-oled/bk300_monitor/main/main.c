/**
 * Точка входа: NVS, OLED, стек BT, регистрация колбэков BK300-драйвера.
 * Вся логика GAP/GATTC — в bk300_driver.c.
 */
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gattc_api.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "bk300_driver.h"
#include "oled.h"

static const char *TAG = "main";

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

  bk300_driver_start_stats_task();

  ESP_LOGI(TAG, "BK300 OLED monitor (ESP-IDF / Bluedroid) initialized.");
}
