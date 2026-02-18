#include "mic_pdm.h"
#include "driver/i2s_pdm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

volatile uint8_t g_mic_level = 0;

static i2s_chan_handle_t s_rx_chan = NULL;

// ---------------------------------------------------------------------------
// Mic sampling task — Core 0, ~30 Hz (512 samples / 16 kHz = 32 ms/block)
//
// Reads a block of 16-bit PCM samples from the PDM2PCM hardware filter,
// computes RMS, log-scales it to 0–255, and writes g_mic_level with
// asymmetric smoothing (fast attack, slow decay) for visual comfort.
// ---------------------------------------------------------------------------

static void mic_task(void *) {
    static int16_t buf[512];
    // DC blocking: IIR low-pass tracks the mean (α=0.995 → ~200-sample time constant).
    // Subtracting it from each sample removes the DC offset that otherwise dominates
    // the RMS and pins the bars at max.  The hardware hp_en filter is a no-op on
    // ESP32-S3 (SOC_I2S_SUPPORTS_PDM_RX_HP_FILTER not defined).
    static float dc = 0.0f;

    for (;;) {
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(s_rx_chan, buf, sizeof(buf),
                                          &bytes_read, pdMS_TO_TICKS(200));
        if (ret != ESP_OK || bytes_read == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int n = (int)(bytes_read / sizeof(int16_t));

        // Remove DC offset then compute RMS on AC signal only
        int64_t sum_sq = 0;
        for (int i = 0; i < n; i++) {
            dc = dc * 0.995f + buf[i] * 0.005f;   // track mean
            float ac = buf[i] - dc;
            sum_sq += (int64_t)(ac * ac);
        }
        float rms = sqrtf((float)sum_sq / n);

        // Log-scale on AC-only RMS. Floor at 30 LSB (~mic thermal noise after DC removal).
        // Saturates at ~3000 LSB (loud music / shouting at close range).
        // amplify_num is not available on ESP32-S3 (SOC_I2S_SUPPORTS_PDM_RX_HP_FILTER absent).
        int level = 0;
        if (rms > 7.0f) {
            level = (int)(logf(rms / 7.0f) / logf(100.0f) * 255.0f);
            if (level > 255) level = 255;
            if (level < 0)   level = 0;
        }

        // Asymmetric smoothing: fast attack, slow decay
        uint8_t prev = g_mic_level;
        if ((uint8_t)level >= prev) {
            g_mic_level = (uint8_t)((level * 3 + prev + 1) / 4);   // α=0.75 attack
        } else {
            g_mic_level = (uint8_t)((level + prev * 3 + 1) / 4);   // α=0.25 decay
        }
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
    pdm_cfg.gpio_cfg.clk             = (gpio_num_t)clk_pin;
    pdm_cfg.gpio_cfg.din             = (gpio_num_t)data_pin;
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
