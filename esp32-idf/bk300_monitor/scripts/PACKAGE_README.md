# bk300_monitor — переносной пакет прошивки

Этот архив содержит готовые бинари ESP-IDF проекта `bk300_monitor` и
скрипты `flash.sh` / `flash.bat`, чтобы прошить устройство с другого
компа без полной установки ESP-IDF.

## Что внутри

```
flash.sh / flash.bat        — обёртки над esptool
monitor.sh / monitor.bat    — простой serial-монитор (pyserial miniterm)
flash.env                   — параметры (CHIP, базовая скорость и т.п.)
flash_args                  — args-файл для esptool (@flash_args)
flasher_args.json           — полные параметры от idf.py (для справки)
bootloader/bootloader.bin
partition_table/partition-table.bin
<project>.bin               — основной образ приложения
```

## Что нужно поставить на целевой машине

Только Python 3.8+ и pip.

- **Linux**: `sudo apt install python3 python3-pip` (плюс drivers от платы:
  CP210x / CH34x / FTDI обычно встроены в ядро, доступ через
  `sudo usermod -aG dialout $USER`).
- **Windows**: установить Python с [python.org](https://www.python.org/),
  при установке отметить "Add Python to PATH". Драйверы USB-UART (CP210x,
  CH340/CH341, FTDI) — с сайта производителя моста.
- **macOS**: `brew install python` (или Python с python.org), драйверы
  CP210x/CH34x по необходимости.

`esptool` и `pyserial` скрипты доустановят сами через `pip install --user`.

## Прошивка

### Linux / macOS

```bash
chmod +x flash.sh monitor.sh
./flash.sh                       # автоопределит /dev/ttyUSB0 или /dev/ttyACM0
./flash.sh /dev/ttyUSB0          # явный порт
./flash.sh /dev/ttyUSB0 921600   # порт + скорость
```

После прошивки:

```bash
./monitor.sh /dev/ttyUSB0
```

Выход из монитора — `Ctrl+]`.

### Windows

```cmd
flash.bat COM5
flash.bat COM5 921600
monitor.bat COM5
```

## Если что-то не работает

- **`esptool` не нашёлся / не ставится**: запусти вручную
  `python -m pip install --user esptool` и убедись, что `python` есть в PATH.
- **`Permission denied: /dev/ttyUSB0`** (Linux): добавь себя в группу
  dialout и перезайди в систему: `sudo usermod -aG dialout $USER`.
- **`A fatal error occurred: Failed to connect to ESP32`**: зажми BOOT,
  кратко нажми RESET, отпусти BOOT и запусти прошивку снова. Часть плат
  требует «ручной» bootloader-режим.
- **Не та платка**: открой `flash.env` и проверь `CHIP=esp32s3` (или какой
  у тебя), при необходимости поменяй и запусти прошивку заново.
- **Прошивка прошла, но устройство ничего не пишет**: проверь, что
  serial-монитор открыт на 115200 и порт тот же, что использовался для
  прошивки.
