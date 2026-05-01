/**
 * Минимальный SSD1306-драйвер + UI для проекта bk300_oled_monitor.
 *
 * Покрывает:
 *   - esp_lcd: esp_lcd_panel_io_i2c + esp_lcd_new_panel_ssd1306 (как в IDF i2c_oled);
 *   - локальный framebuffer 1024 байта (8 страниц × 128 столбцов);
 *   - простой text rendering на встроенном шрифте 5x7 (oled_font.h);
 *   - thread-safe API: oled_set_status / oled_set_voltage можно дёргать
 *     из любого таска / BLE-callback'а; настоящий рендер идёт в
 *     отдельной FreeRTOS-таске на vTaskDelay(100ms).
 *
 * Если SSD1306 не отвечает, oled_init() всё равно возвращает ESP_OK,
 * а модуль работает «без экрана» (см. лог).
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Инициализация драйвера + создание задачи рендера. Можно вызывать только
// один раз. Возвращает ESP_OK даже если экран не отвечает — в этом случае
// модуль молча работает «вхолостую» (вызовы oled_set_*() не делают I/O).
esp_err_t oled_init(void);

// Установить верхнюю строку статуса. Передаётся NUL-терминированная строка.
// Может быть вызвано из BLE-callback'а / таймера / любого таска.
void oled_set_status(const char *status);

// Установить отображаемое напряжение в вольтах. Передавай отрицательное
// значение (например -1.0f), чтобы показать «--.-- V».
void oled_set_voltage(float volts);

// Принудительно «пнуть» рендер — обычно не нужен, render-task сам
// перерисовывает раз в секунду + при каждом изменении статуса/вольтажа.
void oled_kick(void);

#ifdef __cplusplus
}
#endif
