/**
 * BK300 voltage monitor (ESP32 / ESP32-C3, Arduino) — на штатной библиотеке `BLE` из arduino-esp32.
 *
 * На ESP32 (классический) внутри Bluedroid; на ESP32-C3 в arduino-esp32 3.x внутри NimBLE-shim,
 * но публичный API (BLEDevice / BLEClient / BLERemoteCharacteristic) одинаков. Сравнение по HCI
 * с Android (см. btsnoop_hci.log.last) показало: достаточно Write Request 0x0001 в CCCD и
 * последовательности 0B06 00 → 0100 → 0B01 <time LE> → 0B08 → 0B0B, чтобы BK300 начал слать HVN.
 *
 * Flow:
 *  1) BLEDevice::init / setMTU(247) / setPower P9
 *  2) сканирование (active) BK300 по сервису FFF0 либо по имени; кэш MAC + addr_type в Preferences
 *  3) connect(BLEAddress, type) — conn params на дефолтах (на C3 публичного API для них нет)
 *  4) getService(FFF0) → getCharacteristic(FFF1, FFF2)
 *  5) registerForNotify(...) (библиотека сама пишет 0001 в CCCD), плюс наш страховочный
 *     явный writeValue 01 00 в дескриптор 0x2902 (как Write Request)
 *  6) Init как у Android HCI: 0B06 00 -> 0100 -> 0B01 <time LE> -> 0B08 -> 0B0B
 *  7) Каждую секунду 0B0B; парсер 40 40 / 24 24; печать V = U16LE/100
 */

#include <Arduino.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <time.h>
// На ESP32-C3 / arduino-esp32 3.x штатная библиотека BLE собрана без Bluedroid (NimBLE-shim),
// поэтому типы вроде esp_ble_addr_type_t / esp_bd_addr_t недоступны. Используем uint8_t[6] и
// NimBLE-style константы BLE_ADDR_PUBLIC / BLE_ADDR_RANDOM (они exposed через BLEAddress.h).
#include <Preferences.h>

// Объявление структуры до первого использования (Arduino IDE инжектит прототипы после #includes).
struct Bk300Frame {
  uint16_t length;
  uint16_t typeLE;      // bytes [4..5] as u16le
  const uint8_t* data;  // pointer into tmp buffer
  const uint8_t* payload;
  uint16_t payloadLen;
  bool crcOk;
};

// ---- Constants ----
static const char* PREF_NS = "bk300";
static const char* PREF_KEY_MAC = "mac";
static const char* PREF_KEY_MAC_TYPE = "mac_type"; // ble_addr type byte (PUBLIC=0, RANDOM=1, ...)
static const uint32_t SCAN_DURATION_S = 15;        // BLEScan::start у Bluedroid — в секундах!
static const uint32_t POLL_MS = 1000;

static BLEUUID SERVICE_UUID("0000fff0-0000-1000-8000-00805f9b34fb");
static BLEUUID NOTIFY_UUID("0000fff1-0000-1000-8000-00805f9b34fb");
static BLEUUID WRITE_UUID("0000fff2-0000-1000-8000-00805f9b34fb");
static BLEUUID CCCD_UUID((uint16_t)0x2902);

// CRC-16/PPP (CRC-16/X25 reflected), poly=0x8408, init=0xFFFF, xorout=0xFFFF.
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

static size_t buildFrame(uint8_t* out,
                         size_t outCap,
                         uint16_t cmdLE /* bytes [4..5] */,
                         const uint8_t* payload,
                         size_t payloadLen) {
  // prefix(2) + len(2) + cmd(2) + payload + crc(2) + 0D0A(2)
  const uint16_t length = (uint16_t)(payloadLen + 10);
  if (outCap < length) return 0;
  out[0] = 0x40;
  out[1] = 0x40;
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

// 0301 frame is hard-coded in the Android app; mirrored by web-monitor:
// 40 40 0A 00 03 01 33 D3 0D 0A
static const uint8_t FRAME_0301[] = {0x40, 0x40, 0x0A, 0x00, 0x03, 0x01, 0x33, 0xD3, 0x0D, 0x0A};

// ---- RX buffer / counters ----
static uint8_t rxBuf[256];
static size_t rxLen = 0;

static uint32_t c_notifyPackets = 0;
static uint32_t c_notifyBytes = 0;
static uint32_t c_rxOverflows = 0;
static uint32_t c_framesTotal = 0;
static uint32_t c_framesBadCrc = 0;
static uint32_t c_frames4b0b = 0;
static uint32_t c_connectOk = 0;
static uint32_t c_connectFail = 0;
static uint32_t c_scanRuns = 0;
static uint32_t c_scanFound = 0;
static uint32_t c_scanBleReports = 0;
static uint32_t c_pollWriteFail = 0;
static uint32_t c_gattReadFFF1 = 0;
static uint32_t c_rdAttempts = 0;
static bool dbgPrinted0b0bHex = false;
static uint32_t c_disconnects = 0;
static uint32_t lastStatsMs = 0;
static const uint32_t STATS_EVERY_MS = 5000;

static void rxAppend(const uint8_t* data, size_t len) {
  if (len == 0) return;
  if (rxLen + len > sizeof(rxBuf)) {
    rxLen = 0;
    c_rxOverflows++;
  }
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

// ---- BLE state ----
static BLEClient* client = nullptr;
static BLERemoteCharacteristic* notifyChr = nullptr;
static BLERemoteCharacteristic* writeChr = nullptr;
static bool kickDone = false;
static uint32_t lastPollMs = 0;
static uint32_t reconnectAttempt = 0;
static uint32_t nextReconnectAtMs = 0;
static Preferences prefs;
static volatile bool isConnectedFlag = false;
static uint8_t lastPeerBda[6] = {0};
static volatile bool lastPeerBdaValid = false;

// arduino-esp32 3.x BLE отдаёт имена/значения как Arduino String (не std::string).
static bool nameIsBk300Str(const String& name) {
  String s(name);
  s.trim();
  s.toLowerCase();
  if (s.length() == 0) return false;
  return s == "bk300" || s.startsWith("bk300");
}

static bool advertisesBk300Service(BLEAdvertisedDevice* dev) {
  if (!dev) return false;
  if (dev->haveServiceUUID() && dev->getServiceUUID().equals(SERVICE_UUID)) return true;
  return false;
}

static void hexPrint(const char* label, const uint8_t* data, size_t len) {
  Serial.print(label);
  if (len == 0) {
    Serial.println("(empty)");
    return;
  }
  for (size_t i = 0; i < len; i++) {
    if (i) Serial.print(" ");
    uint8_t b = data[i];
    if (b < 16) Serial.print("0");
    Serial.print(b, HEX);
  }
  Serial.println();
}

// FFF2 — только Write Command (0x52), как в HCI Android.
static bool writeFrameBk300NoRsp(BLERemoteCharacteristic* w,
                                 const uint8_t* data,
                                 size_t len,
                                 const char* dbgTag) {
  if (!w || len == 0 || !isConnectedFlag) return false;
  if (!w->canWriteNoResponse()) {
    Serial.print("FFF2 has no WRITE_NO_RESPONSE? tag=");
    Serial.println(dbgTag ? dbgTag : "");
    return false;
  }
  w->writeValue((uint8_t*)data, len, false);  // Bluedroid: response=false → ATT Write Command
  yield();
  delay(5);
  return true;
}

static void bk300NotifyCb(BLERemoteCharacteristic* /*chr*/, uint8_t* data, size_t len, bool /*isNotify*/) {
  c_notifyPackets++;
  c_notifyBytes += (uint32_t)len;
  rxAppend(data, len);
}

static bool pollVoltage0b0b(BLERemoteCharacteristic* w, const char* tag) {
  if (!w) return false;
  uint8_t frame[64];
  const uint16_t cmdLE = (uint16_t)(0x0B | (0x0B << 8));
  size_t len = buildFrame(frame, sizeof(frame), cmdLE, nullptr, 0);
  if (len == 0) return false;
  if (writeFrameBk300NoRsp(w, frame, len, tag)) return true;
  c_pollWriteFail++;
  Serial.print("0B0B write failed ");
  Serial.println(tag);
  return false;
}

static void dbgPrintBuilt0b0bOnce() {
  if (dbgPrinted0b0bHex) return;
  uint8_t f[32];
  const uint16_t cmdLE = (uint16_t)(0x0B | (0x0B << 8));
  size_t n = buildFrame(f, sizeof(f), cmdLE, nullptr, 0);
  hexPrint("CHK 0B0B frame ", f, n);
  dbgPrinted0b0bHex = true;
}

static bool writeCmdBytes(BLERemoteCharacteristic* w, uint8_t b0, uint8_t b1) {
  uint8_t frame[64];
  uint16_t cmdLE = (uint16_t)(b0 | (b1 << 8));
  size_t len = buildFrame(frame, sizeof(frame), cmdLE, nullptr, 0);
  if (len == 0) return false;
  return writeFrameBk300NoRsp(w, frame, len, "writeCmdBytes");
}

static bool writeCmdPayload(BLERemoteCharacteristic* w,
                            uint8_t b0,
                            uint8_t b1,
                            const uint8_t* payload,
                            size_t payloadLen,
                            const char* tag) {
  uint8_t frame[64];
  uint16_t cmdLE = (uint16_t)(b0 | (b1 << 8));
  size_t len = buildFrame(frame, sizeof(frame), cmdLE, payload, payloadLen);
  if (len == 0) return false;
  return writeFrameBk300NoRsp(w, frame, len, tag);
}

static void bk300AppendFFF1Read() {
  if (!notifyChr || !isConnectedFlag) return;
  // FFF1 у BK300 без READ-property (rd_prop=0). ATT Read даст "Read Not Permitted" — не спамим.
  if (!notifyChr->canRead()) return;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt != 0) delay(45);
    c_rdAttempts++;
    String rv = notifyChr->readValue();
    if (rv.length() == 0) continue;
    c_gattReadFFF1++;
    rxAppend((const uint8_t*)rv.c_str(), rv.length());
    return;
  }
}

// ---- Client callbacks ----
class BkClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient* c) override {
    Serial.println("Connected.");
    isConnectedFlag = true;
    BLEAddress peer = c->getPeerAddress();
    memcpy(lastPeerBda, peer.getNative(), sizeof(lastPeerBda));
    lastPeerBdaValid = true;
    // Conn params: на arduino-esp32 3.x (C3) нет публичного esp_ble_gap_update_conn_params,
    // и BLEClient::setConnectionParams в этой версии тоже не выставлен. Идём на дефолтах.
  }
  void onDisconnect(BLEClient*) override {
    Serial.println("Disconnected.");
    c_disconnects++;
    isConnectedFlag = false;
    lastPeerBdaValid = false;
    notifyChr = nullptr;
    writeChr = nullptr;
    kickDone = false;
    lastPollMs = 0;
    rxLen = 0;
    c_rdAttempts = 0;
    c_gattReadFFF1 = 0;

    const uint32_t delayMs = min<uint32_t>(30000, 1000U * (1U << min<uint32_t>(reconnectAttempt, 5)));
    reconnectAttempt++;
    nextReconnectAtMs = millis() + delayMs;
    Serial.print("Reconnect in ");
    Serial.print(delayMs);
    Serial.println(" ms");
  }
};
static BkClientCallbacks clientCallbacks;

// ---- Scan ----
static BLEAdvertisedDevice scanLastDev;
static bool scanLastDevValid = false;

class BkScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    c_scanBleReports++;
    if (advertisesBk300Service(&dev) || (dev.haveName() && nameIsBk300Str(dev.getName()))) {
      scanLastDev = dev;
      scanLastDevValid = true;
      c_scanFound++;
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

  c_scanBleReports = 0;
  c_scanRuns++;
  scanLastDevValid = false;
  Serial.println("Scanning for BK300...");

  const uint32_t tScan0 = millis();
  // Bluedroid: длительность скана в секундах. start блокирует до окончания / scan->stop().
  scan->start(SCAN_DURATION_S, false);
  const uint32_t scanElapsed = millis() - tScan0;
  Serial.print("Scan finished in ");
  Serial.print(scanElapsed);
  Serial.println(" ms");
  scan->clearResults();
  return scanLastDevValid;
}

// ---- Connect ----
static bool ensureConnected() {
  if (!client) return false;
  if (isConnectedFlag) return true;
  if (millis() < nextReconnectAtMs) return false;

  // 1) cached MAC
  String cachedMac = prefs.getString(PREF_KEY_MAC, "");
  const uint8_t cachedType = prefs.getUChar(PREF_KEY_MAC_TYPE, 0xFF);
  if (cachedMac.length() > 0) {
    Serial.print("Trying cached MAC: ");
    Serial.println(cachedMac);
    bool ok = false;
    const uint32_t t0 = millis();

    // arduino-esp32 3.x: BLEAddress(const String&, uint8_t type), BLEClient::connect(addr, uint8_t).
    // Имена констант: BLE_ADDR_PUBLIC=0, BLE_ADDR_RANDOM=1 (NimBLE-style, без _TYPE_ в середине).
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

    if (ok) {
      c_connectOk++;
      reconnectAttempt = 0;
      return true;
    }
    c_connectFail++;
    Serial.print("Cached MAC connect failed (");
    Serial.print(millis() - t0);
    Serial.println(" ms)");
  }

  // 2) scan + connect
  if (!scanForBk300() || !scanLastDevValid) {
    Serial.println("BK300 not found.");
    Serial.print("BLE reports this scan: ");
    Serial.println(c_scanBleReports);
    if (c_scanBleReports == 0) {
      Serial.println("Подсказка: 0 отчётов — помехи/coexistence (Wi-Fi), антенна, или BK300 занят.");
    }
    const uint32_t delayMs = min<uint32_t>(30000, 1000U * (1U << min<uint32_t>(reconnectAttempt, 5)));
    reconnectAttempt++;
    nextReconnectAtMs = millis() + delayMs;
    return false;
  }

  String mac = String(scanLastDev.getAddress().toString().c_str());
  uint8_t typ = (uint8_t)scanLastDev.getAddressType();
  prefs.putString(PREF_KEY_MAC, mac);
  prefs.putUChar(PREF_KEY_MAC_TYPE, typ);
  Serial.print("Saved MAC: ");
  Serial.println(mac);
  Serial.print("Saved MAC type: ");
  Serial.println(typ);

  if (!client->connect(&scanLastDev)) {
    c_connectFail++;
    Serial.println("Connect failed after scan.");
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

  const uint32_t t0 = millis();

  // Bluedroid выполняет MTU exchange в client->connect(). На всякий случай выводим.
  Serial.print("ATT MTU=");
  Serial.println(client->getMTU());

  Serial.println("Direct discovery: getService(FFF0) → FFF1/FFF2.");
  BLERemoteService* svc = client->getService(SERVICE_UUID);
  if (!svc) {
    Serial.println("Service FFF0 not found.");
    client->disconnect();
    return false;
  }

  notifyChr = svc->getCharacteristic(NOTIFY_UUID);
  writeChr = svc->getCharacteristic(WRITE_UUID);
  if (!notifyChr || !writeChr) {
    Serial.println("Chars FFF1/FFF2 not found.");
    client->disconnect();
    return false;
  }

  Serial.print("GATT caps: FFF1 notify=");
  Serial.print(notifyChr->canNotify());
  Serial.print(" ind=");
  Serial.print(notifyChr->canIndicate());
  Serial.print(" rd_prop=");
  Serial.print(notifyChr->canRead());
  Serial.print(" | FFF2 write=");
  Serial.print(writeChr->canWrite());
  Serial.print(" norsp=");
  Serial.println(writeChr->canWriteNoResponse());

  // 1) registerForNotify: Bluedroid пишет 0001 в CCCD автоматически и регистрирует HVN-handler.
  //    Сигнатура у разных версий ESP32-core отличается; вызываем без флагов — defaults=true,true.
  if (notifyChr->canNotify() || notifyChr->canIndicate()) {
    notifyChr->registerForNotify(bk300NotifyCb);
    Serial.println("registerForNotify(): submitted.");
  } else {
    Serial.println("FFF1: нет ни notify, ни indicate — ответы по BLE могут не приходить.");
  }

  // 2) Страховочная явная запись CCCD как Write Request 01 00 (как в HCI Android frame 402).
  //    Не читаем CCCD обратно: Android этого не делает, а лишний ATT Read у BK300 может сбивать
  //    состояние (на эту прошивку и без того жалуются на странности).
  delay(60);
  BLERemoteDescriptor* cccd = notifyChr->getDescriptor(CCCD_UUID);
  Serial.print("FFF1 CCCD 0x2902: ");
  Serial.println(cccd ? "ok" : "MISSING");
  if (!cccd) {
    Serial.println("CCCD не найдена — отключение.");
    client->disconnect();
    return false;
  }
  static const uint8_t kEnableNotifyLE[] = {0x01, 0x00};
  cccd->writeValue((uint8_t*)kEnableNotifyLE, sizeof(kEnableNotifyLE), true);
  Serial.println("Explicit CCCD Write Request 01 00 sent.");
  delay(120);

  Serial.print("GATT setup total ");
  Serial.print(millis() - t0);
  Serial.println(" ms (svc/desc + subscribe/CCCD)");

  return true;
}

static void ensureKick() {
  if (!writeChr || kickDone) return;
  dbgPrintBuilt0b0bOnce();
  delay(400);
  Serial.println("Init (Android HCI): 0B06 00 -> 0100 -> 0B01 time -> 0B08 -> 0B0B");

  // MainActivityNew.notifySuccess(): sendOpenVoltage(0) → 0B06 00, затем 0100.
  static const uint8_t openVoltageOff[] = {0x00};
  writeCmdPayload(writeChr, 0x0B, 0x06, openVoltageOff, sizeof(openVoltageOff), "0B06 00");
  delay(10);
  writeCmdBytes(writeChr, 0x01, 0x00);
  delay(700);

  // sendGetLastData(): payload = currentTimeMillis()/1000 LE.
  // ВАЖНО: это Unix-epoch (как в Android), а НЕ uptime millis()/1000.
  // BK300 может валидировать timestamp и игнорировать клиента «из прошлого».
  // Если SNTP не настроен — берём фиксированное значение из BK300_FALLBACK_EPOCH.
  #ifndef BK300_FALLBACK_EPOCH
  #define BK300_FALLBACK_EPOCH 1772582400UL  // 2026-03-04 UTC
  #endif
  time_t realNow = 0;
  time(&realNow);
  uint32_t sec = (realNow > 1700000000) ? (uint32_t)realNow : (uint32_t)BK300_FALLBACK_EPOCH;
  Serial.print("0B01 timestamp = ");
  Serial.print(sec);
  Serial.print(" (real_time_ok=");
  Serial.print(realNow > 1700000000 ? 1 : 0);
  Serial.println(")");
  const uint8_t sincePayload[] = {
      (uint8_t)(sec & 0xFF),
      (uint8_t)((sec >> 8) & 0xFF),
      (uint8_t)((sec >> 16) & 0xFF),
      (uint8_t)((sec >> 24) & 0xFF),
  };
  writeCmdPayload(writeChr, 0x0B, 0x01, sincePayload, sizeof(sincePayload), "0B01 time");
  delay(100);
  writeCmdBytes(writeChr, 0x0B, 0x08);
  delay(50);
  kickDone = true;

  Serial.println("Poll: 0B0B (initial x2)");
  pollVoltage0b0b(writeChr, "(initial #1)");
  delay(100);
  pollVoltage0b0b(writeChr, "(initial #2)");
  delay(200);
  bk300AppendFFF1Read();
  lastPollMs = millis();
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
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(50);

  prefs.begin(PREF_NS, false);

  // Wi-Fi + BLE на одном радио иногда «глушат» скан BLE; для отладки централа Wi-Fi не нужен.
  WiFi.mode(WIFI_OFF);

  BLEDevice::init("");
  BLEDevice::setMTU(247);
  BLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_DEFAULT);
  // SMP/encryption у BK300 в HCI нет — ничего не настраиваем (Bluedroid по умолчанию JustWorks no-bond).

  client = BLEDevice::createClient();
  client->setClientCallbacks(&clientCallbacks);

  rxLen = 0;
  reconnectAttempt = 0;
  nextReconnectAtMs = 0;
  lastPollMs = 0;
  kickDone = false;

  // На ESP32 (классический, core 2.x) бинарник идёт через Bluedroid; на C3/S3/в core 3.x — NimBLE-shim.
  // Печатаем стек, чтобы по логу было видно что запущено.
#if defined(CONFIG_BT_NIMBLE_ENABLED)
  Serial.println("BK300 monitor started (BLE@core, NimBLE backend).");
#else
  Serial.println("BK300 monitor started (BLE@core, Bluedroid backend).");
#endif
}

void loop() {
  if (!ensureConnected()) {
    delay(50);
    return;
  }

  if (!ensureCharacteristics()) {
    delay(50);
    return;
  }

  ensureKick();

  const uint32_t now = millis();
  if (now - lastPollMs >= POLL_MS) {
    lastPollMs = now;
    if (writeChr) {
      pollVoltage0b0b(writeChr, "(periodic #1)");
      delay(100);
      pollVoltage0b0b(writeChr, "(periodic #2)");
      delay(150);
      bk300AppendFFF1Read();
    }
  }

  drainFrames();

  if (now - lastStatsMs >= STATS_EVERY_MS) {
    lastStatsMs = now;
    Serial.print("stats: conn_ok=");
    Serial.print(c_connectOk);
    Serial.print(" conn_fail=");
    Serial.print(c_connectFail);
    Serial.print(" disc=");
    Serial.print(c_disconnects);
    Serial.print(" scan=");
    Serial.print(c_scanRuns);
    Serial.print("/");
    Serial.print(c_scanFound);
    Serial.print(" notify=");
    Serial.print(c_notifyPackets);
    Serial.print(" pkts ");
    Serial.print(c_notifyBytes);
    Serial.print(" B");
    Serial.print(" frames=");
    Serial.print(c_framesTotal);
    Serial.print(" badcrc=");
    Serial.print(c_framesBadCrc);
    Serial.print(" 4b0b=");
    Serial.print(c_frames4b0b);
    Serial.print(" poll_wr_fail=");
    Serial.print(c_pollWriteFail);
    Serial.print(" rd_at=");
    Serial.print(c_rdAttempts);
    Serial.print(" fff1_rd=");
    Serial.print(c_gattReadFFF1);
    Serial.print(" rxovf=");
    Serial.println(c_rxOverflows);
  }

  delay(5);
}
