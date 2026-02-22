#pragma once

#include <lvgl.h>
#include <stdint.h>
#include <stdbool.h>

// Encoder mode constants for the light screen
#define LIGHT_ENC_NONE       0
#define LIGHT_ENC_BRIGHTNESS 1
#define LIGHT_ENC_COLORTEMP  2

// Returns the underlying lv_obj_t* for lv_scr_load_anim transitions.
lv_obj_t *light_screen_get_obj();

// Create all widgets.  Does NOT call lv_scr_load — switching is driven by main.cpp.
// Call once from Core 1 after main_screen_create().
void light_screen_create();

// Redraw all state-dependent widgets.
// Called from Core 1 whenever a pending encoder target or g_light_state_dirty is set.
void light_screen_update(bool on, int brightness, int colortemp, float hue, float sat);

// Take (and clear) a pending JSON command queued by a button tap or colorwheel drag.
// Returns nullptr if none pending.  Core 1 only.
const char *light_screen_take_cmd();

// Current encoder mode.
// Returns LIGHT_ENC_BRIGHTNESS while the colorpicker popup is open (encoder always
// adjusts brightness while picking colour).
int light_screen_get_encoder_mode();

// True while the colorpicker popup is visible (suppresses swipe gestures).
bool light_screen_is_colorpicker_open();
