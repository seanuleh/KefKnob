#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * drv2605 — TI DRV2605 haptic driver (ERM/LRA) over I2C.
 *
 * Uses I2C_NUM_0, which must already be installed by Touch_Init().
 * Address: 0x5A (fixed).
 *
 * Call drv2605_init() once after Touch_Init(). If the chip is not wired,
 * init returns false and all drv2605_play() calls become no-ops.
 */

bool drv2605_init(void);

/**
 * Trigger a waveform-library effect by ID (ERM Library 1).
 * Returns true if the effect was queued, false if skipped (not init'd or GO still set).
 * effect_id: 1-123 (ERM Library A).
 */
bool drv2605_play(uint8_t effect_id);

/** True while the GO bit is set (effect still running). */
bool drv2605_is_playing(void);

#ifdef __cplusplus
}
#endif
