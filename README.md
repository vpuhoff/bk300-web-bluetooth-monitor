# BK300 decompile + Web/ESP32 Voltage Monitor

Этот репозиторий нужен для **реверс‑инжиниринга и документирования
BLE‑протокола KONNWEI BK300**, а также содержит **рабочие реализации
монитора напряжения** на нескольких платформах (Web Bluetooth, ESP32 Arduino
и ESP-IDF). Это полезно, когда хочется понимать, **как именно** устройство
общается по BLE, автоматизировать чтение данных или использовать BK300
**без/в дополнение** к мобильному приложению.

## Состав репозитория

- **`docs/spec.md`** — рабочая спецификация BLE‑протокола BK300:
  - GATT-профиль (`FFF0` / `FFF1` / `FFF2`),
  - формат кадра + CRC-16/PPP,
  - инициализационная цепочка (`0B06 → 0100 → 0B01 → 0B08 → 0B0B`),
  - подтверждённый путь чтения напряжения `0B0B → 4B0B` (`U16LE / 100`),
  - заметки про режим waveform.
- **`web-monitor/`** — веб‑приложение (Web Bluetooth, React + Vite + uPlot)
  для мониторинга напряжения в реальном времени с UI в стиле shadcn.
- **`esp32/`** — Arduino-скетч (`ESP32 / C3 / S3`, штатная `BLEDevice.h` из
  `arduino-esp32`) с Preferences-кэшем MAC и переподключением.
- **`esp32-idf/bk300_monitor/`** — нативный ESP-IDF проект на Bluedroid
  GATT-Client. **Это эталонная реализация** — самый тщательно проверенный
  вариант, повторяет HCI-сценарий Android-клиента 1:1. С `make rebuild`
  собирается и пакуется в zip с `flash.sh` / `flash.bat` за одну команду.
- **`tools/analyze_btsnoop.py`** — анализатор `btsnoop_hci.log` через pyshark
  (использовался для сопоставления Android-обмена с нашими реализациями).
- **`btsnoop_hci.log` / `btsnoop_hci.log.last`** — снимки HCI-обмена с
  Android-клиента, на которые опирается `docs/spec.md`.
- **`jadx_out/`** — декомпиляция оригинального Android-клиента BKmonitor
  (`com.jiawei.batteryonline`).

## Что такое BK300

**KONNWEI BK300** — это Bluetooth‑монитор автомобильного аккумулятора и
электросистемы, совместимый с батареями **6V / 12V / 24V**. Он собирает
данные о состоянии аккумулятора (в т.ч. напряжение), параметры пуска
(cranking), работы системы зарядки и записи поездок; используется вместе
с мобильным приложением **BKmonitor** (iOS/Android).
Источник: [страница продукта KONNWEI BK300](https://konnwei.com/product/463.html).

## Скриншот

![BK300 Voltage Monitor](./screenshot.png)

## Быстрый старт

### Веб-приложение (Web Bluetooth)

```bash
cd web-monitor
npm install
npm run dev
```

Требования: Chrome / Edge с поддержкой Web Bluetooth, контекст HTTPS
(или `http://localhost`). Подробнее — `web-monitor/README.md`.

### ESP32 (Arduino IDE)

См. `esp32/README.md`. Открой `esp32/bk300_oneshot_sleep.ino` в Arduino IDE,
выбери плату ESP32 / C3 / S3, прошей. В Serial увидишь подключение, kick и
`Voltage: XX.XX V` каждую секунду.

### ESP32 (ESP-IDF, нативный Bluedroid) — **рекомендуется**

Это эталон, который точно работает на ESP32-S3 + BK300 с прошивкой
из репозитория:

```bash
cd esp32-idf/bk300_monitor
make setup                       # один раз: apt-deps + ESP-IDF + тулчейны
make set-target TARGET=esp32s3
make rebuild                     # build + package в один заход
make PORT=/dev/ttyACM0 flash monitor
```

Если хочется только переносной zip, чтобы прошить с другого ПК:

```bash
make package                     # создаст dist/bk300_monitor-esp32s3-<sha>.zip
```

Внутри zip — бинари + `flash.sh` / `flash.bat` (обёртки вокруг esptool).
На целевой машине нужен только Python 3.8+; `esptool` подтянется
автоматически через `pip install --user`. Подробнее — `esp32-idf/bk300_monitor/README.md`.

## Главная находка реверса (для тех, кто будет повторять)

**Команда `0B01` в инициализационной цепочке требует реальный Unix-epoch
(`System.currentTimeMillis() / 1000`), а не uptime.** Если передать
заведомо мелкий timestamp (например `millis() / 1000` сразу после boot),
прибор молча игнорирует все последующие команды — `notify=0` стоит часами,
хотя CCCD-write возвращает status=ok. Лечится отправкой любого правдоподобного
epoch ≥ ~1.7e9. Подробности — в `docs/spec.md` (раздел «КРИТИЧНО: `0B01`
ожидает реальный Unix-epoch»).
