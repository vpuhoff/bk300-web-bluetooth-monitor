#pragma once
/**
 * NTP time sync via WiFi.
 *
 * Sequence:
 *   1. Stop BLE (caller must do this before calling ntp_sync_run).
 *   2. Connect WiFi, sync SNTP, disconnect WiFi.
 *   3. RTC (settimeofday) is updated — subsequent time(NULL) returns real epoch.
 *   4. Caller restarts BLE.
 *
 * ntp_sync_run() is blocking, call from a FreeRTOS task (not from BTC/GAP callbacks).
 */

#include <stdbool.h>
#include <time.h>

/**
 * Run full WiFi+NTP sync cycle.
 * Blocks until sync completes or timeout.
 * @return true if time was successfully synchronised.
 */
bool ntp_sync_run(void);

/**
 * Returns true if the system clock holds a real wall-clock time
 * (year >= 2024), false if it's still the default epoch (Jan 1 1970 / boot time).
 */
bool ntp_sync_time_is_valid(void);

/**
 * Returns current Unix epoch (time(NULL)).
 * If clock is not yet valid, returns CONFIG_BK300_FALLBACK_EPOCH.
 */
time_t ntp_sync_get_epoch(void);
