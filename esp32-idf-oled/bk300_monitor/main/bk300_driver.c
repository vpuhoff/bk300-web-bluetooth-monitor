/**
 * BK300 voltage monitor — драйвер (ESP-IDF, Bluedroid GATT client).
 *
 * GATT-Client напрямую через esp_ble_gattc_* / esp_ble_gap_* — как в Android-стеке
 * (Bluedroid). Сценарий повторяет btsnoop_hci.log.last:
 *   1) скан BLE по сервису FFF0 / имени BK300
 *   2) open + MTU 247 + service search
 *   3) get char FFF1 (notify) + FFF2 (write-no-response)
 *   4) register-for-notify (Bluedroid пишет CCCD 0x0001 как Write Request)
 *   5) init-цепочка: 0B06 00 → 0100 → 0B01 <millis()/1000 LE> → 0B08 → 0B0B
 *   6) каждую секунду 0B0B; на notify FFF1 разбираем 4B0B и печатаем напряжение U16LE/100.
 *
 * Учти: часто один активный централь на слейве — ПК отбирает линк, а ESP ещё «connected»;
 * тогда FFF2 в никуда. Ловим по read_rssi / ошибке write и делаем gattc_close.
 *
 * Если этот вариант ТОЖЕ даст «notify=0» — значит дело реально не во wrapper-е, а в чем-то
 * специфичном для радио ESP32 vs прошивка BK300. Тогда нужен сниффер на nRF52840.
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatt_defs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bk300_driver.h"
#include "bk300_protocol.h"
#include "oled.h"

static const char *TAG = "bk300";

// ---- BK300 GATT UUIDs ----
#define UUID16_FFF0 0xFFF0
#define UUID16_FFF1 0xFFF1
#define UUID16_FFF2 0xFFF2
#define UUID16_CCCD 0x2902

#define SCAN_DURATION_S 15
#define POLL_PERIOD_US (1000 * 1000)  // 1 s
/** Если после kick так и не пришёл 0B4B — раз в столько µs дублируем 0100 + 0B0B. */
#define BK300_KICK_STALL_NUDGE_US (5 * 1000 * 1000ULL)
/** Подряд неудач read_rssi (ошибка или r==127) после kick — сброс «зомби» GATT. */
#define BK300_RSSI_DEAD_LINK_STREAK 4
/** После хотя бы одного 0B4B: нет новых кадров напряжения — эскалация (нужно, иначе rx «растёт» бесконечно при зомби-линке). */
#define BK300_VOLTAGE_SILENCE_NUDGE_US (15 * 1000000ULL)
#define BK300_VOLTAGE_SILENCE_REREG_US (25 * 1000000ULL)
#define BK300_VOLTAGE_SILENCE_CCCD_US (35 * 1000000ULL)
#define BK300_VOLTAGE_SILENCE_REKICK_US (50 * 1000000ULL)
#define BK300_VOLTAGE_SILENCE_DISCONNECT_US (75 * 1000000ULL)
/** Нет нового 0B4B столько времени — пробуем GATT Read (проверка живого ACL/ATT). */
#define BK300_RX_STALE_LINK_PROBE_US (30 * 1000000ULL)
/** Если ответ на probe-read не пришёл — считаем линк мёртвым. */
#define BK300_LINK_READ_TIMEOUT_US (5 * 1000000ULL)
/** После удачного read не ддосим слейв: пауза перед следующим probe, пока rx всё ещё stale. */
#define BK300_LINK_PROBE_COOLDOWN_OK_US (8 * 1000000ULL)
/** Read прошёл, но телеметрии всё равно нет столько — рвём линк и коннект с нуля. */
#define BK300_LINK_PROBE_STALE_HARD_DISCONNECT_US (45 * 1000000ULL)

#define BK300_LINK_PROBE_KIND_NONE 0
#define BK300_LINK_PROBE_KIND_CHAR 1
#define BK300_LINK_PROBE_KIND_DESCR 2

static esp_bt_uuid_t s_svc_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = {.uuid16 = UUID16_FFF0},
};
static esp_bt_uuid_t s_notify_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = {.uuid16 = UUID16_FFF1},
};
static esp_bt_uuid_t s_write_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = {.uuid16 = UUID16_FFF2},
};
static esp_bt_uuid_t s_cccd_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = {.uuid16 = UUID16_CCCD},
};

// Параметры скана: активный, 100 ms интервал, 99 ms окно (как у нас в Arduino).
static esp_ble_scan_params_t s_scan_params = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50,  // 80 * 0.625 = 50 ms
    .scan_window = 0x4F,
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
};

// Connection parameters: давим ESP32 на быстрый интервал (15-30 ms), чтобы поведение
// было похоже на Android-клиент. Дефолтный 70-90 ms может быть слишком медленным
// для прошивки BK300, и notify, хотя и приходит на ATT-уровне, может теряться или
// откладываться.
static esp_ble_conn_update_params_t s_conn_params = {
    .min_int = 0x0C,  // 12 * 1.25 = 15 ms
    .max_int = 0x18,  // 24 * 1.25 = 30 ms
    .latency = 0,
    .timeout = 400,   // 4 s
    // bda заполним перед вызовом.
};

// Состояние одной активной сессии. Простой single-connection клиент.
typedef struct {
  bool registered;
  esp_gatt_if_t gattc_if;
  uint16_t conn_id;
  bool connected;

  esp_bd_addr_t peer_bda;
  esp_ble_addr_type_t peer_bda_type;

  uint16_t service_start_handle;
  uint16_t service_end_handle;

  uint16_t notify_char_handle;
  uint16_t notify_cccd_handle;
  uint16_t write_char_handle;

  bool notify_registered;
  bool kicked;

  /** Время завершения полной kick-цепочки (esp_timer_get_time), 0 = ещё не завершили. */
  uint64_t kick_done_us;
  /** После kick получили хотя бы один кадр 0B4B с напряжением. */
  bool saw_voltage_notify;
  /** Время последнего валидного кадра 0B4B (esp_timer_get_time), 0 = ещё не было / только kick. */
  uint64_t last_0b4b_us;
  /** Номер последней выполненной фазы recovery при тишине 0B4B (1..5), 0 = сброс. */
  uint8_t voltage_recovery_phase;
  /** Счётчик 5-секундных «stall»-тиков (лёгкий nudge); растёт, пока нет 0B4B до первого кадра. */
  uint32_t stall_ticks;
  /** Подряд плохих read_rssi при kicked (недоступен слой линка). */
  uint8_t rssi_dead_link_streak;

  /** Зомби-линк: возраст rx > порога — уходит GATT read, ждём колбэк или таймаут. */
  uint64_t link_probe_sent_us;
  uint64_t link_probe_cooldown_until_us;
  uint8_t link_probe_kind;

  esp_timer_handle_t poll_timer;

  // Stats
  uint32_t notify_pkts;
  uint32_t notify_bytes;
  uint32_t frames_total;
  uint32_t frames_4b0b;
  uint32_t frames_badcrc;
} bk300_ctx_t;

static bk300_ctx_t g_ctx = {0};

static void bk300_send_kick_sequence(void);
static void bk300_kick_run_body(const char *phase_tag);
static void bk300_send_0b0b(const char *tag);
static void bk300_poll_stall_nudge(void);
static void bk300_recovery_rewrite_cccd(void);
static void bk300_recovery_rereg_notify(void);
static void bk300_kick_repeat_task(void *arg);
static void bk300_poll_timer_cb(void *arg);
static void bk300_poll_voltage_silence_recovery(uint64_t now);
static void bk300_poll_link_read_probe(uint64_t now);
static void bk300_link_probe_start(uint64_t now);
static void bk300_post_disconnect_rescan_task(void *arg);

static volatile bool s_rekick_busy;

/** Закрыть GATT: стек иногда не шлёт DISCONNECT, пока слейв уже недоступен. */
static void bk300_force_disconnect(const char *reason) {
  if (!g_ctx.connected || g_ctx.gattc_if == ESP_GATT_IF_NONE) return;
  ESP_LOGW(TAG, "force disconnect: %s", reason ? reason : "?");
  oled_set_status("Stale link");
  g_ctx.rssi_dead_link_streak = 0;
  g_ctx.link_probe_sent_us = 0;
  g_ctx.link_probe_kind = BK300_LINK_PROBE_KIND_NONE;
  g_ctx.link_probe_cooldown_until_us = 0;
  (void)esp_ble_gattc_close(g_ctx.gattc_if, g_ctx.conn_id);
}

// ---- Helpers ----

static bool bk300_advertises_fff0(uint8_t *adv_data, uint8_t adv_len) {
  uint8_t len = 0;
  uint8_t *p = esp_ble_resolve_adv_data(adv_data, ESP_BLE_AD_TYPE_16SRV_CMPL, &len);
  if (!p) p = esp_ble_resolve_adv_data(adv_data, ESP_BLE_AD_TYPE_16SRV_PART, &len);
  if (!p || len < 2) return false;
  for (int i = 0; i + 1 < len; i += 2) {
    uint16_t u16 = (uint16_t)(p[i] | (p[i + 1] << 8));
    if (u16 == UUID16_FFF0) return true;
  }
  return false;
}

static bool bk300_name_is_bk300(uint8_t *adv_data, uint8_t adv_len) {
  uint8_t len = 0;
  uint8_t *name = esp_ble_resolve_adv_data(adv_data, ESP_BLE_AD_TYPE_NAME_CMPL, &len);
  if (!name) name = esp_ble_resolve_adv_data(adv_data, ESP_BLE_AD_TYPE_NAME_SHORT, &len);
  if (!name || len == 0) return false;
  // case-insensitive prefix match
  if (len < 5) return false;
  return (name[0] == 'B' || name[0] == 'b') && (name[1] == 'K' || name[1] == 'k') &&
         (name[2] == '3') && (name[3] == '0') && (name[4] == '0');
}

static void hexdump_line(const char *label, const uint8_t *data, size_t len) {
  static char line[3 * 64 + 1];
  size_t lim = len < 32 ? len : 32;
  size_t off = 0;
  for (size_t i = 0; i < lim; i++) {
    off += snprintf(line + off, sizeof(line) - off, "%02X%s", data[i], (i + 1 < lim) ? " " : "");
  }
  ESP_LOGI(TAG, "%s%s%s", label, line, len > lim ? "..." : "");
}

// ---- GATTC writes ----

static esp_err_t bk300_write_fff2_no_rsp(const uint8_t *data, size_t len, const char *tag) {
  if (!g_ctx.connected || g_ctx.write_char_handle == 0) return ESP_ERR_INVALID_STATE;
  esp_err_t err = esp_ble_gattc_write_char(
      g_ctx.gattc_if, g_ctx.conn_id, g_ctx.write_char_handle, (uint16_t)len,
      (uint8_t *)data, ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "write_char(FFF2, %s) err=%d", tag ? tag : "?", err);
    if (g_ctx.kicked) {
      bk300_force_disconnect("FFF2 write rejected by stack");
    }
  } else {
    hexdump_line("TX FFF2 ", data, len);
  }
  return err;
}

static void bk300_send_cmd_payload(uint8_t b0, uint8_t b1,
                                   const uint8_t *payload, size_t plen,
                                   const char *tag) {
  uint8_t frame[64];
  uint16_t cmd_le = (uint16_t)(b0 | (b1 << 8));
  size_t n = bk300_build_frame(frame, sizeof(frame), cmd_le, payload, plen);
  if (n == 0) return;
  bk300_write_fff2_no_rsp(frame, n, tag);
}

static void bk300_send_cmd_bytes(uint8_t b0, uint8_t b1, const char *tag) {
  bk300_send_cmd_payload(b0, b1, NULL, 0, tag);
}

static void bk300_send_0b0b(const char *tag) {
  bk300_send_cmd_bytes(0x0B, 0x0B, tag ? tag : "0B0B");
}

// ---- Init kick: 0B06 00 → 0100 → 0B01 <time LE> → 0B08 → 0B0B ----

#ifndef BK300_FALLBACK_EPOCH
#define BK300_FALLBACK_EPOCH 1772582400UL
#endif

/** Полная цепочка команд на FFF2 (с задержками). Можно вызывать повторно при recovery. */
static void bk300_kick_run_body(const char *phase_tag) {
  /* last_0b4b_us — только момент последнего *реального* 0B4B; при re-kick не сбрасываем,
   * иначе «тишина» обнуляется и disconnect >=75s от первой потери данных никогда не срабатывает. */
  if (!g_ctx.saw_voltage_notify) {
    g_ctx.last_0b4b_us = 0;
  }
  g_ctx.voltage_recovery_phase = 0;
  ESP_LOGI(TAG, "kick (%s): 0B06 00 -> 0100 -> 0B01 -> 0B08 -> 0B0B", phase_tag ? phase_tag : "?");
  oled_set_status("Kicking...");
  static const uint8_t open_voltage_off[] = {0x00};
  bk300_send_cmd_payload(0x0B, 0x06, open_voltage_off, sizeof(open_voltage_off), "0B06 00");
  vTaskDelay(pdMS_TO_TICKS(15));
  bk300_send_cmd_bytes(0x01, 0x00, "0100");
  vTaskDelay(pdMS_TO_TICKS(700));

  time_t real_now = 0;
  time(&real_now);
  uint32_t sec = (real_now > 1700000000) ? (uint32_t)real_now : (uint32_t)BK300_FALLBACK_EPOCH;
  ESP_LOGI(TAG, "0B01 timestamp = %" PRIu32 " (real_time_ok=%d)",
           sec, (int)(real_now > 1700000000));
  const uint8_t since_payload[] = {
      (uint8_t)(sec & 0xFF),
      (uint8_t)((sec >> 8) & 0xFF),
      (uint8_t)((sec >> 16) & 0xFF),
      (uint8_t)((sec >> 24) & 0xFF),
  };
  bk300_send_cmd_payload(0x0B, 0x01, since_payload, sizeof(since_payload), "0B01 time");
  vTaskDelay(pdMS_TO_TICKS(100));
  bk300_send_cmd_bytes(0x0B, 0x08, "0B08");
  vTaskDelay(pdMS_TO_TICKS(50));

  bk300_send_0b0b("0B0B initial #1");
  vTaskDelay(pdMS_TO_TICKS(100));
  bk300_send_0b0b("0B0B initial #2");

  g_ctx.kicked = true;
  g_ctx.kick_done_us = esp_timer_get_time();
  g_ctx.stall_ticks = 0;
  oled_set_status("Wait data");

  // MTU 247 запрашиваем после kick — теперь BK300 активен и не разорвёт линк.
  esp_ble_gattc_send_mtu_req(g_ctx.gattc_if, g_ctx.conn_id);

  esp_timer_create_args_t targs = {
      .callback = bk300_poll_timer_cb,
      .name = "bk300_poll",
  };
  if (g_ctx.poll_timer == NULL) {
    ESP_ERROR_CHECK(esp_timer_create(&targs, &g_ctx.poll_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(g_ctx.poll_timer, POLL_PERIOD_US));
  }
}

static void bk300_send_kick_sequence(void) {
  if (g_ctx.kicked) return;
  bk300_kick_run_body("first");
}

static void bk300_recovery_rewrite_cccd(void) {
  if (!g_ctx.connected || g_ctx.notify_cccd_handle == 0 || g_ctx.gattc_if == ESP_GATT_IF_NONE) return;
  uint8_t cccd_val[2] = {0x01, 0x00};
  esp_err_t err = esp_ble_gattc_write_char_descr(
      g_ctx.gattc_if, g_ctx.conn_id, g_ctx.notify_cccd_handle, sizeof(cccd_val), cccd_val,
      ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
  ESP_LOGW(TAG, "recovery: CCCD write 01 00 again, err=%d", err);
}

static void bk300_recovery_rereg_notify(void) {
  if (!g_ctx.connected || !g_ctx.notify_char_handle || g_ctx.gattc_if == ESP_GATT_IF_NONE) return;
  esp_err_t e =
      esp_ble_gattc_register_for_notify(g_ctx.gattc_if, g_ctx.peer_bda, g_ctx.notify_char_handle);
  ESP_LOGW(TAG, "recovery: register_for_notify again, err=%d", e);
}

static void bk300_kick_repeat_task(void *arg) {
  (void)arg;
  vTaskDelay(pdMS_TO_TICKS(150));
  if (g_ctx.connected && g_ctx.write_char_handle) {
    bk300_kick_run_body("recover");
  } else {
    ESP_LOGW(TAG, "re-kick skipped (disconnected)");
  }
  s_rekick_busy = false;
  vTaskDelete(NULL);
}

/** Не вызывать vTaskDelay из GATTC_DISCONNECT_EVT — блокирует BTC и скан может не стартовать. */
static void bk300_post_disconnect_rescan_task(void *arg) {
  uint32_t delay_ms = arg ? (uint32_t)(uintptr_t)arg : 1000;
  vTaskDelay(pdMS_TO_TICKS(delay_ms));
  esp_err_t e = esp_ble_gap_start_scanning(SCAN_DURATION_S);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "post-disconnect scan err=%d %s", e, esp_err_to_name(e));
    vTaskDelay(pdMS_TO_TICKS(400));
    e = esp_ble_gap_start_scanning(SCAN_DURATION_S);
    if (e != ESP_OK) {
      ESP_LOGE(TAG, "post-disconnect scan retry err=%d %s", e, esp_err_to_name(e));
    }
  } else {
    ESP_LOGI(TAG, "post-disconnect scan started");
  }
  vTaskDelete(NULL);
}

static void bk300_poll_stall_nudge(void) {
  if (g_ctx.notify_pkts == 0) {
    ESP_LOGW(TAG, "no 0B4B after kick: nudge 0100 + 0B0B (no notify)");
    bk300_send_cmd_bytes(0x01, 0x00, "stall nudge 0100");
  } else {
    ESP_LOGW(TAG, "no 0B4B after kick: nudge 0B0B (notify flowing)");
  }
  bk300_send_0b0b("stall nudge 0B0B");
}

/** После первого кадра напряжения: только RSSI/0B0B могут «жить», а 0B4B нет — отдельная лестница. */
static void bk300_poll_voltage_silence_recovery(uint64_t now) {
  static uint64_t s_last_disc_log_us;

  if (!g_ctx.connected || !g_ctx.kicked || !g_ctx.saw_voltage_notify || g_ctx.last_0b4b_us == 0) {
    return;
  }

  uint64_t silence = now - g_ctx.last_0b4b_us;

  /* Не полагаемся на одну попытку: при зомби-линке первый close может не отработать. */
  if (silence >= BK300_VOLTAGE_SILENCE_DISCONNECT_US) {
    g_ctx.voltage_recovery_phase = 5;
    if (s_last_disc_log_us == 0 || (now - s_last_disc_log_us) >= (15 * 1000000ULL)) {
      s_last_disc_log_us = now;
      ESP_LOGW(TAG, "0B4B stale %" PRIu32 "s: gattc_close (repeat each poll until link drops)",
               (uint32_t)(silence / 1000000ULL));
    }
    (void)esp_ble_gattc_close(g_ctx.gattc_if, g_ctx.conn_id);
    return;
  }

  s_last_disc_log_us = 0;

  uint8_t phase = 0;
  if (silence >= BK300_VOLTAGE_SILENCE_REKICK_US) {
    phase = 4;
  } else if (silence >= BK300_VOLTAGE_SILENCE_CCCD_US) {
    phase = 3;
  } else if (silence >= BK300_VOLTAGE_SILENCE_REREG_US) {
    phase = 2;
  } else if (silence >= BK300_VOLTAGE_SILENCE_NUDGE_US) {
    phase = 1;
  }

  if (phase == 0) {
    g_ctx.voltage_recovery_phase = 0;
    return;
  }
  if (phase <= g_ctx.voltage_recovery_phase) {
    return;
  }

  g_ctx.voltage_recovery_phase = phase;

  switch (phase) {
  case 1:
    ESP_LOGW(TAG, "0B4B stale >=15s: nudge 0B0B only (0100 omitted — live link)");
    bk300_send_0b0b("silence nudge 0B0B");
    break;
  case 2:
    ESP_LOGW(TAG, "0B4B stale >=25s: register_for_notify again");
    bk300_recovery_rereg_notify();
    break;
  case 3:
    ESP_LOGW(TAG, "0B4B stale >=35s: rewrite CCCD");
    bk300_recovery_rewrite_cccd();
    break;
  case 4:
    if (!s_rekick_busy) {
      ESP_LOGW(TAG, "0B4B stale >=50s: full re-kick");
      s_rekick_busy = true;
      if (xTaskCreate(bk300_kick_repeat_task, "bk_vrkick", 4608, NULL, 5, NULL) != pdPASS) {
        s_rekick_busy = false;
      }
    }
    break;
  default:
    break;
  }
}

/** Если долго нет 0B4B, но RSSI ещё «живой» — GATT Read должен уйти в ATT; при обрыве — ошибка/таймаут → disconnect. */
static void bk300_link_probe_start(uint64_t now) {
  if (!g_ctx.connected || g_ctx.gattc_if == ESP_GATT_IF_NONE) return;

  uint16_t handle = 0;
  bool     descr = false;

  if (g_ctx.notify_char_handle) {
    handle = g_ctx.notify_char_handle;
  } else if (g_ctx.write_char_handle) {
    handle = g_ctx.write_char_handle;
  } else if (g_ctx.notify_cccd_handle) {
    handle = g_ctx.notify_cccd_handle;
    descr = true;
  } else {
    bk300_force_disconnect("link probe: no GATT handles");
    return;
  }

  g_ctx.link_probe_sent_us = now;
  g_ctx.link_probe_kind = descr ? (uint8_t)BK300_LINK_PROBE_KIND_DESCR : (uint8_t)BK300_LINK_PROBE_KIND_CHAR;

  esp_err_t e =
      descr ? esp_ble_gattc_read_char_descr(g_ctx.gattc_if, g_ctx.conn_id, handle, ESP_GATT_AUTH_REQ_NONE)
            : esp_ble_gattc_read_char(g_ctx.gattc_if, g_ctx.conn_id, handle, ESP_GATT_AUTH_REQ_NONE);
  if (e != ESP_OK) {
    ESP_LOGW(TAG, "link probe read init err=%d", (int)e);
    g_ctx.link_probe_sent_us = 0;
    g_ctx.link_probe_kind = BK300_LINK_PROBE_KIND_NONE;
    bk300_force_disconnect("link probe stack rejected read");
    return;
  }
  ESP_LOGI(TAG, "link probe: %s read handle=0x%04X (rx >30s)", descr ? "CCCD" : "char", handle);
}

static void bk300_poll_link_read_probe(uint64_t now) {
  if (!g_ctx.connected || !g_ctx.kicked || !g_ctx.saw_voltage_notify || g_ctx.last_0b4b_us == 0) {
    return;
  }

  uint64_t rx_age = now - g_ctx.last_0b4b_us;
  if (rx_age < BK300_RX_STALE_LINK_PROBE_US) {
    g_ctx.link_probe_sent_us = 0;
    g_ctx.link_probe_kind = BK300_LINK_PROBE_KIND_NONE;
    return;
  }

  if (g_ctx.link_probe_sent_us != 0) {
    if ((now - g_ctx.link_probe_sent_us) >= BK300_LINK_READ_TIMEOUT_US) {
      g_ctx.link_probe_sent_us = 0;
      g_ctx.link_probe_kind = BK300_LINK_PROBE_KIND_NONE;
      bk300_force_disconnect("link probe read timeout (no GATT callback)");
    }
    return;
  }

  if (now < g_ctx.link_probe_cooldown_until_us) {
    return;
  }

  bk300_link_probe_start(now);
}

static void bk300_poll_timer_cb(void *arg) {
  (void)arg;
  if (!g_ctx.connected) return;

  uint64_t now = esp_timer_get_time();
  bool     skip_light_nudge = false;

  if (g_ctx.kicked && !g_ctx.saw_voltage_notify && g_ctx.kick_done_us != 0) {
    if ((now - g_ctx.kick_done_us) >= BK300_KICK_STALL_NUDGE_US) {
      g_ctx.kick_done_us = now;
      g_ctx.stall_ticks++;

      /* notify_pkts==0: на FFF1 ничего не приходит — только FFF2 «пинать» мало. */
      if (g_ctx.notify_pkts == 0) {
        if (g_ctx.stall_ticks == 2) {
          bk300_recovery_rereg_notify();
          skip_light_nudge = true;
        } else if (g_ctx.stall_ticks == 4) {
          bk300_recovery_rewrite_cccd();
          skip_light_nudge = true;
        } else if (g_ctx.stall_ticks == 6 && !s_rekick_busy) {
          ESP_LOGW(TAG, "recovery: schedule full re-kick");
          s_rekick_busy = true;
          if (xTaskCreate(bk300_kick_repeat_task, "bk_rkick", 4608, NULL, 5, NULL) != pdPASS) {
            s_rekick_busy = false;
          }
          skip_light_nudge = true;
        } else if (g_ctx.stall_ticks >= 12) {
          ESP_LOGW(TAG, "no NOTIFY ~60s — disconnect & rescan");
          skip_light_nudge = true;
          esp_ble_gattc_close(g_ctx.gattc_if, g_ctx.conn_id);
        }
      }

      if (!skip_light_nudge) {
        bk300_poll_stall_nudge();
      }
    }
  }

  bk300_poll_voltage_silence_recovery(now);

  bk300_poll_link_read_probe(now);

  bk300_send_0b0b("0B0B periodic");
  (void)esp_ble_gap_read_rssi(g_ctx.peer_bda);
}

// ---- Frames draining ----

static void bk300_drain_frames(void) {
  bk300_frame_t f = {0};
  while (bk300_rx_pop_frame(&f)) {
    g_ctx.frames_total++;
    if (!f.crc_ok) {
      g_ctx.frames_badcrc++;
      ESP_LOGW(TAG, "bad CRC, type=%04X len=%u", f.type_le, f.length);
      continue;
    }
    if (f.type_le == 0x0B4B && f.payload_len >= 2) {
      g_ctx.frames_4b0b++;
      g_ctx.saw_voltage_notify = true;
      g_ctx.stall_ticks = 0;
      g_ctx.rssi_dead_link_streak = 0;
      g_ctx.last_0b4b_us = esp_timer_get_time();
      g_ctx.voltage_recovery_phase = 0;
      g_ctx.link_probe_sent_us = 0;
      g_ctx.link_probe_cooldown_until_us = 0;
      g_ctx.link_probe_kind = BK300_LINK_PROBE_KIND_NONE;
      uint16_t raw = (uint16_t)(f.payload[0] | (f.payload[1] << 8));
      float v = raw / 100.0f;
      ESP_LOGI(TAG, "Voltage: %.2f V", v);
      oled_set_voltage(v);
      oled_set_status("Live");
    } else {
      ESP_LOGI(TAG, "frame type=%04X plen=%u", f.type_le, f.payload_len);
    }
  }
}

// ---- GATTC event handler ----

static void gattc_profile_event_handler(esp_gattc_cb_event_t event,
                                        esp_gatt_if_t gattc_if,
                                        esp_ble_gattc_cb_param_t *p) {
  switch (event) {
  case ESP_GATTC_REG_EVT:
    ESP_LOGI(TAG, "GATTC registered, app_id=%d status=%d", p->reg.app_id, p->reg.status);
    g_ctx.registered = (p->reg.status == ESP_GATT_OK);
    g_ctx.gattc_if = gattc_if;
    if (g_ctx.registered) {
      oled_set_rssi_dbm(OLED_RSSI_NONE);
      oled_set_status("Scanning...");
      ESP_ERROR_CHECK(esp_ble_gap_set_scan_params(&s_scan_params));
    }
    break;

  case ESP_GATTC_CONNECT_EVT: {
    g_ctx.rssi_dead_link_streak = 0;
    ESP_LOGI(TAG, "GATTC connect, conn_id=%d, peer=%02X:%02X:%02X:%02X:%02X:%02X",
             p->connect.conn_id,
             p->connect.remote_bda[0], p->connect.remote_bda[1], p->connect.remote_bda[2],
             p->connect.remote_bda[3], p->connect.remote_bda[4], p->connect.remote_bda[5]);
    g_ctx.conn_id = p->connect.conn_id;
    memcpy(g_ctx.peer_bda, p->connect.remote_bda, sizeof(esp_bd_addr_t));
    g_ctx.connected = true;
    oled_set_status("Connected");

    // Сразу после connect просим параметры соединения, чтобы интервал был похож
    // на Android (15-30 ms, latency 0). Без этого Bluedroid стартует с ~95 ms.
    memcpy(s_conn_params.bda, p->connect.remote_bda, sizeof(esp_bd_addr_t));
    esp_err_t pe = esp_ble_gap_update_conn_params(&s_conn_params);
    ESP_LOGI(TAG, "request conn params 15-30ms latency=0 to=4s, err=%d", pe);

    // Зафиксируем 1M PHY: 2M на BK300 точно не поддерживается, и стек может
    // ошибочно перейти. Если функции PHY нет в этом IDF — пропустим.
#if defined(CONFIG_BT_BLE_50_FEATURES_SUPPORTED) && CONFIG_BT_BLE_50_FEATURES_SUPPORTED
    esp_ble_gap_set_prefered_phy(p->connect.remote_bda,
                                 0,  // tx_phy_mask: no preference
                                 ESP_BLE_GAP_PHY_1M_PREF_MASK,
                                 ESP_BLE_GAP_PHY_1M_PREF_MASK,
                                 ESP_BLE_GAP_PHY_OPTIONS_NO_PREFER);
#endif
    break;
  }

  case ESP_GATTC_OPEN_EVT:
    if (p->open.status != ESP_GATT_OK) {
      ESP_LOGE(TAG, "GATTC open fail, status=%d", p->open.status);
      oled_set_rssi_dbm(OLED_RSSI_NONE);
      oled_set_status("Conn failed");
      g_ctx.connected = false;
      // 0x3E = CONN_FAILED_ESTABLISHMENT: BK300 не готов после разрыва.
      // Ждём дольше перед повторным сканом — не блокируем BTC task.
      uint32_t backoff_ms = (p->open.status == 133) ? 5000 : 1500;
      if (xTaskCreate(bk300_post_disconnect_rescan_task, "bk_rescan",
                      3584, (void *)(uintptr_t)backoff_ms, 5, NULL) != pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        ESP_ERROR_CHECK(esp_ble_gap_start_scanning(SCAN_DURATION_S));
      }
      break;
    }
    ESP_LOGI(TAG, "GATTC open ok, conn_id=%d", p->open.conn_id);
    g_ctx.conn_id = p->open.conn_id;
    g_ctx.connected = true;
    // Если handles уже известны (reconnect к тому же устройству) — пропускаем
    // service discovery (3+ с) и сразу регистрируем notify. Это критично:
    // BK300 ставит supervision_timeout=100 (1 s) рано, и discovery не успевает.
    if (g_ctx.notify_char_handle && g_ctx.write_char_handle &&
        memcmp(g_ctx.peer_bda, p->open.remote_bda, sizeof(esp_bd_addr_t)) == 0) {
      ESP_LOGI(TAG, "reconnect: reusing cached handles FFF1=0x%04X FFF2=0x%04X CCCD=0x%04X",
               g_ctx.notify_char_handle, g_ctx.write_char_handle, g_ctx.notify_cccd_handle);
      esp_err_t err = esp_ble_gattc_register_for_notify(gattc_if, g_ctx.peer_bda,
                                                        g_ctx.notify_char_handle);
      ESP_LOGI(TAG, "register_for_notify (cached), err=%d", err);
    } else {
      esp_ble_gattc_search_service(gattc_if, p->open.conn_id, &s_svc_uuid);
    }
    break;

  case ESP_GATTC_CFG_MTU_EVT:
    ESP_LOGI(TAG, "MTU exchanged, status=%d, mtu=%d", p->cfg_mtu.status, p->cfg_mtu.mtu);
    break;

  case ESP_GATTC_SEARCH_RES_EVT:
    if (p->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
        p->search_res.srvc_id.uuid.uuid.uuid16 == UUID16_FFF0) {
      g_ctx.service_start_handle = p->search_res.start_handle;
      g_ctx.service_end_handle = p->search_res.end_handle;
      ESP_LOGI(TAG, "FFF0 service found, handles 0x%04X..0x%04X",
               p->search_res.start_handle, p->search_res.end_handle);
    }
    break;

  case ESP_GATTC_SEARCH_CMPL_EVT: {
    if (g_ctx.service_start_handle == 0) {
      ESP_LOGE(TAG, "FFF0 service not found, disconnecting");
      esp_ble_gattc_close(gattc_if, p->search_cmpl.conn_id);
      break;
    }
    // Берём FFF1 и FFF2 по UUID в диапазоне сервиса.
    uint16_t count = 1;
    esp_gattc_char_elem_t elem = {0};
    if (esp_ble_gattc_get_char_by_uuid(gattc_if, p->search_cmpl.conn_id,
                                        g_ctx.service_start_handle,
                                        g_ctx.service_end_handle,
                                        s_notify_uuid, &elem, &count) == ESP_OK && count > 0) {
      g_ctx.notify_char_handle = elem.char_handle;
      ESP_LOGI(TAG, "FFF1 char_handle=0x%04X props=0x%02X", elem.char_handle, elem.properties);
    } else {
      ESP_LOGE(TAG, "FFF1 not found");
    }
    count = 1;
    if (esp_ble_gattc_get_char_by_uuid(gattc_if, p->search_cmpl.conn_id,
                                        g_ctx.service_start_handle,
                                        g_ctx.service_end_handle,
                                        s_write_uuid, &elem, &count) == ESP_OK && count > 0) {
      g_ctx.write_char_handle = elem.char_handle;
      ESP_LOGI(TAG, "FFF2 char_handle=0x%04X props=0x%02X", elem.char_handle, elem.properties);
    } else {
      ESP_LOGE(TAG, "FFF2 not found");
    }

    // Ищем CCCD 0x2902 у FFF1.
    uint16_t descr_count = 1;
    esp_gattc_descr_elem_t descr = {0};
    if (g_ctx.notify_char_handle &&
        esp_ble_gattc_get_descr_by_char_handle(gattc_if, p->search_cmpl.conn_id,
                                               g_ctx.notify_char_handle, s_cccd_uuid,
                                               &descr, &descr_count) == ESP_OK &&
        descr_count > 0) {
      g_ctx.notify_cccd_handle = descr.handle;
      ESP_LOGI(TAG, "FFF1 CCCD handle=0x%04X", descr.handle);
    } else {
      ESP_LOGW(TAG, "FFF1 CCCD not found via descriptor lookup");
    }

    if (g_ctx.notify_char_handle && g_ctx.write_char_handle) {
      // Bluedroid сам пошлёт Write Request 01 00 на CCCD после удачной регистрации.
      esp_err_t err = esp_ble_gattc_register_for_notify(gattc_if, g_ctx.peer_bda,
                                                       g_ctx.notify_char_handle);
      ESP_LOGI(TAG, "register_for_notify, err=%d", err);
    } else {
      ESP_LOGE(TAG, "FFF1/FFF2 missing — disconnecting");
      esp_ble_gattc_close(gattc_if, p->search_cmpl.conn_id);
    }
    break;
  }

  case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
    if (p->reg_for_notify.status != ESP_GATT_OK) {
      ESP_LOGE(TAG, "register_for_notify FAIL status=%d", p->reg_for_notify.status);
      break;
    }
    ESP_LOGI(TAG, "register_for_notify ok, handle=0x%04X", p->reg_for_notify.handle);
    g_ctx.notify_registered = true;
    // Bluedroid может не послать WRITE_DESCR_EVT если CCCD закеширован.
    // Всегда пишем CCCD явно сами — гарантируем что BK300 его получит.
    if (g_ctx.notify_cccd_handle) {
      uint8_t cccd_val[2] = {0x01, 0x00};
      esp_err_t err = esp_ble_gattc_write_char_descr(
          gattc_if, g_ctx.conn_id, g_ctx.notify_cccd_handle, sizeof(cccd_val), cccd_val,
          ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
      ESP_LOGI(TAG, "explicit CCCD write_descr, err=%d", err);
      if (err != ESP_OK && !g_ctx.kicked) {
        // write_descr не запустился — шлём kick напрямую
        vTaskDelay(pdMS_TO_TICKS(250));
        bk300_send_kick_sequence();
      }
    } else if (!g_ctx.kicked) {
      vTaskDelay(pdMS_TO_TICKS(250));
      bk300_send_kick_sequence();
    }
    break;
  }

  case ESP_GATTC_WRITE_DESCR_EVT:
    ESP_LOGI(TAG, "write_descr complete, status=%d handle=0x%04X",
             p->write.status, p->write.handle);
    if (p->write.status == ESP_GATT_OK && !g_ctx.kicked) {
      vTaskDelay(pdMS_TO_TICKS(250));
      bk300_send_kick_sequence();
    }
    break;

  case ESP_GATTC_NOTIFY_EVT: {
    g_ctx.notify_pkts++;
    g_ctx.notify_bytes += p->notify.value_len;
    g_ctx.stall_ticks = 0;
    g_ctx.rssi_dead_link_streak = 0;
    ESP_LOGI(TAG, "NOTIFY conn=%d handle=0x%04X is_notify=%d len=%u",
             p->notify.conn_id, p->notify.handle, p->notify.is_notify,
             p->notify.value_len);
    hexdump_line("RX FFF1 ", p->notify.value, p->notify.value_len);
    bk300_rx_append(p->notify.value, p->notify.value_len);
    bk300_drain_frames();
    break;
  }

  case ESP_GATTC_WRITE_CHAR_EVT:
    if (p->write.status != ESP_GATT_OK) {
      ESP_LOGW(TAG, "write_char status=%d handle=0x%04X", p->write.status, p->write.handle);
    }
    break;

  case ESP_GATTC_READ_CHAR_EVT:
    if (g_ctx.link_probe_sent_us != 0 && g_ctx.link_probe_kind == BK300_LINK_PROBE_KIND_CHAR) {
      g_ctx.link_probe_sent_us = 0;
      g_ctx.link_probe_kind = BK300_LINK_PROBE_KIND_NONE;
      if (p->read.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "link probe READ_CHAR status=%d", (int)p->read.status);
        bk300_force_disconnect("link probe READ_CHAR failed");
      } else {
        uint64_t t = esp_timer_get_time();
        if ((t - g_ctx.last_0b4b_us) >= BK300_LINK_PROBE_STALE_HARD_DISCONNECT_US) {
          bk300_force_disconnect("READ_CHAR ok but 0B4B stale — full resync");
        } else {
          g_ctx.link_probe_cooldown_until_us = t + BK300_LINK_PROBE_COOLDOWN_OK_US;
          ESP_LOGI(TAG, "link probe READ_CHAR ok len=%d", p->read.value_len);
        }
      }
      break;
    }
    ESP_LOGI(TAG, "read_char status=%d handle=0x%04X len=%d",
             p->read.status, p->read.handle, p->read.value_len);
    break;

  case ESP_GATTC_READ_DESCR_EVT:
    if (g_ctx.link_probe_sent_us != 0 && g_ctx.link_probe_kind == BK300_LINK_PROBE_KIND_DESCR) {
      g_ctx.link_probe_sent_us = 0;
      g_ctx.link_probe_kind = BK300_LINK_PROBE_KIND_NONE;
      if (p->read.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "link probe READ_DESCR status=%d", (int)p->read.status);
        bk300_force_disconnect("link probe CCCD read failed");
      } else {
        uint64_t t = esp_timer_get_time();
        if ((t - g_ctx.last_0b4b_us) >= BK300_LINK_PROBE_STALE_HARD_DISCONNECT_US) {
          bk300_force_disconnect("READ_DESCR ok but 0B4B stale — full resync");
        } else {
          g_ctx.link_probe_cooldown_until_us = t + BK300_LINK_PROBE_COOLDOWN_OK_US;
          ESP_LOGI(TAG, "link probe READ_DESCR ok len=%d", p->read.value_len);
        }
      }
      break;
    }
    ESP_LOGI(TAG, "read_descr status=%d handle=0x%04X len=%d",
             p->read.status, p->read.handle, p->read.value_len);
    if (p->read.value_len >= 2) {
      ESP_LOGI(TAG, "  CCCD bytes: %02X %02X", p->read.value[0], p->read.value[1]);
    }
    break;

  case ESP_GATTC_SRVC_CHG_EVT:
    ESP_LOGW(TAG, "SRVC_CHG: peer changed services — invalidating discovery");
    break;

  case ESP_GATTC_DIS_SRVC_CMPL_EVT:
    ESP_LOGI(TAG, "DIS_SRVC_CMPL");
    break;

  case ESP_GATTC_DISCONNECT_EVT: {
    uint16_t dc = p->disconnect.conn_id;
    ESP_LOGW(TAG, "Disconnected, reason=0x%02X conn_id=%u", (unsigned)p->disconnect.reason, (unsigned)dc);
    if (!g_ctx.connected || dc != g_ctx.conn_id) {
      ESP_LOGW(TAG, "DISCONNECT_EVT ignored (connected=%d evt_cid=%u ctx_cid=%u)", (int)g_ctx.connected,
               (unsigned)dc, (unsigned)g_ctx.conn_id);
      break;
    }

    oled_set_rssi_dbm(OLED_RSSI_NONE);
    oled_set_voltage(-1.0f);
    oled_set_status("Disconnected");

    g_ctx.connected = false;
    g_ctx.kicked = false;
    g_ctx.kick_done_us = 0;
    g_ctx.saw_voltage_notify = false;
    g_ctx.stall_ticks = 0;
    g_ctx.last_0b4b_us = 0;
    g_ctx.voltage_recovery_phase = 0;
    g_ctx.link_probe_sent_us = 0;
    g_ctx.link_probe_cooldown_until_us = 0;
    g_ctx.link_probe_kind = BK300_LINK_PROBE_KIND_NONE;
    s_rekick_busy = false;
    g_ctx.rssi_dead_link_streak = 0;
    g_ctx.notify_registered = false;
    g_ctx.conn_id = 0;
    g_ctx.notify_pkts = 0;
    g_ctx.notify_bytes = 0;
    g_ctx.frames_total = 0;
    g_ctx.frames_4b0b = 0;
    g_ctx.frames_badcrc = 0;
    // peer_bda и handles сохраняем — при переподключении к тому же устройству
    // можно пропустить service discovery и сэкономить 3+ секунды.
    bk300_rx_reset();
    if (g_ctx.poll_timer) {
      esp_timer_stop(g_ctx.poll_timer);
      esp_timer_delete(g_ctx.poll_timer);
      g_ctx.poll_timer = NULL;
    }
    oled_set_status("Reconnecting");
    if (xTaskCreate(bk300_post_disconnect_rescan_task, "bk_rescan", 3584, NULL, 5, NULL) != pdPASS) {
      ESP_LOGE(TAG, "bk_rescan task create failed — immediate scan");
      (void)esp_ble_gap_start_scanning(SCAN_DURATION_S);
    }
    break;
  }

  default:
    ESP_LOGI(TAG, "GATTC unhandled evt=%d", (int)event);
    break;
  }
}

void bk300_driver_gattc_event_handler(esp_gattc_cb_event_t event,
                                      esp_gatt_if_t gattc_if,
                                      esp_ble_gattc_cb_param_t *param) {
  // Один app_id на всю сессию — не надо мультиплексировать.
  gattc_profile_event_handler(event, gattc_if, param);
}

// ---- GAP event handler ----

void bk300_driver_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *p) {
  switch (event) {
  case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
    ESP_LOGI(TAG, "scan params set, starting scan for %d s", SCAN_DURATION_S);
    ESP_ERROR_CHECK(esp_ble_gap_start_scanning(SCAN_DURATION_S));
    break;

  case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
    if (p->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
      ESP_LOGE(TAG, "scan start failed, status=%d", p->scan_start_cmpl.status);
    } else {
      ESP_LOGI(TAG, "scan started");
    }
    break;

  case ESP_GAP_BLE_SCAN_RESULT_EVT:
    if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
      uint8_t *adv = p->scan_rst.ble_adv;
      uint8_t adv_len = p->scan_rst.adv_data_len + p->scan_rst.scan_rsp_len;
      bool match = bk300_name_is_bk300(adv, adv_len);
      if (match) {
        uint8_t nlen = 0;
        uint8_t *nm = esp_ble_resolve_adv_data(adv, ESP_BLE_AD_TYPE_NAME_CMPL, &nlen);
        if (!nm) nm = esp_ble_resolve_adv_data(adv, ESP_BLE_AD_TYPE_NAME_SHORT, &nlen);
        char devname[32] = {0};
        if (nm && nlen > 0) memcpy(devname, nm, nlen < 31 ? nlen : 31);
        ESP_LOGI(TAG, "Found BK300 at %02X:%02X:%02X:%02X:%02X:%02X (rssi=%d) name='%s'",
                 p->scan_rst.bda[0], p->scan_rst.bda[1], p->scan_rst.bda[2],
                 p->scan_rst.bda[3], p->scan_rst.bda[4], p->scan_rst.bda[5],
                 p->scan_rst.rssi, devname);
        oled_set_rssi_dbm((int16_t)p->scan_rst.rssi);
        oled_set_status("Connecting");
        g_ctx.peer_bda_type = p->scan_rst.ble_addr_type;
        memcpy(g_ctx.peer_bda, p->scan_rst.bda, sizeof(esp_bd_addr_t));
        ESP_ERROR_CHECK(esp_ble_gap_stop_scanning());
        // open вызовем в SCAN_STOP_COMPLETE.
      }
    } else if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
      ESP_LOGI(TAG, "scan finished, no BK300 — restarting");
      oled_set_rssi_dbm(OLED_RSSI_NONE);
      oled_set_status("Not found");
      ESP_ERROR_CHECK(esp_ble_gap_start_scanning(SCAN_DURATION_S));
    }
    break;

  case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
    if (p->scan_stop_cmpl.status == ESP_BT_STATUS_SUCCESS && g_ctx.peer_bda[0] != 0) {
      ESP_LOGI(TAG, "open BK300...");
      ESP_ERROR_CHECK(esp_ble_gattc_open(g_ctx.gattc_if, g_ctx.peer_bda,
                                          g_ctx.peer_bda_type, true));
    }
    break;

  case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
    ESP_LOGI(TAG, "conn params updated: int=%d lat=%d to=%d status=%d",
             p->update_conn_params.conn_int, p->update_conn_params.latency,
             p->update_conn_params.timeout, p->update_conn_params.status);
    break;

  case ESP_GAP_BLE_PHY_UPDATE_COMPLETE_EVT:
    ESP_LOGI(TAG, "PHY updated: tx=%d rx=%d status=%d",
             p->phy_update.tx_phy, p->phy_update.rx_phy, p->phy_update.status);
    break;

  case ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT: {
    bool monitor_rssi = g_ctx.connected && g_ctx.kicked && g_ctx.kick_done_us != 0 &&
                        (esp_timer_get_time() - g_ctx.kick_done_us) > (3 * 1000000ULL);

    if (p->read_rssi_cmpl.status != ESP_BT_STATUS_SUCCESS) {
      if (monitor_rssi) {
        g_ctx.rssi_dead_link_streak++;
        ESP_LOGW(TAG, "read_rssi status=%d streak=%u (возможна «зомби»-сессия)",
                 (int)p->read_rssi_cmpl.status, (unsigned)g_ctx.rssi_dead_link_streak);
        if (g_ctx.rssi_dead_link_streak >= BK300_RSSI_DEAD_LINK_STREAK) {
          bk300_force_disconnect("read_rssi failed");
        }
      }
      break;
    }

    int8_t r = p->read_rssi_cmpl.rssi;
    if (r != 127) {
      oled_set_rssi_dbm((int16_t)r);
      g_ctx.rssi_dead_link_streak = 0;
    } else if (monitor_rssi) {
      g_ctx.rssi_dead_link_streak++;
      ESP_LOGW(TAG, "read_rssi=127 streak=%u", (unsigned)g_ctx.rssi_dead_link_streak);
      if (g_ctx.rssi_dead_link_streak >= BK300_RSSI_DEAD_LINK_STREAK) {
        bk300_force_disconnect("read_rssi=127 repeatedly");
      }
    }
    break;
  }

#if defined(CONFIG_BT_BLE_SMP_ENABLE) && CONFIG_BT_BLE_SMP_ENABLE
  case ESP_GAP_BLE_AUTH_CMPL_EVT:
    ESP_LOGW(TAG, "AUTH_CMPL: success=%d fail_reason=%d (BK300 не должен требовать авторизации!)",
             p->ble_security.auth_cmpl.success, p->ble_security.auth_cmpl.fail_reason);
    break;

  case ESP_GAP_BLE_SEC_REQ_EVT:
    ESP_LOGW(TAG, "SEC_REQ: peer requested security — отклоняем (BK300 не должен)");
    esp_ble_gap_security_rsp(p->ble_security.ble_req.bd_addr, false);
    break;
#endif

  default:
    ESP_LOGI(TAG, "GAP unhandled evt=%d", (int)event);
    break;
  }
}

// ---- Periodic stats task ----

static void stats_task(void *arg) {
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "stats: connected=%d kicked=%d notify_pkts=%" PRIu32
                  " bytes=%" PRIu32 " frames=%" PRIu32 " 4b0b=%" PRIu32 " badcrc=%" PRIu32,
             g_ctx.connected, g_ctx.kicked, g_ctx.notify_pkts, g_ctx.notify_bytes,
             g_ctx.frames_total, g_ctx.frames_4b0b, g_ctx.frames_badcrc);
  }
}

void bk300_driver_start_stats_task(void) {
  xTaskCreate(stats_task, "bk300_stats", 4096, NULL, 5, NULL);
}
