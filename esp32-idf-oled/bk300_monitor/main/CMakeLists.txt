# Регистрируем компонент main. Добавляем зависимости от bt, nvs_flash, и
# esp_driver_i2c (для нового i2c_master API в IDF 5.2+, нужен SSD1306-OLED).
# `driver` оставлен в REQUIRES для совместимости со старыми сборками IDF —
# в новых релизах он реэкспортирует esp_driver_i2c автоматически.
idf_component_register(
    SRCS
        "main.c"
        "bk300_driver.c"
        "bk300_protocol.c"
        "oled.c"
        "ntp_sync.c"
    INCLUDE_DIRS "."
    REQUIRES
        bt
        nvs_flash
        driver
        esp_driver_i2c
        esp_lcd
        esp_timer
        esp_wifi
        esp_netif
        esp_event
        lwip
)
