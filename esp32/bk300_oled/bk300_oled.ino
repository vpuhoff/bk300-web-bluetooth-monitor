/**
 * BK300 voltage monitor with on-board SSD1306 OLED display (ESP32-C3 / ESP32 / S3, Arduino).
 *
 * BLE-логика — ровно как в `esp32/bk300_oneshot_sleep.ino` (тот же протокол: kick
 * 0B06 → 0100 → 0B01 <Unix-epoch LE> → 0B08 → 0B0B; периодический 0B0B; 4B0B = U16LE/100).
 * Поверх неё — вывод текущего напряжения и статуса на 128x64 SSD1306.
 *
 * Зависимости (Library Manager):
 *  - Adafruit GFX Library
 *  - Adafruit SSD1306
 *
 * Пины I2C (по умолчанию для ESP32-C3 dev-board):
 *  - SDA = GPIO 8
 *  - SCL = GPIO 9
 *  Поправь #define I2C_SDA / I2C_SCL под свою плату при необходимости.
 *
 * Settings в Arduino IDE:
 *  - Tools → Partition Scheme: Huge APP (3MB)  ← Bluedroid + GFX могут не влезть в default.
 *  - Tools → CPU Frequency: 160/240 MHz; Flash 4MB QIO 80MHz.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <Preferences.h>
#include <time.h>

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ВАЖНО: Arduino IDE инжектит прототипы функций сразу после includes,
// до любых пользовательских объявлений типов. Поэтому `struct Bk300Frame`
// (которую использует bk300PopFrame() как параметр) обязана быть объявлена
// здесь, ВЫШЕ всех функций. Иначе компилятор ругнётся на
// «'Bk300Frame' was not declared in this scope» в авто-сгенерированном
// прототипе bk300PopFrame.
struct Bk300Frame {
  uint16_t length;
  uint16_t typeLE;       // bytes [4..5] as u16le
  const uint8_t* data;
  const uint8_t* payload;
  uint16_t payloadLen;
  bool crcOk;
};

// ============================================================
// ===                  OLED конфиг                         ===
// ============================================================
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1
#define SCREEN_ADDRESS  0x3C
#define I2C_SDA         8   // ESP32-C3 dev-board: SDA на GPIO8.
#define I2C_SCL         9   // ESP32-C3 dev-board: SCL на GPIO9.
#define OLED_ROTATION   2   // 0 = native, 2 = 180° rotate (зависит от монтажа экрана).

static Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Состояние UI. Меняем флажок dirty из любого места — обновим в loop().
static float    g_displayVoltage = -1.0f;       // < 0 → ещё не получали значения
static String   g_displayStatus  = "Booting...";
static bool     g_displayHasOled = false;
static bool     g_displayDirty   = true;
static uint32_t g_displayLastMs  = 0;
static uint32_t g_displayLastVoltMs = 0;        // для индикатора «свежести»
static const uint32_t DISPLAY_MIN_REFRESH_MS = 200;
static const uint32_t DISPLAY_FORCE_REFRESH_MS = 1000;

static void displaySetStatus(const String& s) {
  if (g_displayStatus != s) {
    g_displayStatus = s;
    g_displayDirty = true;
  }
}

static void displaySetVoltage(float v) {
  // Считаем «значимым изменением» сдвиг ≥ 0.01 V — иначе мерцание.
  if (g_displayVoltage < 0.0f || fabsf(g_displayVoltage - v) >= 0.01f) {
    g_displayVoltage = v;
    g_displayDirty = true;
  }
  g_displayLastVoltMs = millis();
}

static void renderDisplay() {
  if (!g_displayHasOled) return;

  display.clearDisplay();

  // ── 1) Top status bar ─────────────────────────────────────
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("BK300: ");
  display.print(g_displayStatus);

  display.drawLine(0, 12, SCREEN_WIDTH, 12, SSD1306_WHITE);

  // ── 2) Big voltage в центре ───────────────────────────────
  display.setTextSize(3);
  display.setCursor(0, 22);
  if (g_displayVoltage >= 0.0f) {
    display.print(g_displayVoltage, 2);
    display.setTextSize(2);
    display.print(" V");
  } else {
    display.print("--.-- V");
  }

  // ── 3) Bottom mini-stats ──────────────────────────────────
  // строка 54..63: uptime + индикатор свежести данных + кол-во полученных кадров
  display.setTextSize(1);
  display.setCursor(0, 56);
  uint32_t up = millis() / 1000;
  if (up < 60) {
    display.print(up);
    display.print("s");
  } else if (up < 3600) {
    display.print(up / 60);
    display.print("m");
    display.print(up % 60);
    display.print("s");
  } else {
    display.print(up / 3600);
    display.print("h");
    display.print((up % 3600) / 60);
    display.print("m");
  }

  // справа: «sec since last 4B0B» — чтобы видеть, не «зависло» ли значение
  if (g_displayLastVoltMs != 0) {
    uint32_t age = (millis() - g_displayLastVoltMs) / 1000;
    char buf[16];
    if (age < 100) {
      snprintf(buf, sizeof(buf), "rx %us", (unsigned)age);
    } else {
      snprintf(buf, sizeof(buf), "rx>99s");
    }
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
    display.setCursor(SCREEN_WIDTH - (int)w, 56);
    display.print(buf);
  }

  display.display();
}

// Обновлять не чаще DISPLAY_MIN_REFRESH_MS и не реже DISPLAY_FORCE_REFRESH_MS.
static void displayTick() {
  if (!g_displayHasOled) return;
  uint32_t now = millis();
  bool needRefresh = false;
  if (g_displayDirty && (now - g_displayLastMs) >= DISPLAY_MIN_REFRESH_MS) needRefresh = true;
  if ((now - g_displayLastMs) >= DISPLAY_FORCE_REFRESH_MS) needRefresh = true;
  if (!needRefresh) return;

  renderDisplay();
  g_displayDirty = false;
  g_displayLastMs = now;
}

// ============================================================
// ===                  Frame parser/utils                  ===
// ============================================================
// (struct Bk300Frame объявлена в начале файла — см. комментарий там.)

static const char*    PREF_NS         = "bk300_oled";
static const char*    PREF_KEY_MAC    = "mac";
static const char*    PREF_KEY_TYPE   = "mac_type";
static const uint32_t SCAN_DURATION_S = 15;
static const uint32_t POLL_MS         = 1000;

static BLEUUID SERVICE_UUID("0000fff0-0000-1000-8000-00805f9b34fb");
static BLEUUID NOTIFY_UUID("0000fff1-0000-1000-8000-00805f9b34fb");
static BLEUUID WRITE_UUID("0000fff2-0000-1000-8000-00805f9b34fb");
static BLEUUID CCCD_UUID((uint16_t)0x2902);

static uint16_t crc16ppp(const uint8_t* data, size_t len) {
  uint16_t fcs = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    fcs ^= data[i];
    for (int b = 0; b < 8; b++) {
      if (fcs & 0x0001) fcs = (fcs >> 1) ^ 0x8408;
      else fcs >>= 1;
    }
  }
  return (uint16_t)(fcs ^ 0xFFFF);
}

static size_t buildFrame(uint8_t* out, size_t outCap,
                         uint16_t cmdLE,
                         const uint8_t* payload, size_t payloadLen) {
  const uint16_t length = (uint16_t)(payloadLen + 10);
  if (outCap < length) return 0;
  out[0] = 0x40; out[1] = 0x40;
  out[2] = (uint8_t)(length & 0xFF);
  out[3] = (uint8_t)((length >> 8) & 0xFF);
  out[4] = (uint8_t)(cmdLE & 0xFF);
  out[5] = (uint8_t)((cmdLE >> 8) & 0xFF);
  for (size_t i = 0; i < payloadLen; i++) out[6 + i] = payload[i];
  const uint16_t crc = crc16ppp(out, length - 4);
  out[length - 4] = (uint8_t)(crc & 0xFF);
  out[length - 3] = (uint8_t)((crc >> 8) & 0xFF);
  out[length - 2] = 0x0D;
  out[length - 1] = 0x0A;
  return length;
}

// ============================================================
// ===                  RX buffer / counters                ===
// ============================================================
static uint8_t  rxBuf[256];
static size_t   rxLen = 0;

static uint32_t c_notifyPackets  = 0;
static uint32_t c_notifyBytes    = 0;
static uint32_t c_rxOverflows    = 0;
static uint32_t c_framesTotal    = 0;
static uint32_t c_framesBadCrc   = 0;
static uint32_t c_frames4b0b     = 0;
static uint32_t c_connectOk      = 0;
static uint32_t c_connectFail    = 0;
static uint32_t c_disconnects    = 0;
static uint32_t c_pollWriteFail  = 0;
static uint32_t lastStatsMs      = 0;
static const uint32_t STATS_EVERY_MS = 5000;

static void rxAppend(const uint8_t* data, size_t len) {
  if (len == 0) return;
  if (rxLen + len > sizeof(rxBuf)) { rxLen = 0; c_rxOverflows++; }
  memcpy(rxBuf + rxLen, data, len);
  rxLen += len;
}

static int findTerminator() {
  for (size_t i = 0; i + 1 < rxLen; i++) {
    if (rxBuf[i] == 0x0D && rxBuf[i + 1] == 0x0A) return (int)i;
  }
  return -1;
}

static bool bk300PopFrame(Bk300Frame* outFrame, uint8_t* tmp, size_t tmpCap, size_t* tmpLenOut) {
  if (!outFrame || !tmp || !tmpLenOut) return false;
  int termIdx = findTerminator();
  if (termIdx < 0) return false;
  size_t frameLen = (size_t)termIdx + 2;
  if (frameLen > rxLen) return false;
  if (frameLen > tmpCap) {
    memmove(rxBuf, rxBuf + frameLen, rxLen - frameLen);
    rxLen -= frameLen;
    return false;
  }

  memcpy(tmp, rxBuf, frameLen);
  *tmpLenOut = frameLen;
  memmove(rxBuf, rxBuf + frameLen, rxLen - frameLen);
  rxLen -= frameLen;

  if (frameLen < 10) return false;
  const bool ok4040 = (tmp[0] == 0x40 && tmp[1] == 0x40);
  const bool ok2424 = (tmp[0] == 0x24 && tmp[1] == 0x24);
  if (!ok4040 && !ok2424) return false;

  uint16_t length = (uint16_t)(tmp[2] | (tmp[3] << 8));
  if (length != frameLen) return false;

  uint16_t typeLE = (uint16_t)(tmp[4] | (tmp[5] << 8));
  uint16_t payloadLen = (uint16_t)(length - 10);
  const uint8_t* payload = tmp + 6;

  uint16_t crcIn = (uint16_t)(tmp[length - 4] | (tmp[length - 3] << 8));
  uint16_t crcCalc = crc16ppp(tmp, length - 4);

  outFrame->length = length;
  outFrame->typeLE = typeLE;
  outFrame->data = tmp;
  outFrame->payload = payload;
  outFrame->payloadLen = payloadLen;
  outFrame->crcOk = (crcIn == crcCalc);
  return true;
}

// ============================================================
// ===                   BLE state                          ===
// ============================================================
static BLEClient*               client     = nullptr;
static BLERemoteCharacteristic* notifyChr  = nullptr;
static BLERemoteCharacteristic* writeChr   = nullptr;
static bool      kickDone           = false;
static uint32_t  lastPollMs         = 0;
static uint32_t  reconnectAttempt   = 0;
static uint32_t  nextReconnectAtMs  = 0;
static Preferences prefs;
static volatile bool isConnectedFlag = false;

static bool nameIsBk300Str(const String& name) {
  String s(name);
  s.trim();
  s.toLowerCase();
  if (s.length() == 0) return false;
  return s == "bk300" || s.startsWith("bk300");
}

static bool advertisesBk300Service(BLEAdvertisedDevice* dev) {
  if (!dev) return false;
  return dev->haveServiceUUID() && dev->getServiceUUID().equals(SERVICE_UUID);
}

static bool writeFrameBk300NoRsp(BLERemoteCharacteristic* w, const uint8_t* data, size_t len) {
  if (!w || len == 0 || !isConnectedFlag) return false;
  if (!w->canWriteNoResponse()) return false;
  w->writeValue((uint8_t*)data, len, false);
  yield();
  delay(5);
  return true;
}

static bool writeCmdBytes(BLERemoteCharacteristic* w, uint8_t b0, uint8_t b1) {
  uint8_t frame[64];
  uint16_t cmdLE = (uint16_t)(b0 | (b1 << 8));
  size_t len = buildFrame(frame, sizeof(frame), cmdLE, nullptr, 0);
  if (len == 0) return false;
  return writeFrameBk300NoRsp(w, frame, len);
}

static bool writeCmdPayload(BLERemoteCharacteristic* w, uint8_t b0, uint8_t b1,
                            const uint8_t* payload, size_t payloadLen) {
  uint8_t frame[64];
  uint16_t cmdLE = (uint16_t)(b0 | (b1 << 8));
  size_t len = buildFrame(frame, sizeof(frame), cmdLE, payload, payloadLen);
  if (len == 0) return false;
  return writeFrameBk300NoRsp(w, frame, len);
}

static bool pollVoltage0b0b(BLERemoteCharacteristic* w) {
  if (!w) return false;
  if (writeCmdBytes(w, 0x0B, 0x0B)) return true;
  c_pollWriteFail++;
  return false;
}

static void bk300NotifyCb(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  c_notifyPackets++;
  c_notifyBytes += (uint32_t)len;
  rxAppend(data, len);
}

static void bk300AppendFFF1Read() {
  if (!notifyChr || !isConnectedFlag) return;
  if (!notifyChr->canRead()) return;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt != 0) delay(45);
    String rv = notifyChr->readValue();
    if (rv.length() == 0) continue;
    rxAppend((const uint8_t*)rv.c_str(), rv.length());
    return;
  }
}

// ---- Client callbacks ----
class BkClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient*) override {
    Serial.println("Connected.");
    isConnectedFlag = true;
    displaySetStatus("Connected");
  }
  void onDisconnect(BLEClient*) override {
    Serial.println("Disconnected.");
    c_disconnects++;
    isConnectedFlag = false;
    notifyChr = nullptr;
    writeChr = nullptr;
    kickDone = false;
    lastPollMs = 0;
    rxLen = 0;
    const uint32_t delayMs = min<uint32_t>(30000, 1000U * (1U << min<uint32_t>(reconnectAttempt, 5)));
    reconnectAttempt++;
    nextReconnectAtMs = millis() + delayMs;
    Serial.print("Reconnect in ");
    Serial.print(delayMs);
    Serial.println(" ms");
    displaySetStatus("Disconnected");
  }
};
static BkClientCallbacks clientCallbacks;

// ---- Scan ----
static BLEAdvertisedDevice scanLastDev;
static bool                scanLastDevValid = false;

class BkScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (advertisesBk300Service(&dev) || (dev.haveName() && nameIsBk300Str(dev.getName()))) {
      scanLastDev = dev;
      scanLastDevValid = true;
      BLEDevice::getScan()->stop();
    }
  }
};
static BkScanCallbacks scanCallbacks;

static bool scanForBk300() {
  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&scanCallbacks, false);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  scanLastDevValid = false;
  Serial.println("Scanning for BK300...");
  displaySetStatus("Scanning...");
  scan->start(SCAN_DURATION_S, false);
  scan->clearResults();
  return scanLastDevValid;
}

// ---- Connect ----
static bool ensureConnected() {
  if (!client) return false;
  if (isConnectedFlag) return true;
  if (millis() < nextReconnectAtMs) {
    // Показываем «countdown» в статусе.
    uint32_t remaining = (nextReconnectAtMs - millis()) / 1000;
    char buf[24];
    snprintf(buf, sizeof(buf), "Wait %us", (unsigned)remaining);
    displaySetStatus(buf);
    return false;
  }

  // 1) cached MAC
  String cachedMac = prefs.getString(PREF_KEY_MAC, "");
  const uint8_t cachedType = prefs.getUChar(PREF_KEY_TYPE, 0xFF);
  if (cachedMac.length() > 0) {
    Serial.print("Trying cached MAC: ");
    Serial.println(cachedMac);
    displaySetStatus("Reconnecting");
    bool ok = false;
    if (cachedType != 0xFF) {
      BLEAddress addr(cachedMac, cachedType);
      ok = client->connect(addr, cachedType);
    } else {
      BLEAddress addrRand(cachedMac, BLE_ADDR_RANDOM);
      ok = client->connect(addrRand, BLE_ADDR_RANDOM);
      if (!ok) {
        BLEAddress addrPub(cachedMac, BLE_ADDR_PUBLIC);
        ok = client->connect(addrPub, BLE_ADDR_PUBLIC);
      }
    }
    if (ok) { c_connectOk++; reconnectAttempt = 0; return true; }
    c_connectFail++;
  }

  // 2) scan + connect
  if (!scanForBk300() || !scanLastDevValid) {
    Serial.println("BK300 not found.");
    displaySetStatus("Not found");
    const uint32_t delayMs = min<uint32_t>(30000, 1000U * (1U << min<uint32_t>(reconnectAttempt, 5)));
    reconnectAttempt++;
    nextReconnectAtMs = millis() + delayMs;
    return false;
  }

  String mac = String(scanLastDev.getAddress().toString().c_str());
  uint8_t typ = (uint8_t)scanLastDev.getAddressType();
  prefs.putString(PREF_KEY_MAC, mac);
  prefs.putUChar(PREF_KEY_TYPE, typ);
  Serial.print("Saved MAC: ");
  Serial.println(mac);

  displaySetStatus("Connecting");
  if (!client->connect(&scanLastDev)) {
    c_connectFail++;
    Serial.println("Connect failed after scan.");
    displaySetStatus("Conn failed");
    const uint32_t delayMs = min<uint32_t>(30000, 1000U * (1U << min<uint32_t>(reconnectAttempt, 5)));
    reconnectAttempt++;
    nextReconnectAtMs = millis() + delayMs;
    return false;
  }
  c_connectOk++;
  reconnectAttempt = 0;
  return true;
}

// ---- Discovery + subscribe ----
static bool ensureCharacteristics() {
  if (!client || !isConnectedFlag) return false;
  if (notifyChr && writeChr) return true;

  Serial.print("ATT MTU=");
  Serial.println(client->getMTU());

  BLERemoteService* svc = client->getService(SERVICE_UUID);
  if (!svc) {
    Serial.println("Service FFF0 not found.");
    displaySetStatus("No FFF0 svc");
    client->disconnect();
    return false;
  }

  notifyChr = svc->getCharacteristic(NOTIFY_UUID);
  writeChr  = svc->getCharacteristic(WRITE_UUID);
  if (!notifyChr || !writeChr) {
    Serial.println("Chars FFF1/FFF2 not found.");
    displaySetStatus("No FFF1/FFF2");
    client->disconnect();
    return false;
  }

  if (notifyChr->canNotify() || notifyChr->canIndicate()) {
    notifyChr->registerForNotify(bk300NotifyCb);
  }

  delay(60);
  BLERemoteDescriptor* cccd = notifyChr->getDescriptor(CCCD_UUID);
  if (!cccd) {
    Serial.println("CCCD missing.");
    displaySetStatus("No CCCD");
    client->disconnect();
    return false;
  }
  static const uint8_t kEnableNotifyLE[] = {0x01, 0x00};
  cccd->writeValue((uint8_t*)kEnableNotifyLE, sizeof(kEnableNotifyLE), true);
  delay(120);

  return true;
}

static void ensureKick() {
  if (!writeChr || kickDone) return;
  delay(400);
  Serial.println("Init: 0B06 00 -> 0100 -> 0B01 time -> 0B08 -> 0B0B");
  displaySetStatus("Kicking...");

  static const uint8_t openVoltageOff[] = {0x00};
  writeCmdPayload(writeChr, 0x0B, 0x06, openVoltageOff, sizeof(openVoltageOff));
  delay(10);
  writeCmdBytes(writeChr, 0x01, 0x00);
  delay(700);

  // ВАЖНО: реальный Unix-epoch (System.currentTimeMillis()/1000), а НЕ uptime.
  // Если SNTP не настроен — берём фиксированное значение из BK300_FALLBACK_EPOCH.
  #ifndef BK300_FALLBACK_EPOCH
  #define BK300_FALLBACK_EPOCH 1772582400UL  // 2026-03-04 UTC
  #endif
  time_t realNow = 0;
  time(&realNow);
  uint32_t sec = (realNow > 1700000000) ? (uint32_t)realNow : (uint32_t)BK300_FALLBACK_EPOCH;
  Serial.print("0B01 timestamp = ");
  Serial.println(sec);
  const uint8_t sincePayload[] = {
      (uint8_t)(sec & 0xFF),
      (uint8_t)((sec >> 8) & 0xFF),
      (uint8_t)((sec >> 16) & 0xFF),
      (uint8_t)((sec >> 24) & 0xFF),
  };
  writeCmdPayload(writeChr, 0x0B, 0x01, sincePayload, sizeof(sincePayload));
  delay(100);
  writeCmdBytes(writeChr, 0x0B, 0x08);
  delay(50);
  kickDone = true;

  pollVoltage0b0b(writeChr);
  delay(100);
  pollVoltage0b0b(writeChr);
  delay(200);
  bk300AppendFFF1Read();
  lastPollMs = millis();
  displaySetStatus("Live");
}

static void drainFrames() {
  uint8_t tmp[256];
  size_t tmpLen = 0;
  Bk300Frame f{};

  while (bk300PopFrame(&f, tmp, sizeof(tmp), &tmpLen)) {
    c_framesTotal++;
    if (!f.crcOk) {
      c_framesBadCrc++;
      continue;
    }
    if (f.typeLE == 0x0B4B && f.payloadLen >= 2) {
      c_frames4b0b++;
      const uint16_t raw = (uint16_t)(f.payload[0] | (f.payload[1] << 8));
      const float volts = raw / 100.0f;
      Serial.print("Voltage: ");
      Serial.println(volts, 2);
      displaySetVoltage(volts);
    }
  }
}

// ============================================================
// ===                       SETUP                          ===
// ============================================================
void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) yield();
  delay(50);

  Serial.println("BK300 OLED monitor starting...");

  // ── OLED init (до BLE — тогда даже если BLE упадёт, увидим причину) ─
  Wire.begin(I2C_SDA, I2C_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed — OLED disabled."));
    g_displayHasOled = false;
  } else {
    g_displayHasOled = true;
    display.setRotation(OLED_ROTATION);
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("BK300 monitor");
    display.println("init...");
    display.display();
  }

  prefs.begin(PREF_NS, false);

  WiFi.mode(WIFI_OFF);

  BLEDevice::init("");
  BLEDevice::setMTU(247);
  BLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_DEFAULT);

  client = BLEDevice::createClient();
  client->setClientCallbacks(&clientCallbacks);

  rxLen = 0;
  reconnectAttempt = 0;
  nextReconnectAtMs = 0;
  lastPollMs = 0;
  kickDone = false;

#if defined(CONFIG_BT_NIMBLE_ENABLED)
  Serial.println("Backend: NimBLE-shim.");
#else
  Serial.println("Backend: Bluedroid.");
#endif

  displaySetStatus("Init BLE");
}

// ============================================================
// ===                       LOOP                           ===
// ============================================================
void loop() {
  if (!ensureConnected()) {
    displayTick();
    delay(50);
    return;
  }
  if (!ensureCharacteristics()) {
    displayTick();
    delay(50);
    return;
  }

  ensureKick();

  const uint32_t now = millis();
  if (now - lastPollMs >= POLL_MS) {
    lastPollMs = now;
    if (writeChr) {
      pollVoltage0b0b(writeChr);
      delay(100);
      pollVoltage0b0b(writeChr);
      delay(150);
      bk300AppendFFF1Read();
    }
  }

  drainFrames();

  if (now - lastStatsMs >= STATS_EVERY_MS) {
    lastStatsMs = now;
    Serial.print("stats: conn_ok=");      Serial.print(c_connectOk);
    Serial.print(" disc=");                Serial.print(c_disconnects);
    Serial.print(" notify_pkts=");         Serial.print(c_notifyPackets);
    Serial.print(" frames=");              Serial.print(c_framesTotal);
    Serial.print(" 4b0b=");                Serial.print(c_frames4b0b);
    Serial.print(" badcrc=");              Serial.print(c_framesBadCrc);
    Serial.print(" rxovf=");               Serial.println(c_rxOverflows);
  }

  displayTick();

  delay(5);
}
