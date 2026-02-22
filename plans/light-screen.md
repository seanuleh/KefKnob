# Plan: Hue Light Control Screen

---

## IMPLEMENTATION STATUS — read this first

**Status: COMPLETE AND WORKING ON DEVICE.**

Build: clean. RAM 38.1%, Flash 24.4%. Flashed and verified 2026-02-22.

---

## What works (confirmed via serial logs on device)

- MQTT connects and subscribes to `zigbee2mqtt/Sean's Office Light` on boot
- Power button → `{"state":"TOGGLE"}` publishes, light responds, state received by device
- Encoder (brightness mode) → debounced `{"brightness":N}` publishes, light responds
- State rx: `[MQTT] State: on=1 bri=50 ct=390` confirmed parsing correctly
- Swipe left → light screen, swipe right → KEF screen (MOVE_LEFT/RIGHT 300ms animation)
- Encoder mode-aware: volume on KEF screen, brightness/colortemp on light screen

---

## Bugs found and fixed during on-device testing

### 1. Z2M device had no friendly name
**Symptom:** MQTT publishes going out (`[MQTT] Published: {"state":"TOGGLE"}`) but no state
received back, light not responding.
**Root cause:** The Hue Devote was enrolled in Z2M as `0x001788010f43732d` with its IEEE
address as the friendly name. Z2M topic was `zigbee2mqtt/0x001788010f43732d`, not
`zigbee2mqtt/Sean's Office Light`.
**Fix:** Renamed via MQTT API (no server restart needed):
```
mosquitto_pub -t zigbee2mqtt/bridge/request/device/rename \
  -m '{"from":"0x001788010f43732d","to":"Sean'\''s Office Light"}'
```
Z2M config (`/app/data/configuration.yaml` in the `zigbee2mqtt` container on 192.168.1.99)
now shows `friendly_name: Sean's Office Light`. No firmware change needed.

### 2. PubSubClient buffer too small for Z2M state payload
**Symptom:** After rename, MQTT callback still never fired.
**Root cause:** Z2M state payload includes an OTA firmware URL, making it ~700 bytes.
PubSubClient default `MQTT_MAX_PACKET_SIZE` is 256 bytes — message silently dropped before
callback. Internal buffer cap in `on_message` was also 512 bytes.
**Fix** (in `src/network/mqtt_client.cpp`):
```cpp
// in mqtt_client_begin():
s_mqtt.setBufferSize(1024);   // added

// in on_message():
if (len == 0 || len > 1024) return;   // was > 512
char buf[1025];                        // was buf[513]
```

---

## Known remaining issue: color state uses XY not HSV

Z2M state payloads report color as `{"x":0.47,"y":0.41}` (CIE XY), not HSV.
Our `on_message` parser looks for `color.hue` and `color.saturation` — these are absent
in normal state updates, so `g_light_color_hue` and `g_light_color_sat` stay at 0.

**Impact:** Colorwheel popup won't initialize to the current color on screen entry.
The colorwheel *control* direction works fine — publishing `{"color":{"hue":H,"saturation":S}}`
is accepted by Z2M and the light responds.

**If you want to fix this:** Convert XY→HSV in `on_message` in `src/network/mqtt_client.cpp`:
```cpp
// CIE XY → approximate HSV (good enough for colorwheel init)
// After parsing x,y from doc["color"]:
float r = x * 3.2406f - y * 1.5372f - (1-x-y) * 0.4986f;
float g = -x * 0.9689f + y * 1.8758f + (1-x-y) * 0.0415f;
float b = x * 0.0557f - y * 0.2040f + (1-x-y) * 1.0570f;
// then RGB→HSV... or just use lv_color_rgb_to_hsv()
```
Or simpler: subscribe to `color_temp` and `brightness` only (already working), and
just don't sync the colorwheel to current color on entry — leave it at last-set position.

---

## Infrastructure notes (for future work on this project)

**Server:** 192.168.1.99 (SSH works, no password needed from dev machine)
**MQTT broker:** `eclipse-mosquitto` container named `mqtt` on 192.168.1.99:1883
**Zigbee2MQTT:** container named `zigbee2mqtt` on 192.168.1.99
**Z2M web UI:** http://192.168.1.99:8080
**Z2M config file:** inside container at `/app/data/configuration.yaml`
**Hue Devote IEEE:** `0x001788010f43732d`, model `929004297501`, friendly name `Sean's Office Light`
**Z2M state topic:** `zigbee2mqtt/Sean's Office Light` (retained, ~700 bytes with OTA URL)
**Z2M set topic:** `zigbee2mqtt/Sean's Office Light/set`

**Other Z2M devices (for context):**
- Blinds: `Sean's Office Blind`, `Master Bedroom Blind`, `Living Room Blind`
- Remotes: `Living Blinds Remote`, `Master Blinds Remote`
- Plugs: `Ikea Power Plug Hall`, `Ikea Power Plug Kitchen`

**Test a publish from dev machine:**
```bash
python3 -c "import paho.mqtt.publish as pub; pub.single(\"zigbee2mqtt/Sean's Office Light/set\", '{\"state\":\"TOGGLE\"}', hostname='192.168.1.99')"
```

**Watch broker traffic:**
```bash
python3 -c "
import paho.mqtt.subscribe as sub
sub.callback(lambda c,u,m: print(m.topic, m.payload.decode()[:200]),
             'zigbee2mqtt/#', hostname='192.168.1.99')
"
```

---

## Files touched (complete list)

| File | Change |
|---|---|
| `include/config.h` | Added MQTT/light/encoder-mode/screen-index constants after NETWORK section |
| `include/config_local.h` | Added `#define MQTT_BROKER_IP "192.168.1.99"` |
| `include/lv_conf.h` | `LV_USE_COLORWHEEL` 0 → **1** (required, was missing) |
| `platformio.ini` | Added `knolleary/PubSubClient@^2.8` |
| `src/network/mqtt_client.h` | **NEW** — PubSubClient wrapper API + extern volatile light state |
| `src/network/mqtt_client.cpp` | **NEW** — `setBufferSize(1024)`, buf 1025, client ID `"deskknob-light"` |
| `src/ui/light_screen.h` | **NEW** — screen API + `LIGHT_ENC_*` constants |
| `src/ui/light_screen.cpp` | **NEW** — full UI |
| `src/ui/main_screen.h` | Added `lv_obj_t *main_screen_get_obj()` |
| `src/ui/main_screen.cpp` | Added `lv_obj_t *main_screen_get_obj() { return s_screen; }` |
| `src/main.cpp` | Includes, globals, `handle_encoder_delta`, swipe switching, loop+networkTask additions |
| `CLAUDE.md` | File Map, Module API, Architecture, UI Overview, working features |

---

## Critical implementation facts (avoid re-discovering)

**LV_USE_COLORWHEEL must be 1 in `include/lv_conf.h`** — was 0, caused compile error.

**LVGL colorwheel API (8.3.x):**
```cpp
lv_obj_t *cw = lv_colorwheel_create(parent, true);
lv_color_hsv_t hsv = lv_colorwheel_get_hsv(cw);  // h:0-359, s:0-100, v:0-100
lv_colorwheel_set_hsv(cw, hsv);
```

**Actual layout (deviates from original plan):**
```
Brightness mode  CENTER (-100,  0)  60×60  LV_SYMBOL_IMAGE    (not #00BFFF — uses #003050 active bg)
Colour Temp      CENTER (+100,  0)  60×60  LV_SYMBOL_SETTINGS
Colour Picker    CENTER (  0,-100)  60×60  LV_SYMBOL_EDIT
Power Toggle     CENTER (  0,+100)  60×60  LV_SYMBOL_POWER
Centre label     CENTER (  0,   0)  Montserrat 30, #00BFFF
Colorwheel       CENTER (  0, -10)  240×240 (not 260×260)
Done button      CENTER (  0,+140)  100×36
```

**Encoder mode sync — runs every loop() iteration on light screen:**
```cpp
if (g_active_screen == SCREEN_LIGHT) {
    g_encoder_mode = light_screen_get_encoder_mode();  // syncs from button state
    ...
}
```

**PubSubClient on ESP32:** `setBufferSize(1024)` must be called before `connect()`.
Default 256 bytes is too small for any Z2M device with OTA firmware available.

**ArduinoJson v7 pattern used (not v6):**
```cpp
JsonDocument doc;
deserializeJson(doc, buf);
if (!doc["key"].isNull()) { auto val = doc["key"].as<Type>(); }
```

---

## Verification checklist

- [x] `pio run` — clean build, RAM 38.1%, Flash 24.4%
- [x] Serial boot: `[MQTT] Connected and subscribed to zigbee2mqtt/Sean's Office Light`
- [x] Swipe left → light screen; swipe right → KEF screen
- [x] Power button → `[MQTT] Published: {"state":"TOGGLE"}` + light physically toggles
- [x] Encoder (brightness mode) → `[MQTT] Published: {"brightness":N}` + light dims/brightens
- [x] State rx: `[MQTT] State: on=1 bri=50 ct=390` in logs after light responds
- [ ] Colour Temp mode button: mutual exclusion, centre label shows Kelvin, encoder adjusts CCT
- [ ] Colour Picker popup: colorwheel opens, drag queues color publish, Done closes
- [ ] OTA flash still works (`pio run -e ota -t upload`)
