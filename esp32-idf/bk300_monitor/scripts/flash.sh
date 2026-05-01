#!/usr/bin/env bash
# Прошивка bk300_monitor через esptool.
# Запуск: ./flash.sh [PORT] [BAUD]
#   PORT — путь к COM-порту (по умолчанию /dev/ttyUSB0 или /dev/ttyACM0).
#   BAUD — скорость (по умолчанию 460800).
#
# Требования: Python 3.8+ и пакет esptool (`pip install esptool`).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [ -f "flash.env" ]; then
    # shellcheck disable=SC1091
    source flash.env
fi

CHIP="${CHIP:-esp32s3}"
PROJECT_NAME="${PROJECT_NAME:-bk300_monitor}"

PORT="${1:-${PORT:-}}"
BAUD="${2:-${BAUD:-460800}}"

if [ -z "${PORT}" ]; then
    for cand in /dev/ttyUSB0 /dev/ttyACM0; do
        if [ -e "$cand" ]; then
            PORT="$cand"
            break
        fi
    done
fi

if [ -z "${PORT}" ]; then
    echo "ERROR: не найден COM-порт. Передай его аргументом: ./flash.sh /dev/ttyUSB0" >&2
    exit 1
fi

PYTHON="${PYTHON:-python3}"
if ! command -v "$PYTHON" >/dev/null 2>&1; then
    PYTHON="python"
fi
if ! command -v "$PYTHON" >/dev/null 2>&1; then
    echo "ERROR: не найден Python. Поставь Python 3.8+." >&2
    exit 1
fi

if ! "$PYTHON" -m esptool version >/dev/null 2>&1; then
    echo ">> esptool не установлен. Ставлю в --user..."
    "$PYTHON" -m pip install --user --upgrade esptool || {
        echo "ERROR: не удалось поставить esptool. Запусти вручную: $PYTHON -m pip install esptool" >&2
        exit 1
    }
fi

echo ">> CHIP=$CHIP PORT=$PORT BAUD=$BAUD"
echo ">> esptool write_flash @flash_args"

exec "$PYTHON" -m esptool \
    --chip "$CHIP" \
    -p "$PORT" \
    -b "$BAUD" \
    --before default_reset \
    --after hard_reset \
    write_flash \
    "@flash_args"
