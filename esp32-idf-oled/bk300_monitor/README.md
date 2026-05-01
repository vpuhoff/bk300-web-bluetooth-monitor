# BK300 OLED monitor — ESP-IDF / Bluedroid + SSD1306

Это **OLED-вариант** эталонной IDF-реализации `esp32-idf/bk300_monitor`.
Та же BLE-логика 1-в-1 (тот же протокол, тот же Unix-epoch fix, те же
connection params 15–30 ms / 1M PHY), плюс встроенный мини-драйвер
SSD1306 128×64 поверх нового `i2c_master` API.

Project name: **`bk300_oled_monitor`** (бинарь и архивы выходят с этим
префиксом — не путать с базовым `bk300_monitor`).

Если хочешь только Serial-вывод без экрана — бери базовый
`esp32-idf/bk300_monitor`. Этот проект добавляет мини-драйвер OLED
(`main/oled.[ch]`, `main/oled_font.h`, `main/Kconfig.projbuild`) и
держит BLE-логику BK300 в отдельном модуле **`main/bk300_driver.[ch]`**;
в **`main/main.c`** остаётся только NVS, подъём контроллера/Bluedroid,
регистрация колбэков драйвера и старт stats-задачи. Все вызовы
`oled_set_*` живут внутри `bk300_driver.c` (фазы скана, коннекта, kick,
разбор notify).

## Что показывает экран

```
┌────────────────────────────────┐
│ BK300: Live                    │   ← статус (5x7, scale 1)
│────────────────────────────────│
│                                │
│  12.45 V                       │   ← напряжение (5x7, scale 4 + " V" scale 3)
│                                │
│ 2m17s                rx 1s     │   ← uptime + age последнего 4B0B
└────────────────────────────────┘
```

Статусы, которые меняются автоматически (через `oled_set_status()`):

| Статус | Когда |
| --- | --- |
| `Init BLE` | в `app_main`, после `oled_init`, до `esp_bluedroid_enable` |
| `Scanning...` | сразу после `ESP_GATTC_REG_EVT` |
| `Not found` | scan завершился без BK300 в эфире |
| `Connecting` | scan нашёл устройство, идёт `esp_ble_gattc_open` |
| `Conn failed` | `OPEN_EVT.status != OK`, идём в backoff |
| `Connected` | GATTC connect ok |
| `Kicking...` | шлём `0B06 00 → 0100 → 0B01 → 0B08 → 0B0B` |
| `Live` | пришёл первый `4B0B` после kick |
| `Disconnected` | потеряли GATT-сессию |
| `Reconnecting` | стартуем повторный скан после disconnect |

В правом нижнем углу — счётчик секунд **с момента последнего полученного
`4B0B`-кадра**. Если он растёт выше 5–10 секунд — связь подвисла,
обычно после этого происходит таймаут и переход в `Reconnecting`.

## Hardware / pins

По умолчанию:

| Параметр | Default | Где менять |
| --- | --- | --- |
| I²C SDA | GPIO 8 | `idf.py menuconfig` → `BK300 OLED options` |
| I²C SCL | GPIO 9 | то же |
| I²C frequency | 400 kHz | то же |
| SSD1306 address | 0x3C | то же |
| Rotate 180° | yes | то же |

Все пять опций живут в `main/Kconfig.projbuild`. Запусти
`idf.py menuconfig` и найди раздел **«BK300 OLED options»** в самом
верху. Можно сразу переопределить через CLI:

```bash
idf.py menuconfig                          # графический выбор
# или нон-интерактивно:
echo "CONFIG_BK300_OLED_I2C_SDA_GPIO=21" >> sdkconfig.defaults
echo "CONFIG_BK300_OLED_I2C_SCL_GPIO=22" >> sdkconfig.defaults
make refresh-sdkconfig                     # перечитать defaults
```

Дефолты выставлены под **ESP32-C3 dev-board** (8/9 — стандарт
Espressif). Для классического ESP32 (WROOM/WROVER) поставь 21/22.

## OLED: `err=259`, I²C и шумные логи Bluetooth

### `err=259` / `I2C transaction failed`, а по BLE данные идут

Код **259** в логе — `ESP_ERR_INVALID_STATE`: синхронная I²C-транзакция
не закончилась в состоянии `DONE`. Часто это **NACK** (провода, pull-up,
перепутанные SDA/SCL, неверный `0x3C` / `0x3D`). Отдельный нюанс ESP-IDF:
`i2c_master_probe()` проверяет адрес **всегда на 100 kHz**, а передачи
идут на частоте из Kconfig. Раньше дефолт был **400 kHz** — probe проходил,
первый же реальный пакет на 400 kHz ловил NACK → `init sequence err=259`.

Сейчас в проекте:

1. Дефолт `BK300_OLED_I2C_FREQ_HZ` = **100000**.
2. Если в menuconfig стоит выше (например 400000), при ошибке автоматически
   повторяют инициализацию на **100 kHz**.
3. `BK300_OLED_TRY_ALT_ADDR`: при неудаче пробуют второй адрес **0x3C ↔ 0x3D**.

После `make refresh-sdkconfig` / `make rebuild` в логе ищи строку
`SSD1306 ok: ...` — там будут **фактические** addr и freq.

### Меньше шума от `BT_HCI` / `BT_GATT` в UART

По умолчанию выключено (`menuconfig` → **BK300 logging** →
*Verbose BT_HCI / BT_GATT logs* = **No**). Включай только для отладки стека.

### Нет физического экрана или так и не завёлся I²C

Если все попытки инициализации OLED неудачны — шина освобождается, в логе
warning, приложение продолжает работу **как Serial-only** (`oled_set_*` без I²C).

Полностью выключить OLED: `BK300 OLED options → Enable SSD1306 OLED display (n)`.

## Сборка

Идентична базовому проекту:

```bash
cd esp32-idf-oled/bk300_monitor
make doctor                       # проверка окружения
make setup                        # один раз: apt-deps + clone esp-idf + install.sh
make set-target TARGET=esp32c3    # под твою плату
make rebuild                      # refresh-sdkconfig + build + package
make PORT=/dev/ttyACM0 flash monitor
```

`make package` положит архив в `dist/bk300_oled_monitor-<target>-<sha>.zip`
с `flash.sh` / `flash.bat` / `flash.env` — полный набор для прошивки
с другого ПК. Содержимое архива — то же, что у базового проекта,
только бинарь называется `bk300_oled_monitor.bin`.

## Что нового по сравнению с `esp32-idf/bk300_monitor`

| Файл | Что |
| --- | --- |
| `main/bk300_driver.h` / `bk300_driver.c` | весь GAP/GATTC-клиент BK300 (скан, open, kick, notify, recovery), публичные `bk300_driver_gap_event_handler` / `bk300_driver_gattc_event_handler`, `BK300_DRIVER_GATTC_APP_ID`, `bk300_driver_start_stats_task()` |
| `main/main.c` | тонкий `app_main`: NVS, OLED init, BT/Bluedroid, уровни логов, регистрация драйвера, MTU 247, stats |
| `main/oled.h` / `oled.c` | мини-драйвер SSD1306 на `driver/i2c_master.h` (новое API IDF 5.2+) + 1024-байт framebuffer + render-task на 100 ms tick |
| `main/oled_font.h` | компактный 5×7 ASCII-шрифт (95 глифов × 5 байт ≈ 0.5 KB ROM) |
| `main/Kconfig.projbuild` | пять Kconfig-опций для пинов / частоты / адреса / поворота |
| `main/CMakeLists.txt` | + `oled.c`, `bk300_driver.c` и зависимости (`driver`, `esp_driver_i2c`, `esp_timer`) |
| `CMakeLists.txt` | `project(bk300_oled_monitor)` |
| `Makefile` | `PROJECT_NAME=bk300_oled_monitor` |

## Архитектура OLED-секции

- **API:** `oled_set_status(const char*)` и `oled_set_voltage(float)`
  можно дёргать **из любого таска / BLE-callback'а**. Они защищены
  внутренним mutex'ом и просто обновляют state + dirty-флаг.
- **I²C-IO:** идёт **только** в выделенной FreeRTOS-таске `oled_render`
  с `vTaskDelay(100)` между тиками. Это важно: одна транзакция I²C на
  1024 байта при 400 kHz занимает ~25 ms, и звать её из BLE-callback'а
  опасно — может вытолкнуть HCI-нотификации в overflow.
- **Throttling:** перерисовываем не чаще `MIN_REFRESH_MS = 200` и не
  реже `FORCE_REFRESH_MS = 1000` (чтобы footer с uptime двигался).
- **Шрифт:** один глиф 5×7 px, между ними 1 пустой столбец (advance = 6).
  Масштабирование scale=2..4 — простой пиксельный nearest-neighbor.

## Отладка

```
I (xxx) oled: SSD1306 init ok @ I2C0 sda=8 scl=9 addr=0x3C freq=400000 Hz
I (xxx) main: BK300 OLED monitor (ESP-IDF / Bluedroid) initialized.
I (xxx) bk300: scan params set, starting scan for 15 s
I (xxx) bk300: Found BK300 at B3:00:69:77:51:08 (rssi=-72)
I (xxx) bk300: GATTC connect, conn_id=0, peer=B3:00:69:77:51:08
...
I (xxx) bk300: Voltage: 12.45 V
```

Если в логе **нет** `I (xxx) oled: SSD1306 init ok` — значит экран
не отвечает по I²C. Самые частые причины:

1. Перепутаны SDA / SCL (смотри `Kconfig` defaults vs реальную распайку).
2. Не тот I²C-адрес (`0x3C` vs `0x3D`). Перемычка на модуле.
3. Нет питания / GND на модуле.
4. Длинная линия без подтяжек: `enable_internal_pullup` даёт ~45 kΩ —
   для коротких проводов хватает, для длинных нужны внешние 4.7 kΩ
   к 3.3 V (на большинстве китайских модулей они уже есть).

Если ничего не помогает — выключи OLED через menuconfig
(`BK300 OLED options → Enable SSD1306 OLED display = n`), пересобери,
проверь что Serial-логика работает. Это сужает место поиска.

---

Всё остальное (что такое kick, как работает протокол `4B0B`, как
устроен Bluedroid GATT-Client) — см. README **базового** проекта
`esp32-idf/bk300_monitor/README.md` и `docs/spec.md`.
