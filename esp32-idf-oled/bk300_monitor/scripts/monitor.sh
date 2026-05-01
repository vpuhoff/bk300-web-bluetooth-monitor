#!/usr/bin/env bash
# Лёгкий serial-монитор без IDF (через pyserial miniterm).
# Запуск: ./monitor.sh [PORT] [BAUD]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [ -f "flash.env" ]; then
    # shellcheck disable=SC1091
    source flash.env
fi

PORT="${1:-${PORT:-}}"
BAUD="${2:-${MONITOR_BAUD:-115200}}"

if [ -z "${PORT}" ]; then
    for cand in /dev/ttyUSB0 /dev/ttyACM0; do
        if [ -e "$cand" ]; then
            PORT="$cand"
            break
        fi
    done
fi

if [ -z "${PORT}" ]; then
    echo "ERROR: укажи порт: ./monitor.sh /dev/ttyUSB0" >&2
    exit 1
fi

PYTHON="${PYTHON:-python3}"
if ! command -v "$PYTHON" >/dev/null 2>&1; then
    PYTHON="python"
fi

if ! "$PYTHON" -c "import serial.tools.miniterm" >/dev/null 2>&1; then
    echo ">> ставлю pyserial..."
    "$PYTHON" -m pip install --user --upgrade pyserial
fi

echo ">> miniterm $PORT @ $BAUD (Ctrl+] для выхода)"
exec "$PYTHON" -m serial.tools.miniterm --raw "$PORT" "$BAUD"
