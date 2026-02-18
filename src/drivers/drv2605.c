/**
 * DRV2605 haptic driver — TI ERM/LRA haptic driver over I2C.
 *
 * Uses I2C_NUM_0 (shared with CST816S touch sensor).
 * Touch_Init() must be called before drv2605_init().
 *
 * Register map references: TI DRV2605/DRV2605L datasheet.
 */

#include "drv2605.h"
#include "config.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"

#define TAG             "DRV2605"
#define DRV2605_ADDR    0x5A
#define I2C_PORT        I2C_NUM_0
#define I2C_TIMEOUT_MS  50

/* Register addresses */
#define REG_MODE        0x01   /* bit[2:0] mode, bit6 standby */
#define REG_LIBRARY     0x03   /* bits[2:0] library select */
#define REG_WAVESEQ1    0x04   /* first effect slot */
#define REG_WAVESEQ2    0x05   /* second slot — write 0x00 to terminate */
#define REG_GO          0x0C   /* bit0 = GO */
#define REG_RATED_V     0x16   /* Rated voltage — limits library effect amplitude */
#define REG_OD_CLAMP    0x17   /* Overdrive clamp voltage */
#define REG_FEEDBACK    0x1A   /* bit7: 0=ERM, 1=LRA */

/* Mode register values */
#define MODE_INTERNAL_TRIG  0x00   /* internal trigger, out of standby */

static bool s_initialized = false;

/* ---- internal helpers ---- */

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(I2C_PORT, DRV2605_ADDR,
                                      buf, sizeof(buf),
                                      pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static esp_err_t reg_read(uint8_t reg, uint8_t *out)
{
    return i2c_master_write_read_device(I2C_PORT, DRV2605_ADDR,
                                        &reg, 1,
                                        out, 1,
                                        pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

/* ---- public API ---- */

bool drv2605_init(void)
{
    /* Probe: attempt to exit standby.  Fails cleanly if chip absent. */
    if (reg_write(REG_MODE, MODE_INTERNAL_TRIG) != ESP_OK) {
        ESP_LOGW(TAG, "DRV2605 not found on I2C bus (addr 0x5A) — haptics disabled");
        return false;
    }

    /* ERM Library A (library 1). */
    reg_write(REG_LIBRARY, 0x01);

    /* Ensure ERM mode: clear bit 7 of feedback-control register. */
    uint8_t fb = 0;
    if (reg_read(REG_FEEDBACK, &fb) == ESP_OK) {
        reg_write(REG_FEEDBACK, fb & 0x7F);
    }

    /* Drive voltage — tunable in config.h (HAPTIC_RATED_VOLTAGE / HAPTIC_OD_CLAMP). */
    reg_write(REG_RATED_V,  HAPTIC_RATED_VOLTAGE);
    reg_write(REG_OD_CLAMP, HAPTIC_OD_CLAMP);

    s_initialized = true;
    ESP_LOGI(TAG, "DRV2605 initialized");
    return true;
}

bool drv2605_play(uint8_t effect_id)
{
    if (!s_initialized) return false;

    /* Skip if a previous effect is still playing. */
    uint8_t go = 0;
    if (reg_read(REG_GO, &go) != ESP_OK) return false;
    if (go & 0x01) return false;

    reg_write(REG_WAVESEQ1, effect_id);
    reg_write(REG_WAVESEQ2, 0x00);   /* terminate sequence after slot 1 */
    reg_write(REG_GO, 0x01);
    return true;
}

bool drv2605_is_playing(void)
{
    if (!s_initialized) return false;
    uint8_t go = 0;
    reg_read(REG_GO, &go);
    return (go & 0x01) != 0;
}
