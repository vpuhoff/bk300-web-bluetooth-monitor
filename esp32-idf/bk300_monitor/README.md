# BK300 monitor — ESP-IDF / Bluedroid

GATT-Client напрямую через `esp_ble_gattc_*` и `esp_ble_gap_*`.
BLE-стек **Bluedroid** — тот же, что у Android в `btsnoop_hci.log.last`.

Это эталонная реализация в репозитории — самый тщательно проверенный
вариант, повторяет HCI-сценарий Android-клиента 1:1. **Подтверждённо
работает** на ESP32-S3 + BK300, выдаёт стабильный поток `4B0B`-notify
с напряжением раз в секунду.

## Что внутри

- `main/main.c` — GAP/GATTC handlers, скан, MTU exchange,
  `register-for-notify`, kick, polling. Принудительно задаёт connection
  params (15–30 ms / latency 0 / 4 s) и фиксирует 1M PHY — без этого
  default-ы Bluedroid делают MTU exchange по 2 секунды.
- `main/bk300_protocol.[ch]` — CRC-16/PPP, builder и парсер кадров
  (общий с Arduino-вариантом).
- `sdkconfig.defaults` — Bluedroid включён, NimBLE выключен, BLE-only режим,
  BT_GATTC включён, BLE 4.2 legacy API включены, `LOG_MAXIMUM_LEVEL=DEBUG`
  для возможности включать HCI/GATT trace в runtime.
- `Makefile` — обёртка над `idf.py` со всеми удобными целями
  (см. ниже).
- `scripts/` — шаблоны `flash.sh` / `flash.bat` для переносного пакета
  (`make package`).

## Установка ESP-IDF (один раз)

ESP-IDF — это родной build-toolchain Espressif с `idf.py`. Под Linux/macOS/Windows.

### Ubuntu — через Makefile-обёртку (быстро)

```bash
cd esp32-idf/bk300_monitor
make doctor          # покажет, что и где не хватает
make setup           # apt-deps + clone esp-idf + install.sh (один раз)
make set-target TARGET=esp32s3
make rebuild         # build + package
make PORT=/dev/ttyACM0 flash monitor
```

Параметры можно переопределить в командной строке:
`make TARGET=esp32 PORT=/dev/ttyUSB0 flash`. Все цели и переменные —
`make help`.

### Windows

Скачать **ESP-IDF Tools Installer** для Windows: <https://dl.espressif.com/dl/esp-idf/>.
Берём свежую LTS, `v5.3` или `v5.4`. Инсталлятор сам ставит Python,
тулчейны и cmake.

После установки в Start-меню появится «**ESP-IDF 5.x CMD**» — это
PowerShell/CMD с уже настроенным окружением (`%IDF_PATH%`, переменные путей).
Все команды ниже надо запускать **из этого терминала**, иначе `idf.py`
его не найдёт.

### Linux/macOS — вручную (без Makefile)

```bash
git clone -b v5.3.2 --recurse-submodules https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf
./install.sh esp32,esp32s3,esp32c3
. ./export.sh   # каждый раз в новом терминале
```

## Ключевые цели Makefile

```bash
make help                   # все цели и параметры

make set-target TARGET=esp32s3
make build                  # incremental build (sdkconfig.defaults НЕ перечитывает)
make refresh-sdkconfig      # удалить sdkconfig + set-target (после правки defaults)
make rebuild                # = refresh-sdkconfig + build + package
make flash                  # idf.py -p $PORT flash
make monitor                # idf.py -p $PORT monitor
make flash-monitor          # flash + monitor одной командой
make package                # zip с бинарями + flash.sh/.bat для другого ПК
make package-clean          # удалить dist/
```

`make build` сам предупреждает, если `sdkconfig.defaults` свежее `sdkconfig`:

```
############################################################
## ВНИМАНИЕ: sdkconfig.defaults свежее sdkconfig.         ##
## Изменения в defaults НЕ попадут в текущий build!       ##
## Запусти:  make refresh-sdkconfig && make build         ##
## или одной командой: make rebuild                       ##
############################################################
```

## Сборка / прошивка вручную (без Makefile)

```bash
. ~/esp/esp-idf/export.sh        # активируй IDF-окружение
cd esp32-idf/bk300_monitor
idf.py set-target esp32s3        # или esp32, esp32c3 — под твою плату
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

После прошивки войдёт `idf.py monitor`. Чтобы выйти — `Ctrl+]`.

## Переносной пакет для прошивки с другого ПК

`make package` собирает `dist/bk300_monitor-<target>-<sha>.zip`, в котором
лежат:

```
flash.sh / flash.bat        — обёртки над esptool
monitor.sh / monitor.bat    — простой serial-монитор (pyserial miniterm)
flash.env                   — CHIP=esp32s3, PROJECT_NAME, BAUD, версия
flash_args                  — esptool args-файл (читается через @flash_args)
flasher_args.json           — полные параметры от idf.py
bootloader/bootloader.bin
partition_table/partition-table.bin
bk300_monitor.bin
README.md                   — инструкция по прошивке
```

На целевом ПК нужен только Python 3.8+; `esptool` и `pyserial` скрипты
доустановят сами через `pip install --user`. Распаковываешь zip и:

```bash
chmod +x flash.sh monitor.sh
./flash.sh /dev/ttyUSB0
./monitor.sh /dev/ttyUSB0
```

или на Windows:

```cmd
flash.bat COM5
monitor.bat COM5
```

## Что должно быть в логе при успешной работе

```
I (xxx) bk300: BK300 monitor (ESP-IDF / Bluedroid) initialized.
I (xxx) bk300: scan params set, starting scan for 15 s
I (xxx) bk300: Found BK300 at B3:00:69:77:51:08 (rssi=-72)
I (xxx) bk300: GATTC connect, conn_id=0, peer=B3:00:69:77:51:08
I (xxx) bk300: request conn params 15-30ms latency=0 to=4s, err=0
I (xxx) bk300: conn params updated: int=24 lat=0 to=400 status=0
I (xxx) bk300: MTU exchanged, status=0, mtu=247
I (xxx) bk300: FFF0 service found, handles 0x000E..0xFFFF
I (xxx) bk300: FFF1 char_handle=0x0010 props=0x10
I (xxx) bk300: FFF2 char_handle=0x0013 props=0x04
I (xxx) bk300: FFF1 CCCD handle=0x0011
I (xxx) bk300: register_for_notify ok, handle=0x0010
I (xxx) bk300: write_descr complete, status=0 handle=0x0011
I (xxx) bk300: kick: 0B06 00 -> 0100 -> 0B01 time -> 0B08 -> 0B0B
I (xxx) bk300: 0B01 timestamp = 1772582400 (real_time_ok=0)
I (xxx) bk300: TX FFF2 40 40 0E 00 0B 01 00 76 A7 69 3D 88 0D 0A
...
I (xxx) bk300: NOTIFY conn=0 handle=0x0010 is_notify=1 len=11
I (xxx) bk300: RX FFF1 24 24 0B 00 4B 06 00 A5 29 0D 0A
I (xxx) bk300: NOTIFY conn=0 handle=0x0010 is_notify=1 len=14
I (xxx) bk300: RX FFF1 24 24 0E 00 4B 0B 88 05 02 01 AA 3C 0D 0A
I (xxx) bk300: Voltage: 14.16 V
```

Если **ничего после `register_for_notify ok` / kick** — см. раздел
«Главная находка» ниже и `docs/spec.md`.

## Главная находка реверса (история)

Долгое время оба ESP32-варианта (NimBLE-Arduino, Arduino BLE shim,
и эта IDF-Bluedroid реализация) одинаково давали `notify_pkts=0` после
успешного `register_for_notify`. Симптомы выглядели как
«CCCD не подписался» — хотя в эфире `Write Request 01 00` был, ack
приходил, и Bluedroid status сообщал `0`.

**Причина оказалась в команде `0B01`** (set RTC). Android-клиент
шлёт туда `System.currentTimeMillis() / 1000` — настоящий Unix-epoch
(~1.7e9 на 2026-й год). Наши же реализации использовали `millis() / 1000`
или `esp_timer_get_time() / 1e6` — то есть секунды от **boot чипа**
(значения 0, 1, 2, ...). Прошивка BK300, видимо, валидирует timestamp
и считает такого клиента «из прошлого» — после чего **молчаливо**
игнорирует все последующие команды.

В этой реализации сейчас:

```c
time_t real_now = 0;
time(&real_now);
uint32_t sec = (real_now > 1700000000)
    ? (uint32_t)real_now
    : (uint32_t)BK300_FALLBACK_EPOCH;  // 1772582400 = 2026-03-04 UTC
```

Если на ESP32 есть Wi-Fi + SNTP, `time(NULL)` отдаст настоящее время.
Иначе — fallback-epoch, который раз в год можно обновить в `main.c`
(или передать через `idf.py build -DBK300_FALLBACK_EPOCH=...`).

Подробности — в `docs/spec.md`, раздел «КРИТИЧНО: `0B01` ожидает
реальный Unix-epoch».

## Гайд по основным API Bluedroid

- `esp_bt_controller_init` / `enable` — поднять контроллер (LL/PHY).
- `esp_bluedroid_init` / `enable` — поднять Host (Bluedroid).
- `esp_ble_gap_register_callback` — single-cb на все события скана/conn-params/security.
- `esp_ble_gattc_register_callback` — single-cb на все GATTC события.
- `esp_ble_gattc_app_register(app_id)` — регистрирует «приложение» (можно несколько профилей).
- `esp_ble_gap_set_scan_params` → `start_scanning` — скан.
- В `SCAN_RESULT_EVT` находишь нужного и `esp_ble_gap_stop_scanning`
  + `esp_ble_gattc_open`.
- В `OPEN_EVT`: `send_mtu_req` → `search_service` → `get_char_by_uuid`.
- `esp_ble_gattc_register_for_notify` регистрирует **только** локальный
  callback. CCCD `01 00` нужно писать вручную через
  `esp_ble_gattc_write_char_descr(..., WRITE_TYPE_RSP, ...)`.
- `esp_ble_gattc_write_char` с `WRITE_TYPE_NO_RSP` — это `ATT Write Command (0x52)`.

## Отладка

- `idf.py monitor --print_filter "*:I bk300:V"` — VERBOSE для нашего тэга, INFO для остальных.
- В `sdkconfig.defaults` уже стоит `CONFIG_BT_LOG_HCI_TRACE_LEVEL_DEBUG=y` +
  `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y`. В `main.c` через
  `esp_log_level_set("BT_HCI", ESP_LOG_DEBUG)` включается HCI-trace в Serial —
  будет видно LE Connection Update, Number Of Completed Packets, LE Meta
  события на низком уровне.
- `idf.py menuconfig` → `Component config` → `Bluetooth` → `Bluedroid Options`
  → `Stack log level` — можно поднимать/опускать уровень для отдельных
  подсистем (`HCI`, `BTM`, `GATT`, `L2CAP`).
