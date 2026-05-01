/**
 * BK300 frame protocol (CRC + builder + parser).
 * Identical bytes to web-monitor / Android и Arduino-скетчу из ../../esp32/.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// CRC-16/PPP (CRC-16/X25 reflected): poly=0x8408, init=0xFFFF, xorout=0xFFFF.
uint16_t bk300_crc16ppp(const uint8_t *data, size_t len);

// Собирает кадр `40 40 LL LL CC CC [payload...] CRC_LO CRC_HI 0D 0A`.
// Возвращает реальную длину или 0 если буфер слишком мал.
size_t bk300_build_frame(uint8_t *out,
                         size_t out_cap,
                         uint16_t cmd_le,           // bytes [4..5] little-endian
                         const uint8_t *payload,
                         size_t payload_len);

typedef struct {
  uint16_t length;       // полная длина кадра, должна совпадать с реальной
  uint16_t type_le;      // bytes [4..5] как u16le (для нас интересны 0x0B4B и т.п.)
  const uint8_t *payload;
  uint16_t payload_len;
  bool crc_ok;
} bk300_frame_t;

// Кладёт байты в внутренний rx-буфер; разбирает кадры по терминатору 0D 0A.
void bk300_rx_append(const uint8_t *data, size_t len);
bool bk300_rx_pop_frame(bk300_frame_t *out);

void bk300_rx_reset(void);

#ifdef __cplusplus
}
#endif
