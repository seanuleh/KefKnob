# Microphone & Waveform Visualiser — Agent Context Document

> **Purpose:** Everything a future agent needs to work on the microphone or waveform feature without doing additional research. Covers hardware, firmware, IDF API version, signal processing, LVGL canvas, threading, and known pitfalls.

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

#### `I2S_CHANNEL_DEFAULT_CONFIG` expands to

```c
{
    .id              = I2S_NUM_0,
    .role            = I2S_ROLE_MASTER,
    .dma_desc_num    = 6,
    .dma_frame_num   = 240,
    .auto_clear      = false,
    .auto_clear_after_cb   = false,
    .auto_clear_before_cb  = false,   // TX-only fields, ignored for RX
}
```

Do not set `auto_clear_before_cb` explicitly for an RX channel — it is irrelevant and just adds noise.

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

### DC blocking (software high-pass filter)

**This step is essential.** The hardware HP filter (`hp_en`) is a no-op on ESP32-S3, so the raw PCM output has a large DC offset. Without removal, this DC component dominates the RMS calculation and pins the bars at 80–90% of maximum even in complete silence. Music from speakers adds only a small AC signal on top, so the waveform shows no meaningful response to audio.

```cpp
static float dc = 0.0f;   // persists across blocks

for (int i = 0; i < n; i++) {
    dc = dc * 0.995f + buf[i] * 0.005f;   // IIR low-pass tracks the mean
    float ac = buf[i] - dc;               // AC-only signal
    sum_sq += (int64_t)(ac * ac);
}
```

The IIR filter has α = 0.995, giving a time constant of ~200 samples (~12.5 ms at 16 kHz) and a corner frequency of ~12 Hz. All audio content passes through; only the DC component is removed.

### RMS computation (on AC signal)

```cpp
int64_t sum_sq = 0;
for (int i = 0; i < n; i++) {
    dc = dc * 0.995f + buf[i] * 0.005f;
    float ac = buf[i] - dc;
    sum_sq += (int64_t)(ac * ac);
}
float rms = sqrtf((float)sum_sq / n);
```

Using `int64_t` accumulator and `float` for the AC sample prevents overflow. The DC-removed RMS represents only actual sound pressure variation.

### Log-scale mapping (rms → 0–255)

```cpp
int level = 0;
if (rms > 7.0f) {
    level = (int)(logf(rms / 7.0f) / logf(100.0f) * 255.0f);
    level = max(0, min(255, level));
}
```

| AC rms LSB (after DC removal) | Level | Approximate source |
|---|---|---|
| < 7 | 0 | Silence / mic thermal noise |
| ~7 | ~0 | Noise floor threshold |
| ~70 | ~128 | Music from speakers 1–2 m away |
| ~700 | ~255 | Loud music / shouting at close range |

The noise floor (`7.0f`) and log base (`100.0f`) are the primary tuning knobs. After any change to the DC blocking filter, re-tune the floor from scratch — the effective noise floor changes significantly with DC removed.

### Asymmetric smoothing

```cpp
uint8_t prev = g_mic_level;
if ((uint8_t)level >= prev) {
    g_mic_level = (uint8_t)((level * 3 + prev + 1) / 4);   // α = 0.75 — fast attack
} else {
    g_mic_level = (uint8_t)((level + prev * 3 + 1) / 4);   // α = 0.25 — slow decay
}
```

Fast attack (transients snap up immediately), slow decay (bars hang visibly after a beat). Adjust the multipliers to taste. The `+ 1` avoids floor bias in integer division.

### Shared state

```cpp
// mic_pdm.h / mic_pdm.cpp
volatile uint8_t g_mic_level;   // 0–255, written at ~30 Hz on Core 0
```

`volatile uint8_t` is sufficient — single-byte reads/writes are atomic on Xtensa LX7. No mutex is needed. Core 1 reads this in `loop()` every `MIC_BAR_MS` ms.

---

## 5. LVGL waveform canvas

### Widget properties

| Property | Value |
|---|---|
| Widget | `lv_canvas_t` |
| Buffer format | `LV_IMG_CF_TRUE_COLOR` (RGB565, 2 bytes/pixel) |
| Buffer size | 160 × 30 × 2 = **9 600 bytes** in internal SRAM (`malloc`) |
| Position | `LV_ALIGN_CENTER, x=0, y=+75` from screen centre (screen y = 255) |
| Visible span | y = 240 – 270 (screen coords), x = 100 – 260 |
| Background OPA | `LV_OPA_TRANSP` on the widget; canvas pixels filled `#000000` |

### Bar geometry

```
WAVE_CANVAS_W = 160 px
WAVE_CANVAS_H = 30  px
MIC_N_BARS    = 20  bars

bar_w  = 5 px
gap    = 3 px
step   = 8 px (bar_w + gap)

total bar span = 20 × 8 − 3 = 157 px  (last gap omitted)
left margin    = (160 − 157) / 2 = 1 px
```

Bars grow **symmetrically from the canvas vertical centre** (y = 15):
- Silence → bar_h = 2 px (1 px above and below centre)
- Maximum → bar_h = 26 px (13 px above and below centre, 2 px margin)

### Draw call per frame

```cpp
lv_canvas_fill_bg(s_wave_canvas, lv_color_hex(0x000000), LV_OPA_COVER);

lv_draw_rect_dsc_t dsc;
lv_draw_rect_dsc_init(&dsc);
dsc.bg_color     = lv_color_hex(0x00BFFF);   // same blue as volume arc
dsc.bg_opa       = LV_OPA_COVER;
dsc.radius       = 2;
dsc.border_width = 0;
dsc.shadow_width = 0;

for (int i = 0; i < count; i++) {
    int hh    = 1 + ((int)levels[i] * 12) / 255;   // 1..13 half-height
    int bar_h = hh * 2;
    int x     = 1 + i * 8;
    int y     = 15 - hh;
    lv_canvas_draw_rect(s_wave_canvas, x, y, 5, bar_h, &dsc);
}

lv_obj_invalidate(s_wave_canvas);
```

### Layer position

The canvas sits directly above the hidden progress bar objects in LVGL's draw order:

```
... → progress_outline (hidden) → progress_bar (hidden) → waveform_canvas → playback buttons → ...
```

### Visual clearance from the volume arc

The volume arc is 280 px diameter centred at (180, 180). Its endpoints are at y = +70 from screen centre (screen y = 250). The waveform canvas top is at screen y = 240 — 10 px above the arc endpoints.

At canvas top y = 240 (y_offset = −40 from screen centre), the arc's **inner** boundary is at:
```
x = ±sqrt(122² − 40²) = ±sqrt(14884 − 1600) = ±sqrt(13284) ≈ ±115 px from centre
```
The canvas extends only ±80 px from centre, so it is entirely inside the arc at every row. No visual clipping occurs.

---

## 6. Threading & update loop

```
Core 0 — mic task (continuous)
    i2s_channel_read (blocks 32 ms)
    → DC block → RMS → log-scale → smooth
    → g_mic_level (volatile uint8_t write)

Core 1 — loop() (5 ms tick via delay(5))
    Every MIC_BAR_MS (80 ms):
        read g_mic_level
        push to wave_hist[MIC_N_BARS] ring buffer
        build ordered[] array (oldest → newest)
        call main_screen_update_waveform(ordered, MIC_N_BARS)
```

### Ring buffer (Core 1 only — no sync needed)

```cpp
static uint8_t  wave_hist[MIC_N_BARS] = {};
static int      wave_head             = 0;   // index of next write slot

wave_hist[wave_head] = g_mic_level;
wave_head = (wave_head + 1) % MIC_N_BARS;

// Linearise: oldest = wave_head (slot just overwritten), newest = wave_head - 1
uint8_t ordered[MIC_N_BARS];
for (int i = 0; i < MIC_N_BARS; i++) {
    ordered[i] = wave_hist[(wave_head + i) % MIC_N_BARS];
}
```

The waveform update is **gated on `g_power_on`** — it pauses when the speaker is in standby (no point animating a hidden canvas).

---

## 7. File map

| File | Role |
|---|---|
| `src/drivers/mic_pdm.h` | Public API: `mic_pdm_init()`, `g_mic_level` extern |
| `src/drivers/mic_pdm.cpp` | I2S init, mic task, DC blocking, RMS + smoothing |
| `src/ui/main_screen.cpp` | `s_wave_canvas`, `s_wave_buf`, `main_screen_update_waveform()` |
| `src/ui/main_screen.h` | Declaration of `main_screen_update_waveform()` |
| `src/main.cpp` | `initMic()` call in `setup()`, waveform update loop in `loop()` |
| `include/config.h` | `MIC_PDM_CLK_PIN`, `MIC_PDM_DATA_PIN`, `MIC_N_BARS`, `MIC_BAR_MS`, `WAVE_CANVAS_W`, `WAVE_CANVAS_H` |

---

## 8. Configuration reference (`include/config.h`)

```c
#define MIC_PDM_CLK_PIN    45    // GPIO — do not change (schematic-fixed)
#define MIC_PDM_DATA_PIN   46    // GPIO — do not change (schematic-fixed)
#define MIC_N_BARS         20    // number of waveform bars
#define MIC_BAR_MS         80    // ms between bar advances (window = N×BAR_MS = 1.6 s)
#define WAVE_CANVAS_W      160   // canvas width  in px
#define WAVE_CANVAS_H      30    // canvas height in px
```

---

## 9. Tuning guide

| Goal | Change |
|---|---|
| Bars too flat (music not registering) | Lower noise floor: `7.0f` → `4.0f` in `mic_pdm.cpp` |
| Bars active during silence / noise floor visible | Raise noise floor: `7.0f` → `10.0f` |
| Bars pinned at max constantly | DC offset not removed — ensure DC blocking IIR filter is in place before RMS |
| Boost sensitivity without touching floor | Add software gain: multiply `ac` by a factor (e.g. `ac *= 3.0f`) before squaring. **Do not use `amplify_num` — it is a no-op on ESP32-S3** |
| Faster visual response | Increase attack alpha: `level * 3 + prev` → `level * 7 + prev` (then divide by 8) |
| Smoother/slower bars | Increase `MIC_BAR_MS` (e.g. 120) in `config.h` |
| Wider time window | Increase `MIC_N_BARS` — also increase `WAVE_CANVAS_W` and adjust bar/gap math |
| Taller bars | Increase `WAVE_CANVAS_H` and adjust `max_hh` formula in `main_screen_update_waveform()` |
| Different bar colour | Change `lv_color_hex(0x00BFFF)` in `main_screen_update_waveform()` |

---

## 10. Known pitfalls & decisions

### DC offset pins bars at max — hardware HP filter is a no-op on ESP32-S3

**This is the most important pitfall.** The `i2s_pdm_rx_slot_config_t` struct fields `hp_en`, `hp_cut_off_freq_hz`, and `amplify_num` are all marked `/* No effect, only for cpp compatibility */` in the IDF 5.5 header for ESP32-S3. They are gated by `#if SOC_I2S_SUPPORTS_PDM_RX_HP_FILTER`, which is not defined for this chip (only `SOC_I2S_SUPPORTS_PDM2PCM` is defined).

The consequence: the PDM2PCM output has a large DC offset that, if included in the RMS calculation, completely dominates the result. The bars sit at 80–90% in complete silence and show almost no response to actual audio. The fix is a software IIR DC blocking filter applied to every sample before squaring (see §4).

### `amplify_num` causes a compile error on ESP32-S3

Because `SOC_I2S_SUPPORTS_PDM_RX_HP_FILTER` is not defined, the `amplify_num` field does not exist in the `i2s_pdm_rx_slot_config_t` struct at compile time. Setting `pdm_cfg.slot_cfg.amplify_num = N` will produce:
```
error: 'struct i2s_pdm_rx_slot_config_t' has no member named 'amplify_num'
```
For software gain, multiply the AC sample directly before squaring.

### Haptic motor causes waveform movement via structure-borne vibration

The DRV2605 haptic motor physically vibrates the PCB. The mic, being soldered to the same PCB, picks this up as structure-borne vibration rather than airborne sound. This produces waveform movement that is unrelated to audio. The effect is most visible with encoder-turn haptic clicks (which fire at ~30 Hz, matching the mic task rate). Play/pause haptic (STRONG) fires less frequently and may appear not to trigger movement. This is expected behaviour — the waveform is not purely an audio visualiser.

### IDE false errors
The clang language server (VS Code IntelliSense) reports `'driver/i2s_pdm.h' file not found` and related unknown-type errors. These are **false positives** — the clang toolchain does not have the PlatformIO ESP32 SDK on its path. `pio run` compiles cleanly.

### i2s_mode_t naming conflict
Both the legacy `driver/deprecated/driver/i2s.h` and `ESP_I2S.h` define `i2s_mode_t` with different values. If you ever include both (directly or transitively) in the same translation unit, you get a redefinition error. Solution: use only the new IDF 5.x API and never include the legacy header.

### Old Waveshare demo uses different IDF
`ESP-IDF/07_Audio_Test/main/audio_bsp.h` uses the **new ESP-IDF 5.x channel API** internally (despite the project folder name suggesting IDF), confirming that the same new-API approach is correct for this board. The demo uses 44100 Hz sample rate; DeskKnob uses 16000 Hz (sufficient for amplitude visualisation, lower CPU load).

### gpio_cfg anonymous union
`i2s_pdm_rx_gpio_config_t` has `union { gpio_num_t din; gpio_num_t dins[N]; }`. C++ does not allow designated initialisers on anonymous union members inside a nested struct literal. Always set `.gpio_cfg.clk` and `.gpio_cfg.din` via separate assignment statements **after** the struct literal.

### Progress bar vs waveform
The old `s_progress_bar` and `s_progress_outline` LVGL bar objects are still created in `main_screen_create()` but are **permanently hidden** immediately after creation. They are not updated anywhere. They can be safely deleted in a future refactor — their variables and creation code in `main_screen.cpp` serve no runtime purpose.

### Waveform is always active
The mic runs continuously regardless of source (WiFi, USB+Spotify, USB+mute). The waveform canvas visibility is managed purely by `g_power_on` in `loop()` and the initial `LV_OBJ_FLAG_HIDDEN` cleared on first `main_screen_update()` call.

### Memory
The canvas buffer (9 600 bytes) lives in **internal SRAM** via `malloc()` — not PSRAM. This is deliberate: LVGL accesses the canvas buffer frequently during rendering and PSRAM has higher latency. At boot the internal heap is ~168 KB free so this is trivially accommodated.

---

## 11. Verified working state

Confirmed via serial boot log after flashing:
```
[Mic] PDM mic ready on GPIO 45 (CLK) / 46 (DATA)
```
No errors. All other subsystems unaffected. Heap usage nominal.

Behaviour confirmed:
- Bars flat in silence (DC blocking removes offset)
- Bars react to shouting at close range (full deflection)
- Bars react to music from speakers at 1–2 m (moderate deflection)
- Bars react to structure-borne haptic vibration (encoder turns) — expected, not a bug

*Document created: 2026-02-18 — Updated: 2026-02-18*
