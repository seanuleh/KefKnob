# KefKnob - Agent Instructions

> **Agent rule:** After completing any task that changes behaviour, adds/removes features, modifies the API usage, or changes shared state, update the relevant sections of this file before finishing. Keep the File Map, Architecture, Module API, UI Overview, and Debugging Checklist accurate. Update the "Last updated" line and the working-features list at the bottom.

KEF LSX II speaker controller running on a Waveshare ESP32-S3 1.8" Touch LCD.
**Current status: fully working.** Display renders, touch works, encoder works, WiFi connects, KEF volume/track/source/power control works, album art displays, standby screen shows when speaker is off, Spotify API integration on USB source works (now-playing metadata + playback control + progress bar). OTA firmware updates via `deskknob.local`.

---

## File Map — go here first

| What you want to change | File |
|---|---|
| Pin assignments, timeouts, task sizes, volume step, swipe threshold, OTA hostname/password, haptic effect IDs, mic pins, waveform bar count/timing | `include/config.h` |
| WiFi credentials, speaker IP, Spotify credentials (gitignored) | `include/config_local.h` |
| LVGL settings (color depth, fonts, tick) | `include/lv_conf.h` |
| Main screen UI layout, widget updates, button callbacks | `src/ui/main_screen.cpp` / `.h` |
| KEF HTTP API calls | `src/network/kef_api.cpp` / `.h` |
| Spotify Web API (now-playing + playback control) | `src/network/spotify_api.cpp` / `.h` |
| App wiring: encoder, touch, tasks, LVGL init, networkTask | `src/main.cpp` |
| Display hardware init (SH8601 QSPI) | `src/drivers/display_sh8601.cpp` / `.h` |
| Touch hardware init (CST816S I2C) | `src/drivers/touch_cst816.cpp` / `.h` |
| Encoder hardware init (iot_knob) — **C file, not C++** | `src/drivers/encoder.c` / `.h` |
| DRV2605 haptic driver — **C file, not C++** | `src/drivers/drv2605.c` / `.h` |
| PDM MEMS microphone (MSM261D4030H1CPM) I2S driver + mic task | `src/drivers/mic_pdm.cpp` / `.h` |
| SH8601 low-level LCD driver | `src/drivers/esp_lcd_sh8601.c` / `.h` |
| MQTT PubSubClient wrapper (Zigbee2MQTT light control) | `src/network/mqtt_client.cpp` / `.h` |
| Hue light control screen (LVGL) | `src/ui/light_screen.cpp` / `.h` |
| Library dependencies, board config, partition table | `platformio.ini` |
| Flash partition layout (OTA slots) | `partitions_ota.csv` |

---

## Module API Quick Reference

Saves reading headers. All functions are in the files listed in the File Map.

### `src/ui/main_screen.h` — Core 1 only

```cpp
void main_screen_create();
// Call once after LVGL init. Builds all widgets and loads the screen.

void main_screen_update(int volume, const char *title, const char *artist,
                        bool is_playing, bool source_is_usb, bool is_muted,
                        bool spotify_active, int progress_pct);
// Full UI redraw. Called every 1s from loop() on state change, or
// immediately on encoder turn (volume arc only updates in that case).

void main_screen_update_art(const uint8_t *jpeg_buf, size_t jpeg_size);
// Decode JPEG → album art canvas. Pass nullptr/0 to clear to black.

void main_screen_update_power_source(bool power_on, bool source_is_usb);
// Updates control panel button highlights + shows/hides standby screen.

void main_screen_toggle_control_panel();
bool main_screen_is_control_panel_visible();
bool main_screen_is_standby_visible();

void main_screen_update_waveform(const uint8_t *levels, int count);
// Redraws the 160×30 px waveform canvas. levels[0..count-1] are amplitudes
// 0-255, oldest first. Bars grow symmetrically from the canvas centre line.
// Call from Core 1 every MIC_BAR_MS ms (driven by loop()).

const char *main_screen_take_control_cmd();
// Returns one of: "power" "src_wifi" "src_usb" "pwr_wifi" "pwr_usb"
// Returns nullptr if no command pending. Clears the pending command.

const char *main_screen_take_track_cmd();
// Returns one of: "pause" "next" "previous" "mute" "unmute"
// Returns nullptr if no command pending. Clears the pending command.
```

### `src/drivers/drv2605.h` — Core 1 only (I2C_NUM_0 shared with touch)

```c
bool drv2605_init(void);
// Call once after Touch_Init(). Returns false if chip not found — all play() calls become no-ops.

bool drv2605_play(uint8_t effect_id);
// Trigger ERM Library A effect by ID (1-123). Returns false if busy or not init'd.
// Skips silently if GO bit still set from previous effect.

bool drv2605_is_playing(void);
// True while GO bit is set.
```

Effect IDs and per-event mapping defined in `include/config.h` (`HAPTIC_EFFECT_*`, `HAPTIC_*`).
Drive voltage maxed at init (`RATED_V=0xFF`, `OD_CLAMP=0xFF`). Scale back if motor overheats.

### `src/network/kef_api.h` — Core 0 (network task) only

```cpp
bool kef_get_volume(int *out_volume);
bool kef_set_volume(int volume);                  // 0-100

bool kef_get_player_data(char *title,  size_t title_len,
                         char *artist, size_t artist_len,
                         bool     *out_playing,
                         bool     *out_is_standby,
                         char     *cover_url, size_t cover_url_len,
                         uint32_t *out_position_ms,
                         uint32_t *out_duration_ms);

bool kef_track_control(const char *cmd);          // "pause" "next" "previous"
bool kef_get_source(char *source, size_t len);    // "wifi" "usb" "bluetooth" ...
bool kef_set_source(const char *source);          // same values + "powerOn" "standby"
bool kef_set_power(bool on);                      // wraps kef_set_source("powerOn"/"standby")
bool kef_get_speaker_status(bool *out_is_on);     // use this, NOT player state, for standby detection
bool kef_set_mute(bool muted);

uint8_t *kef_fetch_jpeg(const char *url, size_t *out_size);
// Fetches HTTPS image into PSRAM. Caller must free(). Returns nullptr on error.

// DEAD — do not call: kef_get_power() (HTTP 500), kef_power_on() (superseded by kef_set_power)
```

### `src/drivers/mic_pdm.h` — mic task on Core 0, read on Core 1

```cpp
bool mic_pdm_init(int clk_pin, int data_pin);
// Call once in setup() before initLVGL(). Opens I2S0 in PDM RX mode at 16 kHz
// mono 16-bit using the hardware PDM2PCM filter (SOC_I2S_SUPPORTS_PDM2PCM=1 on
// ESP32-S3). Spawns a FreeRTOS task on Core 0 (priority 4) that reads 512-sample
// blocks, computes RMS, log-scales to 0-255, and smooths with fast attack /
// slow decay. Returns false if I2S driver install fails.

extern volatile uint8_t g_mic_level;
// Smoothed amplitude 0-255. Written by mic task (~30 Hz, Core 0).
// Single-byte volatile read from Core 1 is atomic on ESP32 — no mutex needed.
```

Microphone hardware: **MSM261D4030H1CPM** PDM MEMS mic.
Pins: GPIO 45 (CLK / PDM SCK), GPIO 46 (DATA / PDM DIN). L/R tied to GND (left channel).

### `src/network/mqtt_client.h` — Core 0 (network task) only

```cpp
// Volatile light state — written by Core 0 MQTT callback, read by Core 1
extern volatile bool  g_light_on;
extern volatile int   g_light_brightness;   // 0–254
extern volatile int   g_light_colortemp;    // Mired 153–500
extern volatile float g_light_color_hue;    // 0–360
extern volatile float g_light_color_sat;    // 0–100
extern volatile bool  g_light_state_dirty;  // Core 0 → Core 1 paint signal

void mqtt_client_begin(const char *broker_ip, int port);
// Connect to broker, subscribe to MQTT_LIGHT_TOPIC.
// No-op if MQTT_BROKER_IP is empty.

void mqtt_client_loop();
// Process incoming messages, reconnect every 5s if disconnected.
// Call every networkTask iteration.

bool mqtt_light_publish(const char *json_payload);
// Publish to MQTT_LIGHT_SET_TOPIC.  Returns false if disconnected.
```

Broker IP / port: `MQTT_BROKER_IP` / `MQTT_BROKER_PORT` in `include/config.h` (set in `config_local.h`).
Topic constants: `MQTT_LIGHT_TOPIC` (subscribe), `MQTT_LIGHT_SET_TOPIC` (publish).

### `src/ui/light_screen.h` — Core 1 only

```cpp
lv_obj_t  *light_screen_get_obj();
void        light_screen_create();
void        light_screen_update(bool on, int brightness, int colortemp,
                                float hue, float sat);
const char *light_screen_take_cmd();        // JSON payload for MQTT /set, clears on read
int         light_screen_get_encoder_mode();// LIGHT_ENC_NONE / BRIGHTNESS / COLORTEMP
bool        light_screen_is_colorpicker_open();
```

Encoder mode constants (defined in `light_screen.h`):
- `LIGHT_ENC_NONE` — no mode active
- `LIGHT_ENC_BRIGHTNESS` — encoder adjusts brightness
- `LIGHT_ENC_COLORTEMP` — encoder adjusts colour temperature

### `src/network/spotify_api.h` — Core 0 (network task) only

```cpp
void spotify_init(const char *client_id, const char *client_secret,
                  const char *refresh_token);
// Call once in networkTask() before polling. No-op if credentials are empty.

bool spotify_get_now_playing(char *title,     size_t title_len,
                              char *artist,    size_t artist_len,
                              char *cover_url, size_t cover_url_len,
                              bool     *out_playing,
                              bool     *out_nothing,    // true = HTTP 204, clear display
                              uint32_t *out_progress_ms,
                              uint32_t *out_duration_ms);
// Returns true = valid data. false = network error (keep old state) OR nothing playing.
// Check *out_nothing to distinguish. HTTP 401 triggers auto token refresh.

bool spotify_play();
bool spotify_pause();
bool spotify_next();
bool spotify_previous();
// All require Spotify Premium + user-modify-playback-state scope.
```

---

## Architecture

```
Core 0 — networkTask() + mic task     Core 1 — loop() / Arduino main
──────────────────────────────────    ────────────────────────────────────
mic task (separate, priority 4):      Swipe left/right → lv_scr_load_anim()
                                        KEF ↔ light screen (g_active_screen)
                                      Encoder: mode-aware (g_encoder_mode)
mic task (separate, priority 4):      ArduinoOTA.handle()
  Reads I2S PDM ~30 Hz               lv_timer_handler() every 5ms
  Updates g_mic_level (volatile u8)
                                      Waveform update every MIC_BAR_MS (80ms):
networkTask() — Loops every 50ms        reads g_mic_level, pushes to ring buf,
  ├─ If g_volume_dirty AND               calls main_screen_update_waveform()
  │    250ms since last send:         ArduinoOTA.handle()
  │    kef_set_volume()               lv_timer_handler() every 5ms
  │    250ms since last send:           Reads g_volume_target → immediate
  │    kef_set_volume()                   arc redraw (no waiting)
  ├─ If g_track_cmd set:              Album art decode (g_art_dirty):
  │    USB+Spotify active:               snapshot jpeg ptr, clear pipeline slot,
  │      spotify_play/pause/next/prev()   decode via TJpgDec, blit to canvas
  │    WiFi:                          Forward button commands:
  │      kef_track_control()            main_screen_take_control_cmd() → g_control_cmd
  ├─ If g_control_cmd set:             main_screen_take_track_cmd()    → g_track_cmd
  │    kef_set_source() / kef_set_power() → sets g_haptic_event (STRONG/MEDIUM)
  │    last_poll_ms = 0 (immediate re-poll) On g_state_dirty: snapshot state
  └─ Every 1s:                          under mutex → main_screen_update()
       kef_get_speaker_status()                       main_screen_update_power_source()
       if USB source:                 Service g_haptic_event → drv2605_play()
         spotify_get_now_playing()     (Core 1 only — I2C_NUM_0 shared with touch)
       else:                          Encoder callbacks set g_haptic_event = CLICK
         kef_get_player_data()
       kef_get_source()
       kef_get_volume() (skipped 3s after a set)
       g_state_dirty = true
```

**Thread safety rules:**
- All LVGL calls must happen on Core 1 (in `loop()` or encoder/touch callbacks, which fire on Core 1)
- `g_title`, `g_artist`, `g_is_playing`, `g_volume` are protected by `g_state_mutex`
- `g_volume_target`, `g_volume_dirty`, `g_track_cmd`, `g_control_cmd` are `volatile`, written by Core 1 callbacks, read/cleared by Core 0 — no mutex needed (single writer, single reader)

### Shared state globals (all in `src/main.cpp`)

```cpp
// Haptic event — written by Core 1 encoder callbacks and loop(), serviced in loop()
static volatile uint8_t g_haptic_event; // HAPTIC_NONE/CLICK/STRONG/MEDIUM (config.h)

// Protected by g_state_mutex — read on Core 1, written on Core 0
static int  g_volume;           // last confirmed volume from KEF
static char g_title[128];       // current track title
static char g_artist[128];      // current track artist
static bool g_is_playing;       // playback state
static bool g_state_dirty;      // true = Core 1 should redraw

// Volatile — written by Core 1 input callbacks, consumed by Core 0
static volatile int      g_volume_target; // -1 = none pending
static volatile bool     g_volume_dirty;  // send volume to KEF
static volatile char     g_track_cmd[12]; // "pause"/"next"/"previous"/""
static volatile char     g_control_cmd[16]; // "power"/"src_wifi"/"src_usb"/"pwr_wifi"/"pwr_usb"

// Written by Core 0, read by Core 1
static volatile bool g_power_on;
static volatile bool g_source_is_usb;
static volatile bool g_is_muted;
static volatile bool g_spotify_active;  // true = Spotify session detected on USB
static volatile int  g_progress_pct;    // track progress 0-100

// Light state — written by Core 0 (MQTT callback), read by Core 1
// Defined in mqtt_client.cpp, declared extern in mqtt_client.h
volatile bool  g_light_on;
volatile int   g_light_brightness;
volatile int   g_light_colortemp;
volatile float g_light_color_hue;
volatile float g_light_color_sat;
volatile bool  g_light_state_dirty;

// Light encoder targets — Core 1 → Core 0
static volatile int  g_light_brightness_target;  // -1 = none pending
static volatile int  g_light_colortemp_target;
static volatile char g_light_cmd[192];            // JSON payload, Core 1 → Core 0

// Screen / encoder mode — Core 1 only (same core, no mutex)
static volatile int  g_encoder_mode;   // ENCODER_MODE_KEF_VOLUME/LIGHT_BRIGHT/LIGHT_COLORTEMP
static volatile int  g_active_screen;  // SCREEN_KEF / SCREEN_LIGHT

// Album art pipeline (lock-free single-producer/single-consumer)
static volatile uint8_t *g_art_jpeg_buf;  // Core 0 writes when nullptr; Core 1 clears to nullptr after decode
static volatile size_t   g_art_jpeg_size;
static volatile bool     g_art_dirty;     // Core 0 → Core 1 signal
```

---

## Adding a New Feature

### Add a new KEF API call
1. Add declaration to `src/network/kef_api.h`
2. Implement in `src/network/kef_api.cpp` — use the existing `http_get()` helper
3. Call from `networkTask()` in `src/main.cpp`

### Add a new UI element
1. Declare widget pointer in `src/ui/main_screen.cpp` (file-local static)
2. Create in `main_screen_create()` with `lv_*_create(s_screen, ...)`
3. Update in `main_screen_update()` — called from Core 1 on every state change

### Add a new input gesture
- Touch: modify `lvgl_touch_read()` in `src/main.cpp` — `touch_start_x/y` and `touch_last_x/y` are tracked
- Horizontal swipe is **reserved for future multi-screen navigation** — skeleton is in `lvgl_touch_read()`; do not assign track commands here
- Encoder button: `ENCODER_BTN` pin is defined in `config.h` (currently -1, not wired)
- To queue a track command from a button: write to `s_pending_track_cmd` in `main_screen.cpp`; `loop()` polls `main_screen_take_track_cmd()` and forwards to `g_track_cmd`
- To queue a power/source command: write to `s_pending_cmd` in `main_screen.cpp` and expose via `main_screen_take_control_cmd()`

### Add a new polled value
In `networkTask()`, inside the `if (now - last_poll_ms >= KEF_STATE_POLL_INTERVAL)` block:
1. Call your KEF API function
2. Take `g_state_mutex`, update state, release
3. `g_state_dirty = true` is already set at the end of that block

---

## KEF API Reference

Base URL: `http://<KEF_SPEAKER_IP>/api` — defined in `include/config.h`

All calls are HTTP GET. URL-encode colons as `%3A`, slashes as `%2F`, `{` as `%7B`, `"` as `%22`, `}` as `%7D`.

### Working endpoints (confirmed on LSX II firmware)

| Function | Path | Roles | Value | Response field |
|---|---|---|---|---|
| Get volume | `player:volume` | `value` | — | `[0]["i32_"]` (int) |
| Set volume | `player:volume` | `value` | `{"type":"i32_","i32_":N}` | — |
| Get player data | `player:player/data` | `value` | — | see below |
| Track control | `player:player/control` | `activate` | `{"control":"CMD"}` | — |
| Get source | `settings:/kef/play/physicalSource` | `value` | — | `[0]["kefPhysicalSource"]` (string) |
| Set source / power | `settings:/kef/play/physicalSource` | `value` | `{"type":"kefPhysicalSource","kefPhysicalSource":"SRC"}` | — |
| Get speaker status | `settings:/kef/host/speakerStatus` | `value` | — | `[0]["kefSpeakerStatus"]` (string) |
| Set mute | `settings:/mediaPlayer/mute` | `value` | `{"type":"bool_","bool_":true}` | — |

**Track control CMD values:** `"pause"`, `"next"`, `"previous"`

**Source / power values for `kefPhysicalSource`:**
- `"wifi"` — WiFi streaming input
- `"usb"` — USB audio input
- `"bluetooth"` — Bluetooth input
- `"optic"` — optical input
- `"coaxial"` — coaxial input
- `"analog"` — analog input
- `"powerOn"` — wake from standby without changing source
- `"standby"` — send to standby

**Player data response fields** (from `player:player/data`, array index 0):
```
["state"]                                          → "playing" | "pause" | "stopped"
["trackRoles"]["title"]                            → track title string
["trackRoles"]["mediaData"]["metaData"]["artist"]  → artist string
["trackRoles"]["icon"]                             → album art HTTPS URL (Spotify CDN)
["status"]["duration"]                             → track duration in milliseconds (int)
```

Note: **position is not in the API response**. Track progress on WiFi source is estimated client-side: the position counter advances while `state == "playing"`, freezes while paused, and resets to 0 on track change (detected via cover URL change). Seeking on the phone will cause the bar to drift until the next track.

**Critical: ALL `physicalSource` commands use `roles=value`**, not `roles=activate`. `roles=activate` returns HTTP 500 on this firmware. Track control (`player:player/control`) is the only command that uses `roles=activate`.

**Speaker status values** (`kefSpeakerStatus`):
- `"powerOn"` — speaker is physically on (playing, paused, or idle)
- anything else — speaker is in standby

**Player state values** (`state` field):
- `"playing"` — actively playing
- `"pause"` — paused
- `"stopped"` — idle/stopped (speaker ON but not playing) **or** in standby
- **Do not use player state for standby detection** — `"stopped"` is returned for both on-but-idle and off. Use `kef_get_speaker_status()` instead.

**Known KEF API behaviour:**
- Rate limiting: volume commands faster than ~250ms apart cause the API to return 0 → use `VOLUME_DEBOUNCE_MS` (250ms)
- Stale reads: `getData` immediately after `setData` returns old value → skip volume poll for 3s after a set
- Setting `physicalSource` wakes from standby AND switches source in one call — no separate wake step needed

---

## Spotify API Reference

Used only when source is USB (`g_source_is_usb == true`). Credentials in `include/config_local.h`.

**Required scopes:** `user-read-currently-playing user-modify-playback-state`

**Auth flow:** Authorization Code with refresh token. One-time setup via Python script embedded in `config_local.h` comments. The access token is refreshed automatically 5 minutes before expiry.

| Function | Method | Endpoint |
|---|---|---|
| Now playing (metadata) | GET | `api.spotify.com/v1/me/player/currently-playing` |
| Play | PUT | `api.spotify.com/v1/me/player/play` |
| Pause | PUT | `api.spotify.com/v1/me/player/pause` |
| Next | POST | `api.spotify.com/v1/me/player/next` |
| Previous | POST | `api.spotify.com/v1/me/player/previous` |

**HTTP 204** from now-playing = nothing playing → `g_spotify_active = false`, clear display.
**HTTP 401** = token expired → force refresh, return false (retry on next 1s poll).
**Playback control requires Spotify Premium.**

**Now-playing fields used:**
```
["is_playing"]                         → bool
["progress_ms"]                        → current position (ms)
["item"]["name"]                       → track title
["item"]["duration_ms"]               → track duration (ms)
["item"]["artists"][0]["name"]         → artist
["item"]["album"]["images"][0]["url"]  → album art URL (640×640 JPEG)
```

---

## Hardware

**Board:** Waveshare ESP32-S3 1.8" Touch LCD
**MCU:** ESP32-S3, dual-core LX7 @ 240MHz, 320KB SRAM, 8MB PSRAM (OPI), 16MB Flash
**Display:** SH8601 360×360 round AMOLED, QSPI interface
**Touch:** CST816S, I2C at 0x15
**Encoder:** A/B quadrature via iot_knob library
**USB:** Built-in USB-JTAG/Serial, port `/dev/cu.usbmodem101`

### Actual pin assignments (from `include/config.h`)
```
LCD QSPI: SCK=13, D0=15, D1=16, D2=17, D3=18, CS=14, RST=21, BL=47
Touch I2C: SDA=11, SCL=12
Encoder: A=8, B=7
```

---

## Memory Rules

**Never allocate >10KB as a static array** — it runs before `setup()` and crashes the boot with no serial output.

```cpp
// Always do this for large buffers:
uint8_t *buf = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
// DMA buffers (display):
buf = (lv_color_t *)heap_caps_malloc(size, MALLOC_CAP_DMA);
```

**Current memory usage after full init (OTA partition — 6.25MB per app slot):**
- RAM: ~37.5% (123KB / 320KB) — healthy
- Flash: ~23.3% (1.52MB / 6.25MB) — healthy, ~4× headroom per OTA slot
- PSRAM: 8MB available, ~26KB used for LVGL draw buffers

---

## Display Gotchas

1. **QSPI quad_mode**: `esp_lcd_panel_io_spi_config_t` must have `.flags.quad_mode = true` or pixels display wrong colors (yellow tint). Init commands work without it, pixel data does not.

2. **Byte swap**: SH8601 expects big-endian RGB565. `lvgl_display_flush()` manually swaps each pixel. `LV_COLOR_16_SWAP` is 0 in `lv_conf.h`. Do not change either without changing the other.

3. **`lv_disp_flush_ready()` is NOT called in the flush callback** — it's called by the `on_color_trans_done` ISR in the panel driver. Calling it manually causes flickering/corruption.

4. **Rounder callback**: `lvgl_rounder_cb()` rounds areas to even x1/y1 and odd x2/y2. Required for the SH8601 QSPI timing. Do not remove.

---

## LVGL Notes

- Version: 8.4.0 (installed via PlatformIO)
- Config: `include/lv_conf.h` — fonts enabled: montserrat 12, 14, 16, 20, 24, 30, 40
- Tick: custom, driven by `lv_tick_custom` — see `include/lv_conf.h`
- `lv_timer_handler()` is called in `loop()` every 5ms on Core 1
- Never call LVGL from Core 0 or from an ISR
- Text pill width: `LV_SIZE_CONTENT` + `lv_obj_set_style_max_width(220)` — shrinks to text, caps at 220px, SCROLL_CIRCULAR kicks in beyond max
- **Text outline technique**: LVGL 8 has no text stroke. Volume number uses 4 black labels at cardinal offsets (±2px N/S/E/W) rendered behind the blue label to produce a clean outline.
- **Arc outline technique**: `s_arc_outline` (286×286, width=24, black) created before `s_arc` (280×280, width=18, blue) gives a ~3px dark border on both edges of the arc track.

---

## UI Overview

### Main playback screen

Layer order (back → front): art canvas → arc outline → arc → vol shadow labels → vol label → title pill → artist pill → progress bar outline (hidden) → progress bar (hidden) → **waveform canvas** → playback buttons → control panel → standby screen

**Layout (all positions are center-relative y offsets):**

| Element | Position | Notes |
|---|---|---|
| Volume arc outline | CENTER, 0, 0 | 286×286, black, width=24 |
| Volume arc | CENTER, 0, 0 | 280×280, blue #00BFFF, width=18, 240° sweep |
| Volume number | CENTER, 0, -68 | Montserrat 40, blue #00BFFF, ±2px black outline |
| Track title | CENTER, 0, -5 | Montserrat 20, dark pill, scrolls if >220px |
| Artist name | CENTER, 0, +20 | Montserrat 16, dark pill, hidden when empty or USB+no-Spotify |
| Progress bar outline | CENTER, 0, +81 | 206×16px — **permanently hidden** (replaced by waveform) |
| Progress bar | CENTER, 0, +81 | 200×10px — **permanently hidden** (replaced by waveform) |
| Waveform canvas | CENTER, 0, +75 | 160×30px, 20 bars × 5px, driven by PDM mic in real time |
| Playback buttons | CENTER, 0/±70, +126 | See state table below |

**Per-source display state:**

| Source | Spotify active | Title | Artist | Progress bar | Buttons |
|---|---|---|---|---|---|
| WiFi | — | track title | artist (or hidden) | shown | prev (56) + play/pause (68, blue) + next (56) |
| USB | yes | track title | artist (or hidden) | shown | prev (56) + play/pause (68, blue) + next (56) |
| USB | no | `"USB"` | hidden | hidden | mute toggle (68, blue/orange) |

Mute button: blue icon + dark bg when unmuted; orange icon + dark-red bg when muted.

**Arc geometry:** 240° sweep from 150° to 30° (gap at bottom, 30°→150° clockwise). Arc endpoints are at center-relative y=+70. Progress bar top at y=+73 is below the arc — no overlap.

### Light control screen (swipe left from KEF screen)

Layer order (back → front): dark background → 4 round buttons → centre label → colour picker overlay

**Layout (360×360, positions are center-relative offsets):**

| Element | Absolute pos | CENTER offset | Notes |
|---|---|---|---|
| Colour Picker button | (180, 80) | (0, -100) | 60×60, `LV_SYMBOL_EDIT`, opens/closes colour picker popup |
| Brightness mode button | (80, 180) | (-100, 0) | 60×60, `LV_SYMBOL_IMAGE`, radio toggle |
| Colour Temp mode button | (280, 180) | (+100, 0) | 60×60, `LV_SYMBOL_SETTINGS`, radio toggle |
| Power Toggle button | (180, 280) | (0, +100) | 60×60, `LV_SYMBOL_POWER`, queues `{"state":"TOGGLE"}` |
| Centre label | (180, 180) | (0, 0) | Montserrat 30, shows context value (see below) |

**Centre label content:**
- Brightness mode active → `"72%"` (brightness/254 × 100)
- Colour temp mode active → `"3700K"` (1 000 000 / mired, rounded to nearest 100 K)
- Neither active → `"Office\nLight"`

**Button states:**
- Active mode button: `#003050` bg (dark blue tint)
- Inactive mode button: `#252525` bg
- Power button: `#1A3A1A` / green icon when on; `#252525` / grey when off

**Default encoder mode on screen entry:** `LIGHT_ENC_BRIGHTNESS` (Brightness button pre-lit)

**Encoder behaviour (mode-aware):**
- `ENCODER_MODE_LIGHT_BRIGHT`: each step ±`LIGHT_BRIGHTNESS_STEP` (3), debounced 150ms MQTT publish
- `ENCODER_MODE_LIGHT_COLORTEMP`: each step ±`LIGHT_COLORTEMP_STEP` (10), debounced 150ms publish
- `ENCODER_MODE_KEF_VOLUME`: unchanged from KEF screen

**Colour picker popup:**
- Full-screen dark overlay (`LV_OPA_80`)
- `lv_colorwheel_create()` 240×240, `LV_ALIGN_CENTER (0, -10)`
- `LV_EVENT_VALUE_CHANGED` queues `{"color":{"hue":H,"saturation":S}}`
- Brightness label at top: `"Brightness: 72%"`, updates via encoder
- "Done" button at `CENTER (0, +140)` closes popup, restores previous encoder mode
- While open: `light_screen_get_encoder_mode()` returns `LIGHT_ENC_BRIGHTNESS`; swipe gestures suppressed

**Navigation:**
- Swipe left on KEF screen → light screen (`LV_SCR_LOAD_ANIM_MOVE_LEFT`, 300ms)
- Swipe right on light screen → KEF screen (`LV_SCR_LOAD_ANIM_MOVE_RIGHT`, 300ms)
- Swipe-down control panel is suppressed on the light screen

### Control panel (swipe down from top)
- Dark overlay, 260×130, hidden until swipe-from-top gesture
- Power button (green = on, grey = standby)
- WiFi source button (highlighted when active)
- USB source button (highlighted when active)
- Tapping any button sends command and closes panel

### Standby screen
- Full-screen dark overlay, shown when `kefSpeakerStatus != "powerOn"`
- "Standby" label
- WiFi button (blue) — wakes speaker with WiFi input
- USB button (amber) — wakes speaker with USB input
- Touch gestures are suppressed while standby screen is visible

---

## Spotify Setup

1. Go to [Spotify Developer Dashboard](https://developer.spotify.com/dashboard) and create an app.
2. Add redirect URI: `http://127.0.0.1:8888/callback`
3. Copy Client ID and Secret into `include/config_local.h`.
4. Run the one-time Python auth script embedded in the `config_local.h` comments. It opens a browser, handles the OAuth callback, and prints the refresh token.
   - Required scopes: `user-read-currently-playing user-modify-playback-state`
5. Paste the refresh token into `config_local.h` as `SPOTIFY_REFRESH_TOKEN`.
6. Flash.

If credentials are empty, Spotify is silently skipped — the USB screen shows the mute button only.

---

## Build & Upload

```bash
pio run                                                        # build only (both envs)
pio run -e esp32-s3-devkitc-1 -t upload \
    --upload-port /dev/cu.usbmodem101                          # serial flash
pio run -e ota -t upload                                       # OTA flash via deskknob.local
pio run -t clean
```

OTA hostname and optional password: `OTA_HOSTNAME` / `OTA_PASSWORD` in `include/config.h`.

**Read serial output** (`pio device monitor` doesn't work headless):
```python
import serial, time
ser = serial.Serial('/dev/cu.usbmodem101', 115200, timeout=1)
ser.setDTR(False); time.sleep(0.1); ser.setDTR(True); time.sleep(0.5)
while True:
    if ser.in_waiting: print(ser.readline().decode('utf-8', errors='replace'), end='')
```

---

## Debugging Checklist

- No serial output at all → static large array allocated before `setup()`, or missing `-DARDUINO_USB_CDC_ON_BOOT=1` in `platformio.ini`
- Wrong display colors → check `quad_mode = true` and byte-swap (see Display Gotchas)
- Volume resets to 0 → KEF rate limit; ensure debounce + 3s poll cooldown are in place
- Volume set to wrong value at boot → spurious encoder GPIO glitch during init using g_volume=50 as wrong baseline; the `vol_known` guard in networkTask prevents sending until first kef_get_volume() succeeds
- LVGL crash / corruption → LVGL call made from Core 0 or ISR
- Multiple definition of `setup` → two `.cpp` files in `src/` both defining `setup()`
- RAM > 70% → move large allocations to PSRAM
- Source/power commands returning HTTP 500 → check you're using `roles=value` not `roles=activate`
- Standby screen showing when speaker is on → use `kef_get_speaker_status()`, not player `state`
- Spotify now-playing not showing → check credentials in `config_local.h`; check serial for `[Spotify]` log lines
- Spotify playback control returning 403 → Spotify Premium required; verify `user-modify-playback-state` scope
- Spotify 401 in logs → refresh token invalid or scope missing; re-run auth script
- OTA upload failing → device must be on WiFi and booted; use hostname `deskknob.local` or set IP directly in `platformio.ini`
- Waveform always flat → check serial for `[Mic] PDM init failed` — I2S0 may be in use by another driver, or GPIO 45/46 are in use
- Waveform jumpy/noisy → tune `MIC_BAR_MS` (slower) or tighten the decay constant in `mic_pdm.cpp`
- Waveform bars don't respond to music → the mic picks up ambient room sound; move DeskKnob closer to speakers or increase speaker volume

---

*Last updated: 2026-02-22*
*Working: display, touch, encoder, WiFi, KEF volume control, track control (WiFi), source switching, power on/off, standby detection, now-playing display, album art, round playback buttons (mute/play-pause/prev/next), Spotify API integration on USB (now-playing + playback control + progress), OTA firmware updates (ArduinoOTA via `deskknob.local`), haptic feedback via DRV2605 (encoder click, play/pause strong, next/prev/mute medium — graceful no-op if chip absent), **real-time mic waveform visualiser** (replaces progress bar — MSM261D4030H1CPM PDM MEMS mic → I2S0 PDM RX → 20-bar animated waveform driven by ambient sound), **Hue light control screen** (swipe left → second LVGL screen with brightness/colour-temp/power/colour-picker via Zigbee2MQTT on 192.168.1.99:1883; PubSubClient; encoder is mode-aware; lv_colorwheel popup for hue/saturation)*
