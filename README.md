# BK300 decompile + Web Voltage Monitor

Репозиторий содержит:

- **`docs/spec.md`**: рабочая спецификация BLE‑протокола BK300 (включая подтверждённый путь чтения напряжения `0B0B → 4B0B`).
- **`web-monitor/`**: веб‑приложение (Web Bluetooth) для мониторинга напряжения в реальном времени, UI в стиле **shadcn** + график **uPlot**.

## Скриншот

![BK300 Voltage Monitor](./screenshot.png)

## Запуск веб‑приложения

```bash
cd web-monitor
npm install
npm run dev
```

Требования:

- Chrome / Edge с поддержкой Web Bluetooth
- HTTPS (или `http://localhost`)

