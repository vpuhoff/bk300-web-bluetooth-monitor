# ESP32 (Arduino) — BK300 Serial monitor

`bk300_oneshot_sleep.ino` — пример для **ESP32 + Arduino + штатная
`BLEDevice.h` из ESP32-core**, который работает как «долго живущий монитор»
в Serial:

- сканирует BLE в поиске **KONNWEI BK300** (по сервису `FFF0` или имени `BK300`);
- подключается как **Central**, включает notifications на `FFF1` (через
  `registerForNotify` + страховочный явный Write Request `0x0100` в CCCD `0x2902`);
- отправляет инициализацию **как у Android в HCI**:
  `0B06 00 → 0100 → 0B01 <Unix-epoch LE> → 0B08 → 0B0B`;
- отправляет `0B0B` **раз в секунду** и парсит `4B0B`;
- парсит напряжение как `U16LE / 100.0` (поддерживаются префиксы кадров
  `40 40` и `24 24`);
- сохраняет **MAC + тип адреса BLE** в `Preferences`, чтобы переподключаться
  **без сканирования**.

Резервная копия предыдущей реализации на NimBLE-Arduino лежит в
`_legacy_nimble/bk300_oneshot_sleep.nimble.bak.cpp.txt` (текстовый файл —
не компилируется автоматически в составе скетча).

> **Эталонная реализация лежит в `esp32-idf/bk300_monitor/` (нативный
> ESP-IDF + Bluedroid).** Этот Arduino-вариант тоже работает, но в IDF-проекте
> точнее настроены connection params, PHY и log-уровни — если что-то не
> заводится, попробуй сначала IDF-вариант.

## Зависимости

- Плата: ESP32 / ESP32-C3 / ESP32-S3 (Arduino core для ESP32 **2.x или 3.x**)
- BLE-библиотека: штатная `BLE` из ESP32-core (`#include <BLEDevice.h>`).
  Отдельную NimBLE-Arduino ставить **не нужно** — иначе будет конфликт
  заголовков.
- ВАЖНО про backend BLE-стека:
  - На **ESP32 (классический, Xtensa dual-core)** библиотека `BLE` —
    это **Bluedroid**.
  - На **ESP32-C3 / S3** в arduino-esp32 3.x — это **NimBLE-shim** с
    тем же публичным API. То есть стек физически тот же, что у NimBLE-Arduino,
    просто другой wrapper. На уровне эфира должно быть идентично.

### Settings в Arduino IDE

- Tools → Partition Scheme: **Huge APP (3MB No OTA / 1MB SPIFFS)** —
  Bluedroid тяжелее NimBLE и стандартный 1.4MB партишен может не влезть.
- Tools → CPU Frequency: 240 MHz; Flash Size: 4MB; Flash Mode: QIO;
  Flash Frequency: 80 MHz.

## BLE UUID’ы (как в web-monitor / Android)

- Service: `0000fff0-0000-1000-8000-00805f9b34fb`
- Notify (RX): `0000fff1-0000-1000-8000-00805f9b34fb`
- Write (TX): `0000fff2-0000-1000-8000-00805f9b34fb`
- CCCD: `0x2902`

## Команды (из HCI Android `btsnoop_hci.log.last`)

- Init / kick: `0B06 00 → 0100 → 0B01 <u32 LE Unix-epoch> → 0B08 → 0B0B`
- Voltage poll: `0B0B` → ответ `4B0B`
  (первые 2 байта payload = `U16LE`, напряжение `V = raw / 100`)
- Все TX-кадры на FFF2 — строго `ATT Write Command (0x52)`, как у Android.

## КРИТИЧНО — `0B01` требует Unix-epoch, а не uptime

В этом скетче `0B01` шлёт `time(NULL)` (если на устройстве есть SNTP/RTC),
иначе — fallback `BK300_FALLBACK_EPOCH` (по умолчанию `1772582400`,
2026-03-04 UTC).

**Если эту правку убрать и слать туда `millis() / 1000`** (uptime от boot),
BK300 молча игнорирует все последующие команды: `Write Command` принимаются,
ack-и приходят, а `notify` от прибора **никогда не идут** — выглядит ровно
как «CCCD не подписался». Подробности — в `docs/spec.md`, раздел
«КРИТИЧНО: `0B01` ожидает реальный Unix-epoch».

Если у тебя на ESP32 есть Wi-Fi и SNTP — `time(NULL)` отдаст реальное время
автоматически. Если нет — раз в год можно поднимать `BK300_FALLBACK_EPOCH`
в исходнике, либо передавать через `-DBK300_FALLBACK_EPOCH=...` в
build-флагах.

## Что должно быть в Serial при успешной работе

```
Connected. ATT MTU=247
GATT caps: FFF1 notify=1 ind=0 rd_prop=0 | FFF2 write=0 norsp=1
FFF1 CCCD 0x2902: ok
Init (Android HCI): 0B06 00 -> 0100 -> 0B01 time -> 0B08 -> 0B0B
0B01 timestamp = 1772582400 (real_time_ok=0)
Poll: 0B0B (initial x2)
Voltage: 12.45 V
Voltage: 12.46 V
...
```

Если `notify=0` стоит и не двигается:

1. Убедись, что в Serial есть строка `0B01 timestamp = NNNNNNNNNN` с
   большим числом (≥ ~1.7e9). Если число маленькое — это и есть проблема.
2. Проверь, что в эфире от ESP32 был `Write Request 01 00` именно на
   CCCD `0x2902` у `FFF1` (по сниферу или Android-логу).
3. Попробуй ESP-IDF-вариант (`esp32-idf/bk300_monitor/`) — он точнее
   воспроизводит HCI-обмен Android (явные conn params, 1M PHY, и т.п.).
