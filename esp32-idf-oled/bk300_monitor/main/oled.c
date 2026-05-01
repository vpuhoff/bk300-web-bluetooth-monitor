/**
 * SSD1306 через официальный стек ESP-IDF: esp_lcd_panel_io_i2c + esp_lcd_new_panel_ssd1306
 * (как в examples/peripherals/lcd/i2c_oled). Рендер — локальный framebuffer + draw_bitmap.
 */
#include "oled.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "oled_font.h"

#define TAG "oled"

#if !defined(CONFIG_BK300_OLED_ENABLED) || !CONFIG_BK300_OLED_ENABLED

esp_err_t oled_init(void) { return ESP_OK; }
void oled_set_status(const char *s) { (void)s; }
void oled_set_voltage(float v) { (void)v; }
void oled_kick(void) {}

#else

// ============================================================
// ===                  Hardware constants                  ===
// ============================================================
#define SSD1306_W           128
#define SSD1306_H           64
#define SSD1306_PAGES       (SSD1306_H / 8)

// ============================================================
// ===                      State                           ===
// ============================================================
static i2c_master_bus_handle_t      s_bus = NULL;
static esp_lcd_panel_io_handle_t    s_io = NULL;
static esp_lcd_panel_handle_t       s_panel = NULL;
static bool                         s_present = false;

static uint8_t                      s_fb[SSD1306_W * SSD1306_PAGES];

static SemaphoreHandle_t            s_mutex = NULL;
static char                         s_status[40] = "Booting...";
static float                        s_voltage = -1.0f;
static int64_t                      s_voltage_ts_us = 0;
static bool                         s_dirty = true;

// ============================================================
// ===        LCD install / teardown (per addr+freq try)    ===
// ============================================================
static void oled_rm_lcd(void) {
  if (s_panel) {
    (void)esp_lcd_panel_del(s_panel);
    s_panel = NULL;
  }
  if (s_io) {
    (void)esp_lcd_panel_io_del(s_io);
    s_io = NULL;
  }
}

static void oled_try_install_lcd_cleanup(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t io) {
  if (panel) {
    (void)esp_lcd_panel_del(panel);
  }
  if (io) {
    (void)esp_lcd_panel_io_del(io);
  }
}

// Пробуем поднять panel+io для одного адреса и частоты. При успехе — panel/io в static s_*.
static esp_err_t oled_try_init_one(uint16_t addr_7bit, uint32_t scl_hz, const char *attempt_tag) {
  esp_lcd_panel_io_handle_t io = NULL;
  esp_lcd_panel_handle_t      panel = NULL;

  esp_err_t e = i2c_master_probe(s_bus, addr_7bit, pdMS_TO_TICKS(150));
  if (e != ESP_OK) {
    ESP_LOGW(TAG, "probe %s: addr=0x%02X err=%d", attempt_tag, addr_7bit, e);
    return e;
  }
  (void)i2c_master_bus_wait_all_done(s_bus, 100);

  esp_lcd_panel_io_i2c_config_t io_cfg = {
      .dev_addr = addr_7bit,
      .scl_speed_hz = scl_hz,
      .control_phase_bytes = 1,
      .lcd_cmd_bits = 8,
      .lcd_param_bits = 8,
      .dc_bit_offset = 6,  // SSD1306 datasheet (Co/D-C#)
  };
  e = esp_lcd_new_panel_io_i2c(s_bus, &io_cfg, &io);
  if (e != ESP_OK) {
    ESP_LOGW(TAG, "new_panel_io %s addr=0x%02X err=%d", attempt_tag, addr_7bit, e);
    return e;
  }

  esp_lcd_panel_ssd1306_config_t ssd_cfg = {
      .height = SSD1306_H,
  };
  esp_lcd_panel_dev_config_t panel_cfg = {
      .reset_gpio_num = -1,
      .bits_per_pixel = 1,
      .vendor_config = &ssd_cfg,
  };

  e = esp_lcd_new_panel_ssd1306(io, &panel_cfg, &panel);
  if (e != ESP_OK) {
    ESP_LOGW(TAG, "new_panel_ssd1306 %s err=%d", attempt_tag, e);
    oled_try_install_lcd_cleanup(panel, io);
    return e;
  }

  e = esp_lcd_panel_reset(panel);
  if (e != ESP_OK) goto fail;

  e = esp_lcd_panel_init(panel);
  if (e != ESP_OK) goto fail;

#if !defined(CONFIG_BK300_OLED_ROTATE_180) || !CONFIG_BK300_OLED_ROTATE_180
  // Дефолт драйвера после init = «как» физический 180° в старом oled.c; для обычной ориентации —
  // зеркалим X+Y как у Adafruit SEGREMAP_ON / COMSCAN_DEC.
  e = esp_lcd_panel_mirror(panel, true, true);
  if (e != ESP_OK) goto fail;
#endif

  e = esp_lcd_panel_disp_on_off(panel, true);
  if (e != ESP_OK) goto fail;

  // В старых IDF в esp_lcd_panel_ssd1306_config_t нет contrast — выставляем вручную (0x81 + уровень).
  {
    const uint8_t contrast = 0xCF;
    e = esp_lcd_panel_io_tx_param(io, 0x81, &contrast, 1);
    if (e != ESP_OK) goto fail;
  }

  s_io = io;
  s_panel = panel;
  return ESP_OK;

fail:
  ESP_LOGW(TAG, "panel bringup %s: addr=0x%02X freq=%" PRIu32 " err=%d (%s)",
           attempt_tag, addr_7bit, scl_hz, e, esp_err_to_name(e));
  oled_try_install_lcd_cleanup(panel, io);
  (void)i2c_master_bus_reset(s_bus);
  (void)i2c_master_bus_wait_all_done(s_bus, 100);
  return e;
}

static esp_err_t ssd1306_flush_fb(void) {
  return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, SSD1306_W, SSD1306_H, s_fb);
}

// ============================================================
// ===                Framebuffer drawing                   ===
// ============================================================
static inline void fb_clear(void) {
  memset(s_fb, 0, sizeof(s_fb));
}

static inline void fb_pixel(int x, int y, bool on) {
  if (x < 0 || x >= SSD1306_W || y < 0 || y >= SSD1306_H) return;
  uint8_t *p = &s_fb[(y >> 3) * SSD1306_W + x];
  uint8_t mask = (uint8_t)(1u << (y & 7));
  if (on) *p |= mask; else *p &= (uint8_t)~mask;
}

static void fb_hline(int x0, int x1, int y, bool on) {
  if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
  for (int x = x0; x <= x1; x++) fb_pixel(x, y, on);
}

static int fb_draw_glyph(int x, int y, char ch, int scale) {
  if (scale < 1) scale = 1;
  if ((unsigned char)ch < OLED_FONT_FIRST_CHAR || (unsigned char)ch > OLED_FONT_LAST_CHAR) {
    ch = '?';
  }
  const uint8_t *col = &oled_font5x7[((unsigned char)ch - OLED_FONT_FIRST_CHAR) * OLED_FONT_GLYPH_W];
  for (int c = 0; c < OLED_FONT_GLYPH_W; c++) {
    uint8_t bits = col[c];
    for (int r = 0; r < OLED_FONT_GLYPH_H; r++) {
      if (bits & (1u << r)) {
        for (int dy = 0; dy < scale; dy++) {
          for (int dx = 0; dx < scale; dx++) {
            fb_pixel(x + c * scale + dx, y + r * scale + dy, true);
          }
        }
      }
    }
  }
  return scale * OLED_FONT_ADVANCE_W;
}

static int fb_draw_string(int x, int y, const char *s, int scale) {
  while (*s) {
    x += fb_draw_glyph(x, y, *s++, scale);
    if (x >= SSD1306_W) break;
  }
  return x;
}

static int text_width(const char *s, int scale) {
  int n = 0;
  while (*s++) n++;
  return n * scale * OLED_FONT_ADVANCE_W;
}

// ============================================================
// ===                       UI                             ===
// ============================================================
static void ui_format_uptime(char *out, size_t cap, uint32_t up_s) {
  if (up_s < 60) {
    snprintf(out, cap, "%us", (unsigned)up_s);
  } else if (up_s < 3600) {
    snprintf(out, cap, "%um%us", (unsigned)(up_s / 60), (unsigned)(up_s % 60));
  } else if (up_s < 24 * 3600) {
    snprintf(out, cap, "%uh%um", (unsigned)(up_s / 3600), (unsigned)((up_s % 3600) / 60));
  } else {
    snprintf(out, cap, "%ud%uh", (unsigned)(up_s / 86400), (unsigned)((up_s % 86400) / 3600));
  }
}

static void ui_render(const char *status, float voltage, int64_t voltage_ts_us) {
  fb_clear();

  fb_draw_string(0, 0, "BK300: ", 1);
  if (status && status[0]) {
    fb_draw_string(7 * OLED_FONT_ADVANCE_W, 0, status, 1);
  }

  fb_hline(0, SSD1306_W - 1, 11, true);

  char volt_buf[12];
  if (voltage >= 0.0f) {
    snprintf(volt_buf, sizeof(volt_buf), "%.2f", (double)voltage);
  } else {
    snprintf(volt_buf, sizeof(volt_buf), "--.--");
  }
  int volt_w = text_width(volt_buf, 4);
  int x_volt = 0;
  fb_draw_string(x_volt, 18, volt_buf, 4);
  fb_draw_string(x_volt + volt_w + 2, 22, "V", 3);

  uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
  char up_buf[16];
  ui_format_uptime(up_buf, sizeof(up_buf), up_s);
  fb_draw_string(0, SSD1306_H - 8, up_buf, 1);

  if (voltage_ts_us > 0) {
    uint32_t age_s = (uint32_t)((esp_timer_get_time() - voltage_ts_us) / 1000000ULL);
    char rx_buf[16];
    if (age_s < 100) {
      snprintf(rx_buf, sizeof(rx_buf), "rx %us", (unsigned)age_s);
    } else if (age_s < 999) {
      snprintf(rx_buf, sizeof(rx_buf), "rx %us", (unsigned)age_s);
    } else {
      snprintf(rx_buf, sizeof(rx_buf), "rx>1ks");
    }
    int rx_w = text_width(rx_buf, 1);
    fb_draw_string(SSD1306_W - rx_w, SSD1306_H - 8, rx_buf, 1);
  }
}

// ============================================================
// ===                  Render task                         ===
// ============================================================
static void oled_task(void *arg) {
  (void)arg;
  const uint32_t MIN_REFRESH_MS = 200;
  const uint32_t FORCE_REFRESH_MS = 1000;
  uint32_t last_render_ms = 0;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!s_present) continue;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    bool dirty_local;

    char       status_local[sizeof(s_status)];
    float      voltage_local;
    int64_t    voltage_ts_local;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) continue;
    dirty_local = s_dirty;
    s_dirty = false;
    strncpy(status_local, s_status, sizeof(status_local));
    status_local[sizeof(status_local) - 1] = 0;
    voltage_local = s_voltage;
    voltage_ts_local = s_voltage_ts_us;
    xSemaphoreGive(s_mutex);

    bool force = (now_ms - last_render_ms) >= FORCE_REFRESH_MS;
    bool can_dirty = (now_ms - last_render_ms) >= MIN_REFRESH_MS;
    if (!(force || (dirty_local && can_dirty))) continue;

    ui_render(status_local, voltage_local, voltage_ts_local);
    esp_err_t e = ssd1306_flush_fb();
    if (e != ESP_OK) {
      ESP_LOGW(TAG, "flush err=%d %s", e, esp_err_to_name(e));
    }
    last_render_ms = now_ms;
  }
}

// ============================================================
// ===                   Public API                         ===
// ============================================================
esp_err_t oled_init(void) {
  if (s_mutex) return ESP_OK;
  s_mutex = xSemaphoreCreateMutex();
  if (!s_mutex) return ESP_ERR_NO_MEM;

  i2c_master_bus_config_t bus_cfg = {
      .i2c_port = CONFIG_BK300_OLED_I2C_PORT,
      .sda_io_num = CONFIG_BK300_OLED_I2C_SDA_GPIO,
      .scl_io_num = CONFIG_BK300_OLED_I2C_SCL_GPIO,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  esp_err_t e = i2c_new_master_bus(&bus_cfg, &s_bus);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "i2c_new_master_bus err=%d (sda=%d scl=%d port=%d)",
             e, CONFIG_BK300_OLED_I2C_SDA_GPIO, CONFIG_BK300_OLED_I2C_SCL_GPIO,
             CONFIG_BK300_OLED_I2C_PORT);
    return e;
  }

  const uint16_t cfg_addr = CONFIG_BK300_OLED_I2C_ADDR;
  const uint32_t cfg_freq = CONFIG_BK300_OLED_I2C_FREQ_HZ;

  uint16_t addrs[2];
  int n_addr = 1;
  addrs[0] = cfg_addr;
#if defined(CONFIG_BK300_OLED_TRY_ALT_ADDR) && CONFIG_BK300_OLED_TRY_ALT_ADDR
  addrs[1] = (cfg_addr == 0x3C) ? 0x3D : 0x3C;
  n_addr = 2;
#endif

  uint32_t freqs[2];
  int n_freq = 1;
  freqs[0] = cfg_freq;
  if (cfg_freq != 100000) {
    freqs[1] = 100000;
    n_freq = 2;
  }

  esp_err_t last_err = ESP_FAIL;
  uint16_t ok_addr = 0;
  uint32_t ok_freq = 0;
  bool success = false;

  for (int ai = 0; ai < n_addr && !success; ai++) {
    for (int fi = 0; fi < n_freq && !success; fi++) {
      uint32_t try_hz = freqs[fi];
      uint16_t try_addr = addrs[ai];
      char tag[28];
      snprintf(tag, sizeof(tag), "a%02X_f%" PRIu32, (unsigned)try_addr, try_hz);
      e = oled_try_init_one(try_addr, try_hz, tag);
      if (e == ESP_OK) {
        success = true;
        ok_addr = try_addr;
        ok_freq = try_hz;
        break;
      }
      last_err = e;
    }
  }

  if (!success) {
    oled_rm_lcd();
    if (s_bus) {
      i2c_del_master_bus(s_bus);
      s_bus = NULL;
    }
    ESP_LOGW(TAG,
             "SSD1306: все попытки инициализации неудачны (last err=%d %s) — работаем без экрана",
             last_err, esp_err_to_name(last_err));
    return ESP_OK;
  }

  fb_clear();
  e = ssd1306_flush_fb();
  if (e != ESP_OK) {
    ESP_LOGW(TAG, "first flush err=%d %s", e, esp_err_to_name(e));
  }
  s_present = true;
  ESP_LOGI(TAG,
           "SSD1306 ok (esp_lcd): I2C%d sda=%d scl=%d addr=0x%02X freq=%" PRIu32
           " Hz (Kconfig было %" PRIu32 " Hz @ 0x%02X)",
           CONFIG_BK300_OLED_I2C_PORT, CONFIG_BK300_OLED_I2C_SDA_GPIO,
           CONFIG_BK300_OLED_I2C_SCL_GPIO, ok_addr, ok_freq, cfg_freq,
           (unsigned)cfg_addr);

  xTaskCreate(oled_task, "oled_render", 4096, NULL, 4, NULL);
  return ESP_OK;
}

void oled_set_status(const char *status) {
  if (!s_mutex) return;
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  if (status && status[0]) {
    if (strncmp(s_status, status, sizeof(s_status)) != 0) {
      strncpy(s_status, status, sizeof(s_status) - 1);
      s_status[sizeof(s_status) - 1] = 0;
      s_dirty = true;
    }
  }
  xSemaphoreGive(s_mutex);
}

void oled_set_voltage(float volts) {
  if (!s_mutex) return;
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  if (s_voltage < 0.0f || fabsf(s_voltage - volts) >= 0.005f) {
    s_voltage = volts;
    s_dirty = true;
  }
  s_voltage_ts_us = esp_timer_get_time();
  xSemaphoreGive(s_mutex);
}

void oled_kick(void) {
  if (!s_mutex) return;
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
  s_dirty = true;
  xSemaphoreGive(s_mutex);
}

#endif // CONFIG_BK300_OLED_ENABLED
