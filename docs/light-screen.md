# Hue Light Control Screen

Second LVGL screen (swipe left from KEF screen) for controlling a Zigbee2MQTT-managed Hue bulb via MQTT.

---

## What works

- MQTT connects and subscribes to `zigbee2mqtt/Sean's Office Light` on boot
- Power button → `{"state":"TOGGLE"}` publishes, light responds, state received by device
- Encoder (brightness mode) → debounced `{"brightness":N}` publishes, light responds
- Encoder (colour temp mode) → debounced `{"color_temp":N}` publishes, light responds
- State rx: `[MQTT] State: on=1 bri=50 ct=390` confirmed parsing correctly
- Swipe left → light screen, swipe right → KEF screen (MOVE_LEFT/RIGHT 300ms animation)
- Encoder is mode-aware: volume on KEF screen, brightness/colortemp on light screen
- Arc slider switches between brightness (filled, coloured indicator) and colour temp (gradient + knob)
- Brightness % and Kelvin value labels update in real time, coloured to match active arc
- Colour disc picker: full-screen HSV disc rendered in PSRAM, touch to pick hue/saturation
- Radio button behaviour: pressing brightness or CT always activates that mode (can never deactivate both)
- KEF standby screen still works after screen switch (encoder mode enforced per active screen)

---

## Known remaining issue: colour state uses XY not HSV

Z2M state payloads report colour as `{"x":0.47,"y":0.41}` (CIE XY).
`on_message` looks for `color.hue` / `color.saturation` — absent in normal state updates,
so `g_light_color_hue` and `g_light_color_sat` stay at 0.

**Impact:** Colour disc won't initialize to the current colour on screen entry. Colour
*control* works fine — publishing `{"color":{"hue":H,"saturation":S}}` is accepted by Z2M.

**To fix:** Convert XY→HSV in `on_message` in `src/network/mqtt_client.cpp`.

---

## Infrastructure

| Item | Value |
|---|---|
| Server | 192.168.1.99 (SSH, no password from dev machine) |
| MQTT broker | `eclipse-mosquitto` container `mqtt` on 192.168.1.99:1883 |
| Zigbee2MQTT | container `zigbee2mqtt` on 192.168.1.99 |
| Z2M web UI | http://192.168.1.99:8080 |
| Hue Devote IEEE | `0x001788010f43732d`, model `929004297501` |
| Friendly name | `Sean's Office Light` |
| Z2M state topic | `zigbee2mqtt/Sean's Office Light` (retained, ~700 bytes with OTA URL) |
| Z2M set topic | `zigbee2mqtt/Sean's Office Light/set` |

---

## Files

| File | Role |
|---|---|
| `include/config.h` | MQTT/light/encoder-mode/screen-index constants |
| `include/config_local.h` | `#define MQTT_BROKER_IP "192.168.1.99"` |
| `include/lv_conf.h` | `LV_USE_COLORWHEEL 1` (required) |
| `platformio.ini` | `knolleary/PubSubClient@^2.8` dependency |
| `src/network/mqtt_client.h/.cpp` | PubSubClient wrapper, volatile light state globals |
| `src/ui/light_screen.h/.cpp` | Full UI — arc sliders, colour disc picker, 2×2 button grid |
| `src/ui/main_screen.h/.cpp` | Added `main_screen_get_obj()` for swipe animation target |
| `src/main.cpp` | Globals, `handle_encoder_delta`, swipe switching, loop/networkTask |

---

## Implementation facts

**Arc geometry (matches KEF volume arc exactly):**
- Outline arc: 286×286, `lv_arc_set_bg_angles(150, 30)`, width=24, all black
- Main arc: 280×280, same angles, width=18, track=`#1E1E1E`
- 240° sweep: starts lower-left (150° = ~7:30), apex at top (270° = 12 o'clock), ends lower-right (30° = ~4:30)
- Value label: `LV_ALIGN_CENTER (0, -105)`, Montserrat 20, coloured to match arc

**Brightness arc:**
- `LV_PART_INDICATOR` colour = `lv_color_hsv_to_rgb(hue, sat, 100)` when in colour mode, else `mired_to_color(colortemp)` — 4-stop warm-to-cool interpolation
- `LV_PART_KNOB` transparent
- Range: `LIGHT_BRIGHTNESS_MIN` (0) to `LIGHT_BRIGHTNESS_MAX` (254)

**Colour temp arc:**
- 24 × 10° gradient segment arcs, colours computed from `mired_to_color()` at each midpoint
- Segments: only `LV_PART_MAIN` visible; `LV_PART_INDICATOR` arc_opa=TRANSP, `LV_PART_KNOB` bg_opa=TRANSP
- Control arc on top: `LV_PART_MAIN` + `LV_PART_INDICATOR` arc_opa=TRANSP, `LV_PART_KNOB` bg_opa=COVER (white circle, pad=5 → 28px knob)
- Range: `LIGHT_COLORTEMP_MIN` (153 mired) to `LIGHT_COLORTEMP_MAX` (500 mired)

**Button layout (2×2 grid, 76×76, centres at ±45 from screen centre):**
```
Power (LV_SYMBOL_POWER)       Colour Picker (LV_SYMBOL_EDIT)
Brightness (LV_SYMBOL_CHARGE) Colour Temp (LV_SYMBOL_TINT)
```
Worst corner distance from centre: √2 × 83 = 117px < 122px inner arc radius ✓

**LVGL symbols used** (LVGL 8.4 built-in set — no thermometer/sun/palette):
POWER, EDIT (colour picker), CHARGE (brightness), TINT (colour temp)

**Colour disc picker:**
- 360×360 canvas, PSRAM buffer (360×360×2 = 259 200 bytes), `LV_IMG_CF_TRUE_COLOR`
- Rendered once in `light_screen_create()` via `render_color_disc()`: per-pixel polar → HSV
- Disc radius: 170px, centred at (180, 180)
- Touch via `LV_EVENT_PRESSING` on overlay → `cp_touch_cb()` → computes (r, angle) → queues `{"color":{"hue":H,"saturation":S}}`
- Indicator: 20×20 transparent circle, white border 2px, shadow, moves with touch
- Done button: 80×80 circle, centred, semi-transparent dark, closes picker and restores encoder mode
- `heap_caps_malloc(…, MALLOC_CAP_SPIRAM)` — requires `#include <esp_heap_caps.h>`

**`show_bri_arcs()` / `show_ct_arcs()` own all mode state:**
- Set `s_encoder_mode` and call `update_mode_buttons()` internally
- All callers (button callbacks, Done, create) call these functions — never set mode directly

**PubSubClient on ESP32:** `setBufferSize(1024)` must be called before `connect()`.

**ArduinoJson v7 pattern:**
```cpp
JsonDocument doc;
deserializeJson(doc, buf);
if (!doc["key"].isNull()) { auto val = doc["key"].as<Type>(); }
```

---

## Bugs found and fixed

### 1. Z2M device had no friendly name
**Fix:** Renamed via MQTT API:
```
mosquitto_pub -t zigbee2mqtt/bridge/request/device/rename \
  -m '{"from":"0x001788010f43732d","to":"Sean'\''s Office Light"}'
```

### 2. PubSubClient buffer too small for Z2M state payload
**Fix** (`src/network/mqtt_client.cpp`):
```cpp
s_mqtt.setBufferSize(1024);
if (len == 0 || len > 1024) return;
char buf[1025];
```

### 3. Light screen shows stale defaults on cold boot
**Fix** (`src/main.cpp`, swipe-left handler):
```cpp
g_light_state_dirty = true;  // ensure light_screen_update() fires on entry
```

### 4. Gradient segment arcs showing blue knobs
**Root cause:** `lv_obj_set_style_arc_opa(seg, LV_OPA_TRANSP, LV_PART_KNOB)` hides the arc line but not the knob background (filled rect needs `bg_opa`).
**Fix:** `lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, LV_PART_KNOB)`

### 5. CT control arc knob not visible
**Root cause:** `lv_obj_set_style_opa(..., LV_PART_MAIN)` bleeds into knob rendering.
**Fix:** Use `lv_obj_set_style_arc_opa` for track lines and `lv_obj_set_style_bg_opa` for the knob separately. Set `lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, 0)` on the widget container to avoid covering gradient segments underneath.

### 6. Button highlight desync — neither brightness nor CT highlighted
**Root cause:** Multiple code paths each partially updating `s_encoder_mode` and `update_mode_buttons()` independently.
**Fix:** `show_bri_arcs()` / `show_ct_arcs()` are the single source of truth — they set `s_encoder_mode` and call `update_mode_buttons()` internally.

### 7. KEF standby screen not showing / encoder stuck in brightness mode after screen switch
**Root cause (standby):** When KEF enters deep standby its network stack goes down. `kef_get_speaker_status()` times out (returns `false`), so `g_power_on` was never set to `false` and the standby overlay never appeared.
**Fix** (`src/main.cpp`, `networkTask`): Added a consecutive-failure counter — after 3 failed calls (~3 s) `g_power_on` is forced `false`.

**Root cause (encoder):** No continuous enforcement kept the encoder in `ENCODER_MODE_KEF_VOLUME` when on the KEF screen. A failed or partial swipe-back could leave it in a light-screen mode.
**Fix** (`src/main.cpp`, `loop()`): Added a guard that sets `g_encoder_mode = ENCODER_MODE_KEF_VOLUME` on every iteration while `g_active_screen == SCREEN_KEF`, mirroring how the light screen already enforces its mode.
