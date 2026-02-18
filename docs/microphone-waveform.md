# Microphone & Waveform Visualiser — Agent Context Document

> **Purpose:** Everything a future agent needs to work on the microphone or waveform feature without doing additional research. Covers hardware, firmware, IDF API version, signal processing, LVGL canvas, threading, and known pitfalls.

---

## Current status — working, visually tuned

**Status: working and visually confirmed on device.** The multicolour animated sine-wave visualiser is running. The renderer, auto-gain, layout, and transparency are all tuned and stable. Further work is optional cosmetic tuning only.

### What has been built and confirmed working

1. **`src/drivers/mic_pdm.cpp`** — `mic_task` runs DC blocking + 4 IIR bandpass filters on every 512-sample block. Produces 5 globals: `g_mic_level` (overall) + `g_band_bass / mid / hmid / high` (per band). Smoothed with fast-attack / slow-decay IIR.

2. **`src/drivers/mic_pdm.h`** — `extern volatile uint8_t` declarations for all 5 globals.

3. **`src/ui/main_screen.cpp`** — `main_screen_update_waveform()` is a per-pixel glow-line renderer writing directly to the `LV_IMG_CF_TRUE_COLOR_ALPHA` canvas buffer. 4 sine waves, one per band, with per-band auto-gain normalization and additive RGB blending.

4. **`include/config.h`** — `WAVE_CANVAS_H` = 56 px, `WAVE_CANVAS_W` = 160 px.

### Key decisions made (do not undo without reading why)

| Decision | Reason |
|---|---|
| `LV_IMG_CF_TRUE_COLOR_ALPHA` (3 bytes/pixel) | Transparent background — unlit pixels show album art beneath |
| Linear glow falloff (`t`, not `t*t`) | Quadratic caused dark near-black pixels at glow edges ("black border"); linear keeps the full glow width uniformly coloured |
| Write threshold `> 10.0f` per component | Clips the outermost ~5% of glow radius cleanly to transparent |
| Per-band auto-gain (not global) | Bass dominates global peak → other bands stay dim; per-band each wave fills canvas independently |
| `mhh = cy - glow - 2.0f` (dynamic) | Wave peak + glow never clips canvas edge regardless of glow setting |
| Multiply glow by `win` (Hann), no `|p|>0.5` gate | Gate caused breaks at sine zero-crossings; Hann multiplication naturally suppresses canvas-edge bleed |
| `lv_obj_set_style_border_width(s_wave_canvas, 0, 0)` | Removes default LVGL widget border |

---

## Key code locations for tuning

| What to change | File | What to look for |
|---|---|---|
| Per-band noise floors | `src/drivers/mic_pdm.cpp` | `smooth(g_band_bass, log_level(..., 4.0f))` etc. |
| Glow radius (line thickness) | `src/ui/main_screen.cpp` | `const float glow = 5.5f` |
| Wave max amplitude | `src/ui/main_screen.cpp` | `const float mhh = (float)cy - glow - 2.0f` — tied to glow |
| Write threshold (edge crispness) | `src/ui/main_screen.cpp` | `if (fr > 10.0f \|\| fg > 10.0f \|\| fb > 10.0f)` |
| Auto-gain decay speed | `src/ui/main_screen.cpp` | `constexpr float DECAY = 0.982f` (~3 s half-life at 12.5 Hz) |
| Auto-gain silence floor | `src/ui/main_screen.cpp` | `constexpr float FLOOR = 0.15f` |
| Phase animation speed | `src/ui/main_screen.cpp` | `t_bass += 0.07f` etc. — larger = faster |
| Spatial frequency (wave cycles) | `src/ui/main_screen.cpp` | `sinf(2.0f * 3.14159f * 1.5f * cx ...)` — change `1.5f` |
| Wave colours | `src/ui/main_screen.cpp` | `fr = g_bass * 20 + g_mid * 0 + ...` — the multipliers are R,G,B |
| Canvas size | `include/config.h` | `WAVE_CANVAS_W` / `WAVE_CANVAS_H` |
| Canvas position | `src/ui/main_screen.cpp` | `lv_obj_align(s_wave_canvas, LV_ALIGN_CENTER, 0, 62)` |
| Update rate | `include/config.h` | `MIC_BAR_MS` (currently 80 ms = 12.5 Hz) |
| IIR filter cutoffs | `src/drivers/mic_pdm.cpp` | `lp_bass * 0.906f` etc. — α = exp(-2π·fc/fs) |

---

## 1. Hardware

### Microphone IC

| Field | Value |
|---|---|
| Part | **MSM261D4030H1CPM** PDM MEMS microphone |
| Interface | PDM (Pulse Density Modulation) |
| Supply | 3.3 V |
| L/R select pin | Tied to **GND** → left channel |
| GPIO CLK | **45** (`MIC_PDM_CLK_PIN` in `include/config.h`) |
| GPIO DATA | **46** (`MIC_PDM_DATA_PIN` in `include/config.h`) |

The schematic label for the signals is `PDM MIC SCK` (GPIO 45) and `PDM MIC DATA` (GPIO 46). Confirmed against the Waveshare demo project at `Arduino/examples/07_Audio_Test/user_config.h` and `ESP-IDF/07_Audio_Test/main/user_config.h` — both specify GPIO 45 (CLK) and GPIO 46 (DATA) on the ESP32-S3.

### Two-chip board architecture

The Waveshare ESP32-S3 Knob Touch LCD 1.8" board contains **two MCUs**:

| Chip | Role |
|---|---|
| **ESP32-S3-R8** | Main MCU — display, touch, encoder, WiFi, mic PDM RX |
| **ESP32-240M** | Audio co-processor — I2S DAC output (PCM5100A / PCM speaker) |

The mic is wired to the ESP32-S3 (confirmed by pin numbers matching S3 GPIO space and the Waveshare demo using the S3 I2S0 PDM RX peripheral). The secondary ESP32 handles audio output only (see `5_DAC.png` schematic page). Do not confuse the two chips when debugging audio issues.

### I2S peripheral assignment

The ESP32-S3 has two I2S peripherals: `I2S_NUM_0` and `I2S_NUM_1`.

| Peripheral | Used by |
|---|---|
| `I2S_NUM_0` | **PDM mic** (this feature) |
| `I2S_NUM_1` | Free |
| SPI2/SPI3 | SH8601 QSPI display |
| I2C_NUM_0 | CST816S touch + DRV2605 haptic (shared) |

There is no conflict. GPIO 45 and 46 are not used by any other subsystem.

### ESP32-S3 PDM2PCM hardware filter

The ESP32-S3 has a **hardware PDM-to-PCM decimation filter** (`SOC_I2S_SUPPORTS_PDM2PCM = 1`). This means:
- The raw 1-bit PDM stream is automatically converted to 16-bit linear PCM by the silicon.
- Software receives standard signed 16-bit PCM samples — no manual decimation required.
- `I2S_PDM_RX_SLOT_DEFAULT_CONFIG` automatically selects `I2S_PDM_DATA_FMT_PCM` on ESP32-S3.

---

## 2. Build system & IDF version

### Framework versions in use

```
platform  : espressif32 (55.3.31)
framework : arduino
framework-arduinoespressif32-libs : 5.5.0+sha.129cd0d247   ← IDF 5.5
framework-arduinoespressif32@src  : 3.3.0 (Arduino library sources)
```

The project uses **Arduino-ESP32 3.x backed by ESP-IDF 5.5**. This is critical for I2S API selection (see §3).

### Include paths for I2S headers (verified from `idedata.json`)

```
.platformio/packages/framework-arduinoespressif32-libs/esp32s3/include/esp_driver_i2s/include/
```

Key headers inside that directory:
```
driver/i2s_common.h   — i2s_new_channel, I2S_CHANNEL_DEFAULT_CONFIG, i2s_channel_read
driver/i2s_pdm.h      — i2s_pdm_rx_config_t, I2S_PDM_RX_CLK_DEFAULT_CONFIG, etc.
```

There is also an **Arduino wrapper** library:
```
framework-arduinoespressif32@src-.../libraries/ESP_I2S/src/ESP_I2S.h
```
This is **not used** by the mic driver (see §3 — Why we use the raw IDF API).

---

## 3. I2S PDM API — the right one to use

### ❌ Do NOT use the legacy API

`driver/deprecated/driver/i2s.h` is present in the include path but must be avoided:
- It defines `i2s_mode_t` as a bitmask enum (`I2S_MODE_MASTER | I2S_MODE_PDM | I2S_MODE_RX`).
- `ESP_I2S.h` also defines its own `i2s_mode_t` (different values).
- Including both causes a **compile-time redefinition error**.
- The legacy API is deprecated in IDF 5.x.

### ❌ Do NOT use the Arduino ESP_I2S wrapper

`ESP_I2S.h` (`I2SClass`) is available and does work for PDM RX, but was avoided because its `i2s_mode_t` typedef conflicts with the legacy IDF type if either is transitively pulled in. The raw IDF API has no such risk.

### ✅ Use the new IDF 5.x API directly

```cpp
#include "driver/i2s_pdm.h"    // pulls in driver/i2s_common.h automatically
```

#### Full init sequence (copy-paste ready)

```cpp
i2s_chan_handle_t rx_chan = NULL;

// 1. Allocate channel (RX only — pass NULL for TX handle)
i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
i2s_new_channel(&chan_cfg, NULL, &rx_chan);

// 2. Configure PDM RX mode
i2s_pdm_rx_config_t pdm_cfg = {
    .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(16000),   // 16 kHz
    .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(
                    I2S_DATA_BIT_WIDTH_16BIT,
                    I2S_SLOT_MODE_MONO),
};
// Set GPIO pins separately to avoid C++ anonymous-union designated-init issues:
pdm_cfg.gpio_cfg.clk              = (gpio_num_t)45;   // PDM clock
pdm_cfg.gpio_cfg.din              = (gpio_num_t)46;   // PDM data
pdm_cfg.gpio_cfg.invert_flags.clk_inv = 0;

i2s_channel_init_pdm_rx_mode(rx_chan, &pdm_cfg);

// 3. Start
i2s_channel_enable(rx_chan);

// 4. Read (blocking, call from a dedicated FreeRTOS task)
int16_t buf[512];
size_t bytes_read = 0;
i2s_channel_read(rx_chan, buf, sizeof(buf), &bytes_read, pdMS_TO_TICKS(200));
```

#### Why the gpio_cfg is set outside the struct literal

`i2s_pdm_rx_gpio_config_t` contains an anonymous union `{ gpio_num_t din; gpio_num_t dins[N]; }`. C++ designated initialisers cannot target anonymous union members inside a nested literal. The workaround is to default-initialise the struct (via the outer literal's zero-fill) then assign the fields directly.

#### `I2S_PDM_RX_SLOT_DEFAULT_CONFIG` on ESP32-S3 expands to

```c
// Resolves to I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG because SOC_I2S_SUPPORTS_PDM2PCM=1
{
    .data_bit_width     = I2S_DATA_BIT_WIDTH_16BIT,
    .slot_bit_width     = I2S_SLOT_BIT_WIDTH_AUTO,
    .slot_mode          = I2S_SLOT_MODE_MONO,
    .slot_mask          = I2S_PDM_SLOT_LEFT,          // L/R=GND → left channel
    .data_fmt           = I2S_PDM_DATA_FMT_PCM,       // hardware decimation active
    .hp_en              = false,      // ← NO EFFECT on ESP32-S3 (see §10)
    .hp_cut_off_freq_hz = 35.5,       // ← NO EFFECT on ESP32-S3 (see §10)
    .amplify_num        = 1,          // ← NO EFFECT on ESP32-S3 (see §10)
}
```

**Critical:** `hp_en`, `hp_cut_off_freq_hz`, and `amplify_num` are **all no-ops on ESP32-S3**. They are gated by `SOC_I2S_SUPPORTS_PDM_RX_HP_FILTER`, which is not defined for ESP32-S3 (only `SOC_I2S_SUPPORTS_PDM2PCM` is defined). The struct fields don't even exist at compile time on this chip — attempting to set `slot_cfg.amplify_num` will cause a compile error (`struct has no member named 'amplify_num'`). Use software DC blocking and software gain instead (see §4).

---

## 4. Signal processing pipeline

### Task parameters

| Parameter | Value |
|---|---|
| Core | 0 |
| Priority | 4 |
| Stack | 3072 bytes |
| Block size | 512 × int16_t = 1024 bytes |
| Read interval | 512 / 16000 = **32 ms** (~30 Hz) |

### Step 1 — DC blocking (essential)

**This step is essential and must remain first in the processing chain.** The hardware HP filter (`hp_en`) is a no-op on ESP32-S3, so the raw PCM output has a large DC offset that without removal pins the bars at 80–90% of maximum even in complete silence.

```cpp
static float dc = 0.0f;   // persists across blocks

dc = dc * 0.995f + buf[i] * 0.005f;   // IIR low-pass tracks the mean
float ac = buf[i] - dc;               // AC-only signal — use this for everything below
```

α = 0.995 → time constant ~200 samples (~12.5 ms at 16 kHz), corner frequency ~12 Hz.

### Step 2 — IIR bandpass filters (current implementation)

Three cascaded 1-pole IIR low-pass filters split the AC signal into four frequency bands. Coefficients for 16 kHz sample rate (α = exp(-2π·fc/fs)):

```cpp
static float lp_bass = 0.0f;   // LPF @250 Hz,  α=0.906, coeff=0.094
static float lp_mid  = 0.0f;   // LPF @1 kHz,   α=0.672, coeff=0.328
static float lp_hi   = 0.0f;   // LPF @4 kHz,   α=0.208, coeff=0.792

lp_bass = lp_bass * 0.906f + ac * 0.094f;
lp_mid  = lp_mid  * 0.672f + ac * 0.328f;
lp_hi   = lp_hi   * 0.208f + ac * 0.792f;

float b_bass = lp_bass;            // <250 Hz
float b_mid  = lp_mid  - lp_bass; // 250 Hz – 1 kHz
float b_hmid = lp_hi   - lp_mid;  // 1 kHz – 4 kHz
float b_high = ac      - lp_hi;   // >4 kHz
```

### Step 3 — RMS per band + log-scale + smoothing

RMS of each band's squared samples, log-scaled to 0–255, smoothed with fast attack / slow decay. See `src/drivers/mic_pdm.cpp` `mic_task()` for the full implementation.

Current noise floors (rough starting point — raise a band's floor if it lights up in silence):

| Band | Global | Noise floor | Approx frequency range |
|---|---|---|---|
| Overall | `g_mic_level` | 7.0 | Full spectrum |
| Bass | `g_band_bass` | 4.0 | < 250 Hz |
| Mid | `g_band_mid` | 5.0 | 250 Hz – 1 kHz |
| High-mid | `g_band_hmid` | 6.0 | 1 kHz – 4 kHz |
| High | `g_band_high` | 8.0 | > 4 kHz |

### Shared state

All 5 globals are `volatile uint8_t`, written at ~30 Hz on Core 0, read on Core 1. Single-byte volatile reads are atomic on Xtensa LX7 — no mutex needed.

```cpp
// mic_pdm.h
extern volatile uint8_t g_mic_level;
extern volatile uint8_t g_band_bass;
extern volatile uint8_t g_band_mid;
extern volatile uint8_t g_band_hmid;
extern volatile uint8_t g_band_high;
```

---

## 5. LVGL waveform canvas

### Widget properties

| Property | Value |
|---|---|
| Widget | `lv_canvas_t` |
| Buffer format | `LV_IMG_CF_TRUE_COLOR_ALPHA` (RGB565 + 8-bit alpha, **3 bytes/pixel**) |
| Buffer size | 160 × 56 × 3 = **26 880 bytes** in internal SRAM (`malloc`) |
| Position | `LV_ALIGN_CENTER, x=0, y=+62` — centred between artist label (bottom ≈ +32) and buttons (top ≈ +92) |
| Background | Fully transparent — `memset(buf, 0, W*H*3)` sets alpha=0 on all pixels each frame |
| Border | `lv_obj_set_style_border_width(0)` — must be explicit or LVGL default draws a 1px border |

### Pixel buffer layout (`LV_IMG_CF_TRUE_COLOR_ALPHA`, RGB565 + alpha)

3 bytes per pixel:
```
px[0] = color.full & 0xFF   // RGB565 low byte
px[1] = color.full >> 8     // RGB565 high byte
px[2] = alpha               // 0 = transparent, 0xFF = opaque
```

Write a pixel:
```cpp
lv_color_t c = lv_color_make(r8, g8, b8);
uint8_t *px  = s_wave_buf + (y * W + x) * 3;
px[0] = (uint8_t)(c.full & 0xFF);
px[1] = (uint8_t)(c.full >> 8);
px[2] = 0xFF;
```

**Do not use `lv_canvas_get_buf()` — it does not exist in LVGL 8.** Use `s_wave_buf` directly.

### Renderer overview

Called from Core 1 every `MIC_BAR_MS` (80 ms). Full function in `main_screen_update_waveform()`.

**Per-band auto-gain normalization** (in `main_screen_update_waveform()`):
```cpp
// Each band tracks its own rolling peak — bass cannot crowd out mid/hmid/high.
static float pk_bass = 0.15f, pk_mid = 0.15f, pk_hmid = 0.15f, pk_high = 0.15f;
constexpr float DECAY = 0.982f, FLOOR = 0.15f;   // ~3 s half-life at 12.5 Hz

if (raw_bass > pk_bass) pk_bass = raw_bass;  pk_bass *= DECAY;  if (pk_bass < FLOOR) pk_bass = FLOOR;
// ... same for mid, hmid, high ...

float bass = raw_bass / pk_bass;   // normalized 0..1
```

**Per-column render** (for each x = 0..W-1):
1. `cx = x / (W-1)`, `win = sin(π·cx)` — Hann window, tapers brightness to 0 at canvas edges
2. Compute signed wave peak offset in pixels for each band: `p_X = amplitude * win * sin(freq * cx + phase) * mhh`
3. `mhh = cy - glow - 2.0f` — ensures peak + glow always fits inside canvas height

**Per-pixel glow** (for each y in column):
```cpp
float dy_f = (float)(cy - y);
float d = fabsf(dy_f - p_bass_f);
if (d < glow) {
    float t = (1.0f - d / glow) * win;   // linear falloff × Hann envelope
    g_bass = bass * t;                    // NOT t*t — linear keeps uniform colour across glow width
}
```

Additive RGB with neon palette — overlapping waves produce vivid mixed hues:

| Band | R | G | B | Colour |
|---|---|---|---|---|
| Bass | 20 | 120 | 255 | Electric blue |
| Mid | 0 | 255 | 220 | Cyan-teal |
| Hmid | 120 | 255 | 20 | Lime-green |
| High | 255 | 40 | 230 | Hot-pink |

Write threshold: `if (fr > 10.0f || fg > 10.0f || fb > 10.0f)` — clips outermost dim fringe to transparent.

### Current wave parameters

| Parameter | Value | Change in code |
|---|---|---|
| Glow radius | 5.5 px | `const float glow = 5.5f` |
| Max half-height | `cy - glow - 2` ≈ 20.5 px | derived — change `glow` only |
| Bass spatial freq | 1.5 cycles | `2.0f * PI * 1.5f * cx` |
| Mid spatial freq | 2.5 cycles | `2.0f * PI * 2.5f * cx` |
| Hmid spatial freq | 4.0 cycles | `2.0f * PI * 4.0f * cx` |
| High spatial freq | 6.5 cycles | `2.0f * PI * 6.5f * cx` |
| Bass phase speed | +0.07 rad/frame | `t_bass += 0.07f` |
| Mid phase speed | +0.11 rad/frame | `t_mid  += 0.11f` |
| Hmid phase speed | +0.16 rad/frame | `t_hmid += 0.16f` |
| High phase speed | +0.22 rad/frame | `t_high += 0.22f` |

### Visual clearance from the volume arc

Arc inner radius = 122 px. Canvas at y=+62, width ±80 px. At x=±80 from centre, arc is at y=±115 px — well outside the canvas vertical span (±28 px). No visual conflict.

### Layer position in LVGL draw order

```
... → progress_outline (hidden) → progress_bar (hidden) → waveform_canvas → playback buttons → ...
```

---

## 6. Threading & update loop

```
Core 0 — mic task (continuous, ~30 Hz)
    i2s_channel_read (blocks 32 ms)
    → DC block
    → IIR bandpass filters (bass, mid, hmid, high)
    → RMS per band → log-scale → smooth
    → g_mic_level, g_band_bass, g_band_mid, g_band_hmid, g_band_high

Core 1 — loop() (5 ms tick)
    Every MIC_BAR_MS (80 ms), gated on g_power_on:
        wave_hist ring buffer (populated with g_mic_level for legacy compat, ignored by renderer)
        call main_screen_update_waveform(ordered, MIC_N_BARS)
            → reads g_band_* globals directly
            → per-band auto-gain normalization
            → advances phase offsets
            → memset canvas to transparent
            → per-pixel glow-line render
            → lv_obj_invalidate()
```

The `levels` / `count` parameters passed to `main_screen_update_waveform` are ignored — the function reads `g_band_*` globals directly. The ring buffer in `main.cpp` currently has no effect on the visual output.

---

## 7. File map

| File | Role |
|---|---|
| `src/drivers/mic_pdm.h` | Public API: `mic_pdm_init()`, all 5 volatile globals |
| `src/drivers/mic_pdm.cpp` | I2S init, mic task: DC block + IIR filters + RMS + smoothing |
| `src/ui/main_screen.cpp` | `s_wave_canvas`, `s_wave_buf`, `main_screen_update_waveform()` — glow-line renderer |
| `src/ui/main_screen.h` | Declaration of `main_screen_update_waveform()` — signature unchanged |
| `src/main.cpp` | `initMic()` in `setup()`, waveform ring buffer + update call in `loop()` |
| `include/config.h` | `MIC_PDM_CLK_PIN`, `MIC_PDM_DATA_PIN`, `MIC_N_BARS`, `MIC_BAR_MS`, `WAVE_CANVAS_W`, `WAVE_CANVAS_H` |

---

## 8. Configuration reference (`include/config.h`)

```c
#define MIC_PDM_CLK_PIN    45    // GPIO — do not change (schematic-fixed)
#define MIC_PDM_DATA_PIN   46    // GPIO — do not change (schematic-fixed)
#define MIC_N_BARS         20    // kept for ring buffer compat, not used by renderer
#define MIC_BAR_MS         80    // ms between renderer calls (12.5 Hz animation rate)
#define WAVE_CANVAS_W      160   // canvas width  in px
#define WAVE_CANVAS_H      56    // canvas height in px (3 bytes/pixel = 26 880 bytes SRAM)
```

---

## 9. Tuning guide

### Per-band noise floors (`src/drivers/mic_pdm.cpp`)

The most important tuning. Each call `smooth(g_band_X, log_level(rms, FLOOR))` — adjust FLOOR:

| Symptom | Action |
|---|---|
| A band always lit even in silence | Raise its noise floor |
| A band never reacts to music | Lower its noise floor |
| All bands flat | Lower all floors; check DC blocking is still running |
| All bands maxed | DC blocking missing or floor too low |

### Auto-gain tuning (`src/ui/main_screen.cpp`)

| Symptom | Action |
|---|---|
| Waveform goes flat when music gets quiet | Lower `FLOOR` (currently 0.15) |
| Quiet room noise makes waves visible | Raise `FLOOR` |
| Peak takes too long to decay after loud passage | Lower `DECAY` (e.g. 0.97) |
| Waveform shrinks too quickly after a loud hit | Raise `DECAY` (e.g. 0.99) |
| One band always dominates visually | Check per-band peaks are independent (they are — do not switch back to global peak) |

### Wave visual tuning (`src/ui/main_screen.cpp`)

| Goal | Change |
|---|---|
| Thicker wave lines | Increase `glow` (e.g. 7.0) — `mhh` auto-adjusts |
| Thinner wave lines | Decrease `glow` (e.g. 4.0) |
| Dark border around waves | Do NOT use `t*t` falloff — keep linear `t` |
| Waves clip canvas top/bottom | `mhh` is `cy - glow - 2` so clipping is impossible; if it still clips, check `WAVE_CANVAS_H` is correct |
| Animation too fast/slow | Change phase deltas: `t_bass += 0.07f` etc. — larger = faster |
| Waves too similar looking | Change spatial frequencies: `1.5f`, `2.5f`, `4.0f`, `6.5f` cycles |
| Different colours | Change multipliers in `fr = g_bass * 20 + g_mid * 0 + ...` |
| Canvas too small vertically | Increase `WAVE_CANVAS_H` in `config.h` — buffer and canvas update automatically |
| Canvas overlaps arc or buttons | Adjust `y=62` in `lv_obj_align(s_wave_canvas, ...)` in `main_screen_create()` |
| Smoother animation | Decrease `MIC_BAR_MS` to 50 (20 Hz) in `config.h` |
| Breaks/gaps in wave lines | Do NOT gate on `|p| > 0.5` — use `win` multiplication instead |

### Software gain (if bands are still too quiet)

In `mic_pdm.cpp` inside the per-sample loop, multiply `ac` before the filter inputs:
```cpp
float ac = (buf[i] - dc) * 3.0f;   // 3× software gain
```
Do **not** use `amplify_num` — it does not exist on ESP32-S3.

---

## 10. Known pitfalls & decisions

### DC offset pins bars at max — hardware HP filter is a no-op on ESP32-S3

**This is the most important pitfall.** The `i2s_pdm_rx_slot_config_t` struct fields `hp_en`, `hp_cut_off_freq_hz`, and `amplify_num` are all marked `/* No effect, only for cpp compatibility */` in the IDF 5.5 header for ESP32-S3. They are gated by `#if SOC_I2S_SUPPORTS_PDM_RX_HP_FILTER`, which is not defined for this chip (only `SOC_I2S_SUPPORTS_PDM2PCM` is defined).

The consequence: the PDM2PCM output has a large DC offset that, if included in the RMS calculation, completely dominates the result. The bars sit at 80–90% in complete silence and show almost no response to actual audio. The fix is a software IIR DC blocking filter applied to every sample before squaring (see §4). **If this filter is accidentally removed, the entire visualiser breaks.**

### `amplify_num` causes a compile error on ESP32-S3

Because `SOC_I2S_SUPPORTS_PDM_RX_HP_FILTER` is not defined, the `amplify_num` field does not exist in the `i2s_pdm_rx_slot_config_t` struct at compile time. Setting `pdm_cfg.slot_cfg.amplify_num = N` will produce:
```
error: 'struct i2s_pdm_rx_slot_config_t' has no member named 'amplify_num'
```
For software gain, multiply the AC sample directly before squaring.

### `lv_canvas_get_buf()` does not exist in LVGL 8

Use `s_wave_buf` directly (the pointer allocated in `main_screen_create()` and passed to `lv_canvas_set_buffer()`). Do not attempt to retrieve it via LVGL APIs.

### Canvas format is `LV_IMG_CF_TRUE_COLOR_ALPHA` — 3 bytes/pixel, not 2

Buffer size = `W * H * 3`, not `W * H * sizeof(lv_color_t)`. The extra byte is the per-pixel alpha channel. Pixel layout: `[RGB565_low][RGB565_high][alpha]`. Set alpha=0xFF for opaque, 0x00 for transparent. `memset(buf, 0, W*H*3)` clears all pixels to fully transparent.

### Dark border around glow — caused by quadratic (`t*t`) falloff

Using `g_band = amplitude * t * t` (where `t` is the linear glow falloff) causes the outer pixels of the glow to have very low brightness (near-black but opaque), which creates a visible dark ring. The fix is linear falloff: `g_band = amplitude * t`. This keeps the full glow width uniformly coloured. Do not change back to `t*t`.

### Breaks/gaps in wave at zero-crossings — caused by `|p| > 0.5` gate

Gating rendering on `fabsf(p_X) > 0.5` was intended to prevent centre-row bleed at canvas edges, but it also silences the wave wherever the sine crosses zero (multiple times per canvas for high-frequency bands), creating visible breaks. The fix: remove the gate and multiply glow weight by `win` (Hann window). At canvas edges where `win → 0`, the glow naturally suppresses itself without a gate.

### Bass domination — use per-band auto-gain, not global

With a single global peak tracker, bass (which typically has the highest energy in music) always sets the scale, leaving mid/hmid/high at a fraction of the canvas. Per-band normalization (`pk_bass`, `pk_mid`, `pk_hmid`, `pk_high`) gives each wave independent visual weight. Do not collapse back to a single shared peak.

### Haptic motor causes waveform movement via structure-borne vibration

The DRV2605 haptic motor physically vibrates the PCB. The mic picks this up as structure-borne vibration. Encoder-turn haptic clicks fire at ~30 Hz (matching the mic task rate) and are most visible. This is expected behaviour.

### IDE false errors

The clang language server reports `'driver/i2s_pdm.h' file not found` and related unknown-type errors. These are **false positives** — the clang toolchain does not have the PlatformIO ESP32 SDK on its path. `pio run` compiles cleanly.

### i2s_mode_t naming conflict

Both the legacy `driver/deprecated/driver/i2s.h` and `ESP_I2S.h` define `i2s_mode_t` with different values. If you ever include both (directly or transitively) in the same translation unit, you get a redefinition error. Solution: use only the new IDF 5.x API and never include the legacy header.

### gpio_cfg anonymous union

`i2s_pdm_rx_gpio_config_t` has `union { gpio_num_t din; gpio_num_t dins[N]; }`. C++ does not allow designated initialisers on anonymous union members inside a nested struct literal. Always set `.gpio_cfg.clk` and `.gpio_cfg.din` via separate assignment statements **after** the struct literal.

### Progress bar vs waveform

The old `s_progress_bar` and `s_progress_outline` LVGL bar objects are still created in `main_screen_create()` but are **permanently hidden**. They can be safely deleted in a future refactor.

### Memory

The canvas buffer (26 880 bytes with H=56) lives in **internal SRAM** via `malloc()` — not PSRAM. LVGL accesses the canvas buffer frequently during rendering and PSRAM has higher latency. At boot the internal heap is ~150 KB free so this is easily accommodated.

---

## 11. Current state

Last flashed: 2026-02-18. Compiles cleanly. Visually confirmed working on device.

Confirmed working:
- 4 coloured sine waves visible and animated during music playback
- Waves react to quiet music (per-band auto-gain normalizes to current SPL)
- Waves stay mostly flat in silence (FLOOR=0.15 prevents noise amplification)
- No black border around waves (linear falloff)
- No breaks/gaps in wave lines (Hann-window suppression, no gate)
- No canvas clipping (mhh = cy − glow − 2 guarantees fit)
- Transparent background (album art shows through)
- Correct position between artist label and playback buttons

*Document created: 2026-02-18 — Updated: 2026-02-18*
