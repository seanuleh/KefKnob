#include "mic_pdm.h"
#include "driver/i2s_pdm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

volatile uint8_t g_mic_level  = 0;
volatile uint8_t g_band_bass  = 0;
volatile uint8_t g_band_mid   = 0;
volatile uint8_t g_band_hmid  = 0;
volatile uint8_t g_band_high  = 0;

static i2s_chan_handle_t s_rx_chan = NULL;

// ---------------------------------------------------------------------------
// Log-scale helper: rms → 0–255 with a given noise floor.
// ---------------------------------------------------------------------------
static uint8_t log_level(float rms, float floor_val) {
    if (rms <= floor_val) return 0;
    int lv = (int)(logf(rms / floor_val) / logf(100.0f) * 255.0f);
    if (lv > 255) lv = 255;
    if (lv < 0)   lv = 0;
    return (uint8_t)lv;
}

// Asymmetric smoothing: fast attack (α=0.75), slow decay (α=0.25).
static void smooth(volatile uint8_t &out, uint8_t nw) {
    uint8_t prev = out;
    out = (nw >= prev)
        ? (uint8_t)((nw * 3 + prev + 1) / 4)
        : (uint8_t)((nw + prev * 3 + 1) / 4);
}

// ---------------------------------------------------------------------------
// Mic sampling task — Core 0, ~30 Hz (512 samples / 16 kHz = 32 ms/block)
//
// Per block:
//  1. DC blocking IIR removes the constant offset (hardware hp_en is a no-op
//     on ESP32-S3 — SOC_I2S_SUPPORTS_PDM_RX_HP_FILTER is not defined).
//  2. Three cascaded 1-pole IIR LPFs split the AC signal into four bands:
//       bass  (<250 Hz)  mid  (250–1kHz)  hmid (1–4kHz)  high (>4kHz)
//  3. RMS of each band is log-scaled and smoothed into the five globals.
// ---------------------------------------------------------------------------
static void mic_task(void *) {
    static int16_t buf[512];

    // IIR filter states (persist across blocks)
    static float dc      = 0.0f;   // DC blocker mean tracker
    static float lp_bass = 0.0f;   // 1-pole LPF cutoff ~250 Hz  (α=0.906)
    static float lp_mid  = 0.0f;   // 1-pole LPF cutoff ~1 kHz   (α=0.672)
    static float lp_hi   = 0.0f;   // 1-pole LPF cutoff ~4 kHz   (α=0.208)

    for (;;) {
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(s_rx_chan, buf, sizeof(buf),
                                          &bytes_read, pdMS_TO_TICKS(200));
        if (ret != ESP_OK || bytes_read == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int n = (int)(bytes_read / sizeof(int16_t));

        float sq_total = 0, sq_bass = 0, sq_mid = 0, sq_hmid = 0, sq_high = 0;
        for (int i = 0; i < n; i++) {
            // DC block: slow IIR tracks mean, subtract to get AC signal
            dc = dc * 0.995f + buf[i] * 0.005f;
            float ac = buf[i] - dc;

            // Cascaded IIR LPFs — coefficients for 16 kHz sample rate:
            //   α = exp(-2π·fc/fs):  250Hz→0.906  1kHz→0.672  4kHz→0.208
            lp_bass = lp_bass * 0.906f + ac * 0.094f;
            lp_mid  = lp_mid  * 0.672f + ac * 0.328f;
            lp_hi   = lp_hi   * 0.208f + ac * 0.792f;

            // Bandpass signals via difference of LPF outputs
            float b_bass = lp_bass;
            float b_mid  = lp_mid  - lp_bass;
            float b_hmid = lp_hi   - lp_mid;
            float b_high = ac      - lp_hi;

            sq_total += ac     * ac;
            sq_bass  += b_bass * b_bass;
            sq_mid   += b_mid  * b_mid;
            sq_hmid  += b_hmid * b_hmid;
            sq_high  += b_high * b_high;
        }

        float inv_n = 1.0f / n;
        smooth(g_mic_level, log_level(sqrtf(sq_total * inv_n), 7.0f));
        smooth(g_band_bass, log_level(sqrtf(sq_bass  * inv_n), 4.0f));
        smooth(g_band_mid,  log_level(sqrtf(sq_mid   * inv_n), 5.0f));
        smooth(g_band_hmid, log_level(sqrtf(sq_hmid  * inv_n), 6.0f));
        smooth(g_band_high, log_level(sqrtf(sq_high  * inv_n), 8.0f));
    }
}

// ---------------------------------------------------------------------------

bool mic_pdm_init(int clk_pin, int data_pin) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&chan_cfg, NULL, &s_rx_chan) != ESP_OK) {
        return false;
    }

    // PDM RX config — I2S_PDM_RX_SLOT_DEFAULT_CONFIG resolves to PCM format
    // on ESP32-S3 (SOC_I2S_SUPPORTS_PDM2PCM=1), so the hardware PDM→PCM filter
    // is active and readBytes() returns standard 16-bit PCM samples.
    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                    I2S_SLOT_MODE_MONO),
    };
    pdm_cfg.gpio_cfg.clk              = (gpio_num_t)clk_pin;
    pdm_cfg.gpio_cfg.din              = (gpio_num_t)data_pin;
    pdm_cfg.gpio_cfg.invert_flags.clk_inv = 0;

    if (i2s_channel_init_pdm_rx_mode(s_rx_chan, &pdm_cfg) != ESP_OK) {
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return false;
    }

    if (i2s_channel_enable(s_rx_chan) != ESP_OK) {
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return false;
    }

    xTaskCreatePinnedToCore(mic_task, "mic", 3072, NULL, 4, NULL, 0);
    return true;
}
