# KefKnob - Agent Instructions

> **Agent rule:** After completing any task that changes behaviour, adds/removes features, modifies the API usage, or changes shared state, update the relevant sections of this file before finishing. Keep the File Map, Architecture, KEF API Reference, UI Overview, and Debugging Checklist accurate. Update the "Last updated" line and the working-features list at the bottom.

KEF LSX II speaker controller running on a Waveshare ESP32-S3 1.8" Touch LCD.
**Current status: fully working.** Display renders, touch works, encoder works, WiFi connects, KEF volume/track/source/power control works, album art displays, standby screen shows when speaker is off, Spotify API integration on USB source works (now-playing metadata + playback control + progress bar).

---

## File Map — go here first

| What you want to change | File |
|---|---|
| Pin assignments, timeouts, task sizes, volume step, swipe threshold | `include/config.h` |
| WiFi credentials, speaker IP, Spotify credentials (gitignored) | `include/config_local.h` |
| LVGL settings (color depth, fonts, tick) | `include/lv_conf.h` |
| Main screen UI layout & widget updates | `src/ui/main_screen.cpp` / `.h` |
| KEF HTTP API calls | `src/network/kef_api.cpp` / `.h` |
| Spotify Web API (now-playing + playback control) | `src/network/spotify_api.cpp` / `.h` |
| App wiring: encoder, touch, tasks, LVGL init | `src/main.cpp` |
| Display hardware init (SH8601 QSPI) | `src/drivers/display_sh8601.cpp` / `.h` |
| Touch hardware init (CST816S I2C) | `src/drivers/touch_cst816.cpp` / `.h` |
| Encoder hardware init (iot_knob) | `src/drivers/encoder.c` / `.h` |
| SH8601 low-level LCD driver | `src/drivers/esp_lcd_sh8601.c` / `.h` |
| Library dependencies, board config | `platformio.ini` |

---

## Architecture

```
Core 0 — networkTask()                Core 1 — loop() / Arduino main
──────────────────────────────────    ────────────────────────────────────
Loops every 50ms                      lv_timer_handler() every 5ms
  ├─ If g_volume_dirty AND              Reads g_volume_target → immediate
  │    250ms since last send:             arc redraw (no waiting)
  │    kef_set_volume()                   (sends every 250ms while turning)
  ├─ If g_track_cmd set:                On g_state_dirty: snapshot state
  │    USB+Spotify active:               under mutex → main_screen_update()
  │      spotify_play/pause/next/prev()   main_screen_update_power_source()
  │    WiFi:
  │      kef_track_control()
  ├─ If g_control_cmd set:
  │    kef_set_source() / kef_set_power()
  │    last_poll_ms = 0 (immediate re-poll)
  └─ Every 1s:
       kef_get_speaker_status()  ← power on/off detection
       if USB source:
         spotify_get_now_playing() → g_spotify_active, g_progress_pct
       else:
         kef_get_player_data()   → client-side progress estimation
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
static volatile char     g_control_cmd[16];       // "power"/"src_wifi"/"src_usb"/"pwr_wifi"/"pwr_usb"

// Written by Core 0, read by Core 1
static volatile bool g_power_on;
static volatile bool g_source_is_usb;
static volatile bool g_is_muted;
static volatile bool g_spotify_active;  // true = Spotify session detected on USB
static volatile int  g_progress_pct;    // track progress 0-100
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
- To queue a command from a button: write to `s_pending_track_cmd` in `main_screen.cpp`; `loop()` polls `main_screen_take_track_cmd()` and forwards to `g_track_cmd`
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
- **Do not use player state for standby detection** — `"stopped"` is returned for both on-but-idle and off. Use `speakerStatus` instead.

**Dead code in `kef_api.cpp` — do not use:**
- `kef_get_power()` — queries `player:power` which returns HTTP 500 on this firmware
- `kef_power_on()` — superseded by `kef_set_power(true)` which calls `kef_set_source("powerOn")`

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

**Current memory usage after full init:**
- RAM: ~36.1% (118KB / 328KB) — healthy
- Flash: ~46.6% (1.47MB / 3.15MB) — healthy
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

Layer order (back → front): art canvas → arc outline → arc → vol shadow labels → vol label → title pill → artist pill → progress bar outline → progress bar → playback buttons → control panel → standby screen

**Layout (all positions are center-relative y offsets):**

| Element | Position | Notes |
|---|---|---|
| Volume arc outline | CENTER, 0, 0 | 286×286, black, width=24 |
| Volume arc | CENTER, 0, 0 | 280×280, blue #00BFFF, width=18, 240° sweep |
| Volume number | CENTER, 0, -68 | Montserrat 40, blue #00BFFF, ±2px black outline |
| Track title | CENTER, 0, -5 | Montserrat 20, dark pill, scrolls if >220px |
| Artist name | CENTER, 0, +20 | Montserrat 16, dark pill, hidden when empty |
| Progress bar outline | CENTER, 0, +81 | 206×16px, black |
| Progress bar | CENTER, 0, +81 | 200×10px, blue #00BFFF fill, dark track |
| Playback buttons | CENTER, 0/±70, +126 | See below |

**Playback buttons** (round, icon-only, shown based on source + state):
- USB + Spotify active: prev (56×56, x=-70) + play-pause (68×68, center, blue icon) + next (56×56, x=+70)
- USB + no Spotify: mute toggle (68×68, center; orange icon + dark red bg when muted, blue when not)
- WiFi: prev + play-pause + next (same as USB+Spotify)

**Arc geometry:** 240° sweep from 150° to 30° (gap at bottom, 30°→150° clockwise). Arc endpoints are at center-relative y=+70. Progress bar top at y=+73 is below the arc — no overlap.

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

**Playback control (play/pause/next/prev on USB screen) requires Spotify Premium.**

If credentials are empty, Spotify is silently skipped — the USB screen shows the mute button only.

---

## Build & Upload

```bash
pio run                                              # build only
pio run -t upload --upload-port /dev/cu.usbmodem101  # build + upload
pio run -t clean                                     # clean
pio device list                                      # find port
```

**Read serial output:**
```python
import serial, time
ser = serial.Serial('/dev/cu.usbmodem101', 115200, timeout=1)
ser.setDTR(False); time.sleep(0.1); ser.setDTR(True); time.sleep(0.5)
while True:
    if ser.in_waiting: print(ser.readline().decode('utf-8', errors='replace'), end='')
```

`pio device monitor` does not work in headless environments — use pyserial above.

---

## Debugging Checklist

- No serial output at all → static large array allocated before `setup()`, or missing `-DARDUINO_USB_CDC_ON_BOOT=1` in `platformio.ini`
- Wrong display colors → check `quad_mode = true` and byte-swap (see Display Gotchas)
- Volume resets to 0 → KEF rate limit; ensure debounce + 3s poll cooldown are in place
- LVGL crash / corruption → LVGL call made from Core 0 or ISR
- Multiple definition of `setup` → two `.cpp` files in `src/` both defining `setup()`
- RAM > 70% → move large allocations to PSRAM
- Source/power commands returning HTTP 500 → check you're using `roles=value` not `roles=activate`
- Standby screen showing when speaker is on → player `state` field is unreliable; `kef_get_speaker_status()` must be called
- Spotify now-playing not showing → check credentials in `config_local.h`; check serial for `[Spotify]` log lines
- Spotify playback control returning 403 → Spotify Premium required; also verify `user-modify-playback-state` scope in refresh token
- Spotify 401 in logs → refresh token invalid or scope missing; re-run auth script

---

*Last updated: 2026-02-18*
*Working: display, touch, encoder, WiFi, KEF volume control, track control (WiFi), source switching, power on/off, standby detection, now-playing display, album art, round playback buttons (mute/play-pause/prev/next), track progress bar, Spotify API integration on USB (now-playing + playback control + progress)*
