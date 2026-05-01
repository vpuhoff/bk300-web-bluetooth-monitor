#include "bk300_protocol.h"

#include <string.h>

#define BK300_RX_BUF_SIZE 256

// Внутренний rx-стейт. Отдельных кадров ждём по терминатору 0D 0A; префикс может быть 40 40 (TX-эхо)
// или 24 24 (response). Если переполнился — сбрасываем (как web-monitor).
static uint8_t s_rx_buf[BK300_RX_BUF_SIZE];
static size_t s_rx_len = 0;

uint16_t bk300_crc16ppp(const uint8_t *data, size_t len) {
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

size_t bk300_build_frame(uint8_t *out,
                         size_t out_cap,
                         uint16_t cmd_le,
                         const uint8_t *payload,
                         size_t payload_len) {
  const uint16_t length = (uint16_t)(payload_len + 10);
  if (out_cap < length) return 0;
  out[0] = 0x40;
  out[1] = 0x40;
  out[2] = (uint8_t)(length & 0xFF);
  out[3] = (uint8_t)((length >> 8) & 0xFF);
  out[4] = (uint8_t)(cmd_le & 0xFF);
  out[5] = (uint8_t)((cmd_le >> 8) & 0xFF);
  for (size_t i = 0; i < payload_len; i++) out[6 + i] = payload[i];

  const uint16_t crc = bk300_crc16ppp(out, length - 4);
  out[length - 4] = (uint8_t)(crc & 0xFF);
  out[length - 3] = (uint8_t)((crc >> 8) & 0xFF);
  out[length - 2] = 0x0D;
  out[length - 1] = 0x0A;
  return length;
}

void bk300_rx_reset(void) { s_rx_len = 0; }

void bk300_rx_append(const uint8_t *data, size_t len) {
  if (len == 0) return;
  if (s_rx_len + len > sizeof(s_rx_buf)) {
    s_rx_len = 0;  // best-effort drop, как в web-monitor popFrames
  }
  memcpy(s_rx_buf + s_rx_len, data, len);
  s_rx_len += len;
}

static int s_find_terminator(void) {
  for (size_t i = 0; i + 1 < s_rx_len; i++) {
    if (s_rx_buf[i] == 0x0D && s_rx_buf[i + 1] == 0x0A) return (int)i;
  }
  return -1;
}

bool bk300_rx_pop_frame(bk300_frame_t *out) {
  if (!out) return false;
  int term_idx = s_find_terminator();
  if (term_idx < 0) return false;
  size_t frame_len = (size_t)term_idx + 2;
  if (frame_len > s_rx_len) return false;
  if (frame_len < 10) {
    memmove(s_rx_buf, s_rx_buf + frame_len, s_rx_len - frame_len);
    s_rx_len -= frame_len;
    return false;
  }

  // Нам нужна копия кадра для CRC; используем отдельный буфер на стеке через указатель в rx_buf.
  static uint8_t s_pop_buf[BK300_RX_BUF_SIZE];
  memcpy(s_pop_buf, s_rx_buf, frame_len);
  memmove(s_rx_buf, s_rx_buf + frame_len, s_rx_len - frame_len);
  s_rx_len -= frame_len;

  const bool ok4040 = (s_pop_buf[0] == 0x40 && s_pop_buf[1] == 0x40);
  const bool ok2424 = (s_pop_buf[0] == 0x24 && s_pop_buf[1] == 0x24);
  if (!ok4040 && !ok2424) return false;

  uint16_t length = (uint16_t)(s_pop_buf[2] | (s_pop_buf[3] << 8));
  if (length != frame_len) return false;

  uint16_t type_le = (uint16_t)(s_pop_buf[4] | (s_pop_buf[5] << 8));
  uint16_t payload_len = (uint16_t)(length - 10);

  uint16_t crc_in = (uint16_t)(s_pop_buf[length - 4] | (s_pop_buf[length - 3] << 8));
  uint16_t crc_calc = bk300_crc16ppp(s_pop_buf, length - 4);

  out->length = length;
  out->type_le = type_le;
  out->payload = s_pop_buf + 6;
  out->payload_len = payload_len;
  out->crc_ok = (crc_in == crc_calc);
  return true;
}
