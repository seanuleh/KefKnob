# Plan: Hue Light Control Screen

## Context

Adding a second LVGL screen to DeskKnob to control the Philips Hue Devote ("Sean's Office Light") connected via Zigbee2MQTT on the home MQTT broker (`192.168.1.99:1883`). The same broker is already used by the `desktop-dash` ESPHome device in this setup, confirming the pattern of direct MQTT control without routing through HA's native API.

The new screen is reachable by horizontal swipe from the KEF screen (the existing TODO in `lvgl_touch_read()`). The encoder becomes mode-aware: on the KEF screen it controls volume as today; on the light screen it controls brightness or colour temperature depending on which mode button is active.

---

## Z2M MQTT API used

| Direction | Topic | Payload example |
|---|---|---|
| Subscribe (state) | `zigbee2mqtt/Sean's Office Light` | `{"state":"ON","brightness":180,"color_temp":370,"color":{"hue":120,"saturation":80}}` |
| Publish (control) | `zigbee2mqtt/Sean's Office Light/set` | `{"state":"TOGGLE"}` |
| Publish (brightness) | `zigbee2mqtt/Sean's Office Light/set` | `{"brightness":180}` |
| Publish (colour temp) | `zigbee2mqtt/Sean's Office Light/set` | `{"color_temp":300}` |
| Publish (colour) | `zigbee2mqtt/Sean's Office Light/set` | `{"color":{"hue":200,"saturation":90}}` |

Brightness: 0–254. Colour temp: 153–500 Mired (≈6500 K – 2000 K). Hue: 0–360. Saturation: 0–100.

---

## Files to Create

### `src/network/mqtt_client.h` / `.cpp`

PubSubClient wrapper. Lives entirely on Core 0 inside `networkTask()`. The MQTT receive callback fires during `mqtt_client_loop()` and writes to the volatile globals exported from this translation unit.

**Exported volatile state** (Core 0 writes, Core 1 reads):
```cpp
extern volatile bool  g_light_on;
extern volatile int   g_light_brightness;   // 0–254
extern volatile int   g_light_colortemp;    // Mired 153–500
extern volatile float g_light_color_hue;    // 0–360
extern volatile float g_light_color_sat;    // 0–100
extern volatile bool  g_light_state_dirty;  // Core 0 → Core 1 paint signal
```

**API:**
```cpp
void mqtt_client_begin(const char *broker_ip, int port);
void mqtt_client_loop();                        // call every networkTask iteration
bool mqtt_light_publish(const char *json_payload);
```

Reconnects automatically every 5 s if disconnected. If `MQTT_BROKER_IP` is empty the whole module is a no-op.

---

### `src/ui/light_screen.h` / `.cpp`

Core 1 only — same rule as `main_screen`. Builds widgets but does NOT call `lv_scr_load()` (screen switching is driven from `main.cpp`).

**API:**
```cpp
lv_obj_t  *light_screen_get_obj();
void        light_screen_create();
void        light_screen_update(bool on, int brightness, int colortemp,
                                float hue, float sat);
const char *light_screen_take_cmd();        // JSON payload for MQTT /set, clears on read
int         light_screen_get_encoder_mode();// LIGHT_ENC_NONE / BRIGHTNESS / COLORTEMP
bool        light_screen_is_colorpicker_open();
```

**Layout** — four 60×60 round buttons at radius 100 px from the 180,180 centre of the 360×360 round display:

```
              [Colour Picker]   (top,    180, 80)

[Brightness]  [centre label]  [Colour Temp]
 (left, 80,180)               (right, 280,180)

              [Power Toggle]    (bottom, 180,280)
```

Style matches `main_screen`: `#252525` idle bg, `LV_RADIUS_CIRCLE`, `lv_font_montserrat_20` icons, no border, no shadow. Active mode button gets `#00BFFF` bg tint. Power button green `#1A3A1A` / `#44EE44` when on, grey when off.

**Centre label** (`lv_font_montserrat_30`): shows context-sensitive value:
- Brightness mode active → `"72%"` (brightness/254 × 100)
- Colour temp mode active → `"3700K"` (1 000 000 / mired, rounded to nearest 100 K)
- Neither active → `"Office Light"`

**Button behaviour:**

| Button | Tap action |
|---|---|
| Brightness Mode | Radio toggle — activates `LIGHT_ENC_BRIGHTNESS`; tapping active btn deactivates |
| Colour Temp Mode | Radio toggle — mutually exclusive with Brightness Mode; activates `LIGHT_ENC_COLORTEMP` |
| Power Toggle | Action — queues `{"state":"TOGGLE"}`, haptic STRONG |
| Colour Picker | Opens/closes the colour picker popup |

Default encoder mode on screen entry: `LIGHT_ENC_BRIGHTNESS` (Brightness button pre-lit).

**Colour picker popup:**
- Full-screen dark overlay (`LV_OPA_70`)
- `lv_colorwheel_create(overlay, true)` centred at 260×260 px
- `LV_EVENT_VALUE_CHANGED` callback: `lv_color_hsv_t hsv = lv_colorwheel_get_hsv(cw)` → queues `{"color":{"hue":H,"saturation":S}}`
- While open `light_screen_get_encoder_mode()` returns `LIGHT_ENC_BRIGHTNESS` (encoder adjusts brightness while choosing colour)
- Small "Done" button (`lv_font_montserrat_16`, centred, y = 310) closes popup and restores previous encoder mode
- Brightness label at top (`"Brightness: 72%"`) updates as encoder turns

---

## Files to Modify

### `include/config.h`

Add after the NETWORK section:

```c
// MQTT broker (set MQTT_BROKER_IP in config_local.h, e.g. "192.168.1.99")
#ifndef MQTT_BROKER_IP
#  define MQTT_BROKER_IP  ""     // empty = MQTT disabled
#endif
#define MQTT_BROKER_PORT      1883
#define MQTT_LIGHT_TOPIC      "zigbee2mqtt/Sean's Office Light"
#define MQTT_LIGHT_SET_TOPIC  "zigbee2mqtt/Sean's Office Light/set"

// Light encoder controls
#define LIGHT_BRIGHTNESS_MIN    0
#define LIGHT_BRIGHTNESS_MAX    254
#define LIGHT_BRIGHTNESS_STEP   3
#define LIGHT_COLORTEMP_MIN     153    // Mired (~6500 K)
#define LIGHT_COLORTEMP_MAX     500    // Mired (~2000 K)
#define LIGHT_COLORTEMP_STEP    10
#define LIGHT_DEBOUNCE_MS       150    // ms between MQTT publishes while encoder spins

// Encoder mode constants
#define ENCODER_MODE_KEF_VOLUME      0
#define ENCODER_MODE_LIGHT_BRIGHT    1
#define ENCODER_MODE_LIGHT_COLORTEMP 2

// Screen indices
#define SCREEN_KEF    0
#define SCREEN_LIGHT  1
```

Also add to `config_local.h` (user-edited, gitignored):
```c
#define MQTT_BROKER_IP "192.168.1.99"
```

### `platformio.ini`

Add to `lib_deps`:
```
knolleary/PubSubClient@^2.8
```

### `src/ui/main_screen.h` + `.cpp`

Add one function to expose the screen object for `lv_scr_load_anim`:
```cpp
// main_screen.h
lv_obj_t *main_screen_get_obj();

// main_screen.cpp — one line implementation
lv_obj_t *main_screen_get_obj() { return s_screen; }
```

### `src/main.cpp`

**New `#include`s:**
```cpp
#include "network/mqtt_client.h"
#include "ui/light_screen.h"
```

**New volatile globals** (after existing ones):
```cpp
static volatile int  g_encoder_mode            = ENCODER_MODE_KEF_VOLUME;
static volatile int  g_active_screen           = SCREEN_KEF;
static volatile int  g_light_brightness_target = -1;  // Core 1 → Core 0, -1=none
static volatile int  g_light_colortemp_target  = -1;
static volatile char g_light_cmd[192]          = "";  // JSON payload, Core 1→Core 0
```

**`encoder_left_cb` / `encoder_right_cb`** — replace the unconditional volume logic with a switch:
```cpp
switch (g_encoder_mode) {
    case ENCODER_MODE_KEF_VOLUME:
        // existing volume logic unchanged
        break;
    case ENCODER_MODE_LIGHT_BRIGHT: {
        int cur = (g_light_brightness_target >= 0) ? g_light_brightness_target
                                                   : (int)g_light_brightness;
        int nxt = cur + delta * LIGHT_BRIGHTNESS_STEP;
        if (nxt < LIGHT_BRIGHTNESS_MIN) nxt = LIGHT_BRIGHTNESS_MIN;
        if (nxt > LIGHT_BRIGHTNESS_MAX) nxt = LIGHT_BRIGHTNESS_MAX;
        g_light_brightness_target = nxt;
        break;
    }
    case ENCODER_MODE_LIGHT_COLORTEMP: {
        int cur = (g_light_colortemp_target >= 0) ? g_light_colortemp_target
                                                  : (int)g_light_colortemp;
        int nxt = cur + delta * LIGHT_COLORTEMP_STEP;
        if (nxt < LIGHT_COLORTEMP_MIN) nxt = LIGHT_COLORTEMP_MIN;
        if (nxt > LIGHT_COLORTEMP_MAX) nxt = LIGHT_COLORTEMP_MAX;
        g_light_colortemp_target = nxt;
        break;
    }
}
```
(`delta` = +1 for right, -1 for left — factor out from the two callbacks.)

**`initLVGL()`** — after `main_screen_create()`:
```cpp
light_screen_create();   // builds widgets, does NOT load the screen
```

**`lvgl_touch_read()` — horizontal swipe branch** (replaces TODO comment):
```cpp
if (horiz_swipe && !light_screen_is_colorpicker_open()) {
    if (g_active_screen == SCREEN_KEF && dx < 0) {
        // Swipe left → Light screen slides in from the right
        g_active_screen = SCREEN_LIGHT;
        g_encoder_mode  = light_screen_get_encoder_mode();
        lv_scr_load_anim(light_screen_get_obj(),
                         LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
    } else if (g_active_screen == SCREEN_LIGHT && dx > 0) {
        // Swipe right → KEF screen slides back in from the left
        g_active_screen = SCREEN_KEF;
        g_encoder_mode  = ENCODER_MODE_KEF_VOLUME;
        lv_scr_load_anim(main_screen_get_obj(),
                         LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
    }
}
```
Also guard the existing swipe-down control-panel branch with `&& g_active_screen == SCREEN_KEF`.

**`loop()` additions:**

```cpp
// Forward light button commands (power toggle, colour picker) to networkTask
{
    const char *lcmd = light_screen_take_cmd();
    if (lcmd && lcmd[0]) {
        strncpy((char *)g_light_cmd, lcmd, sizeof(g_light_cmd) - 1);
        g_haptic_event = HAPTIC_MEDIUM;
    }
}

// Power button gets a stronger haptic — override if the command is power toggle
// (light_screen sets a flag internally; checked via light_screen_is_power_cmd() — or just
// keep HAPTIC_STRONG for power in the button callback itself and HAPTIC_MEDIUM for others)

// Light screen update: encoder feedback + confirmed MQTT state
if (g_active_screen == SCREEN_LIGHT) {
    bool bri_pending = (g_light_brightness_target >= 0);
    bool ct_pending  = (g_light_colortemp_target  >= 0);
    if (bri_pending || ct_pending || g_light_state_dirty) {
        g_light_state_dirty = false;
        int bri = bri_pending ? (int)g_light_brightness_target : (int)g_light_brightness;
        int ct  = ct_pending  ? (int)g_light_colortemp_target  : (int)g_light_colortemp;
        light_screen_update((bool)g_light_on, bri, ct,
                            g_light_color_hue, g_light_color_sat);
    }
}
```

**`networkTask()` additions:**

After `spotify_init(...)`:
```cpp
mqtt_client_begin(MQTT_BROKER_IP, MQTT_BROKER_PORT);
```

In the main loop before the 1 s poll block:
```cpp
mqtt_client_loop();

// Publish brightness (debounced — send latest value after LIGHT_DEBOUNCE_MS quiescence)
if (g_light_brightness_target >= 0) {
    static uint32_t last_bri_ms = 0;
    if (now - last_bri_ms >= LIGHT_DEBOUNCE_MS) {
        int t = g_light_brightness_target; g_light_brightness_target = -1;
        char p[48]; snprintf(p, sizeof(p), "{\"brightness\":%d}", t);
        if (mqtt_light_publish(p)) g_light_brightness = t;
        last_bri_ms = now;
    }
}

// Publish colour temp (debounced)
if (g_light_colortemp_target >= 0) {
    static uint32_t last_ct_ms = 0;
    if (now - last_ct_ms >= LIGHT_DEBOUNCE_MS) {
        int t = g_light_colortemp_target; g_light_colortemp_target = -1;
        char p[48]; snprintf(p, sizeof(p), "{\"color_temp\":%d}", t);
        if (mqtt_light_publish(p)) g_light_colortemp = t;
        last_ct_ms = now;
    }
}

// Publish power toggle / colour commands from button/colorwheel
if (g_light_cmd[0] != '\0') {
    char cmd[192];
    strncpy(cmd, (const char *)g_light_cmd, sizeof(cmd) - 1);
    g_light_cmd[0] = '\0';
    mqtt_light_publish(cmd);
}
```

### `CLAUDE.md`

- Add `mqtt_client.h/.cpp` and `light_screen.h/.cpp` rows to the File Map
- Add MQTT Client section to Module API (matching the style of existing sections)
- Add light screen section to UI Overview
- Update working-features list and Last Updated date

---

## Thread Safety

| Variable | Written by | Read by | Mechanism |
|---|---|---|---|
| `g_light_on/brightness/colortemp/hue/sat` | Core 0 (MQTT cb) | Core 1 (loop) | `volatile` — single writer |
| `g_light_state_dirty` | Core 0 | Core 1 | `volatile` |
| `g_light_brightness_target` | Core 1 (encoder cb) | Core 0 (networkTask) | `volatile` |
| `g_light_colortemp_target` | Core 1 (encoder cb) | Core 0 (networkTask) | `volatile` |
| `g_light_cmd[192]` | Core 1 (loop) | Core 0 (networkTask) | `volatile char[]` |
| `g_encoder_mode` | Core 1 (touch cb, loop) | Core 1 (encoder cb) | same core, no mutex |
| `g_active_screen` | Core 1 (touch cb) | Core 1 (loop, touch cb) | same core, no mutex |

---

## Verification

1. `pio run` — zero errors, zero new warnings
2. Serial on boot: `[MQTT] Connected and subscribed to zigbee2mqtt/Sean's Office Light`
3. Swipe left on KEF screen → light screen slides in (MOVE_LEFT animation); swipe right → back to KEF
4. Control panel swipe-down on KEF screen still works; does not trigger on light screen
5. Brightness Mode button highlights blue → encoder adjusts brightness (immediate label feedback) → after 150 ms MQTT publish visible in Z2M logs
6. Colour Temp Mode button: mutual exclusion with Brightness Mode confirmed; centre label shows Kelvin; encoder adjusts CCT
7. Power button: light toggles on/off; button colour updates on confirmed MQTT state update
8. Colour Picker button: colorwheel popup appears; drag to change hue/saturation → Z2M log shows `{"color":{"hue":...}}` publish; encoder adjusts brightness; "Done" closes popup
9. Disconnect network: reconnect within 5 s; encoder changes queue and publish after reconnect
10. OTA flash still works after adding PubSubClient
