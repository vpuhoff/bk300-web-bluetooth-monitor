/**
 * BK300 BLE GATT client: GAP/GATTC callbacks, kick/recovery, notify parse.
 * Подробное описание сценария см. в комментарии к bk300_driver.c.
 */
#pragma once

#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"

/** app_id для esp_ble_gattc_app_register (должен совпадать с main). */
#define BK300_DRIVER_GATTC_APP_ID 0x55

void bk300_driver_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);

void bk300_driver_gattc_event_handler(esp_gattc_cb_event_t event,
                                      esp_gatt_if_t gattc_if,
                                      esp_ble_gattc_cb_param_t *param);

/** Периодический лог статистики уведомлений (FreeRTOS task). */
void bk300_driver_start_stats_task(void);
