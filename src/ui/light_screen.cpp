#include "light_screen.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Widget handles (file-local)
// ---------------------------------------------------------------------------

static lv_obj_t *s_screen      = NULL;

// Four mode/action buttons (60×60, LV_RADIUS_CIRCLE)
static lv_obj_t *s_btn_bri     = NULL;   // top-left: Brightness mode
static lv_obj_t *s_btn_ct      = NULL;   // top-right: Colour Temp mode
static lv_obj_t *s_btn_pwr     = NULL;   // bottom: Power Toggle
static lv_obj_t *s_btn_cp      = NULL;   // top-centre: Colour Picker open/close

// Centre label: shows brightness%, kelvin, or "Office Light"
static lv_obj_t *s_centre_lbl  = NULL;

// Power button icon label (recoloured per on/off state)
static lv_obj_t *s_pwr_icon    = NULL;

// Colour picker popup
static lv_obj_t *s_cp_overlay  = NULL;
static lv_obj_t *s_cp_wheel    = NULL;
static lv_obj_t *s_cp_bri_lbl  = NULL;   // "Brightness: 72%" at top of popup
static lv_obj_t *s_cp_done     = NULL;
static bool      s_cp_open     = false;
static int       s_cp_prev_enc = LIGHT_ENC_BRIGHTNESS;  // restored when Done is tapped

// Internal encoder mode (radio-toggled by Brightness / Colour Temp buttons)
static int  s_encoder_mode = LIGHT_ENC_BRIGHTNESS;

// Pending command (JSON payload for MQTT /set).  Written by button/wheel callbacks,
// consumed by main.cpp via light_screen_take_cmd().
static char s_pending_cmd[192] = "";

// Cached state for button colour updates
static bool s_last_on  = false;
static int  s_last_bri = 127;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void update_mode_buttons() {
    if (!s_btn_bri || !s_btn_ct) return;
    lv_obj_set_style_bg_color(s_btn_bri,
        (s_encoder_mode == LIGHT_ENC_BRIGHTNESS)
            ? lv_color_hex(0x003050) : lv_color_hex(0x252525), 0);
    lv_obj_set_style_bg_color(s_btn_ct,
        (s_encoder_mode == LIGHT_ENC_COLORTEMP)
            ? lv_color_hex(0x003050) : lv_color_hex(0x252525), 0);
}

static void update_centre_label(int brightness, int colortemp) {
    if (!s_centre_lbl) return;
    if (s_encoder_mode == LIGHT_ENC_BRIGHTNESS) {
        int pct = (int)((brightness * 100) / 254);
        lv_label_set_text_fmt(s_centre_lbl, "%d%%", pct);
    } else if (s_encoder_mode == LIGHT_ENC_COLORTEMP) {
        int kelvin = (colortemp > 0) ? (int)(roundf(1000000.0f / colortemp / 100.0f) * 100) : 0;
        lv_label_set_text_fmt(s_centre_lbl, "%dK", kelvin);
    } else {
        lv_label_set_text(s_centre_lbl, "Office\nLight");
    }
}

static void update_popup_bri_label(int brightness) {
    if (!s_cp_bri_lbl) return;
    int pct = (int)((brightness * 100) / 254);
    lv_label_set_text_fmt(s_cp_bri_lbl, "Brightness: %d%%", pct);
}

// ---------------------------------------------------------------------------
// Button callbacks (Core 1)
// ---------------------------------------------------------------------------

static void btn_bri_mode_cb(lv_event_t *e) {
    s_encoder_mode = (s_encoder_mode == LIGHT_ENC_BRIGHTNESS)
                     ? LIGHT_ENC_NONE : LIGHT_ENC_BRIGHTNESS;
    update_mode_buttons();
    update_centre_label(s_last_bri, 370);  // colourtemp arg unused unless COLORTEMP mode
}

static void btn_ct_mode_cb(lv_event_t *e) {
    s_encoder_mode = (s_encoder_mode == LIGHT_ENC_COLORTEMP)
                     ? LIGHT_ENC_NONE : LIGHT_ENC_COLORTEMP;
    update_mode_buttons();
}

static void btn_pwr_cb(lv_event_t *e) {
    strncpy(s_pending_cmd, "{\"state\":\"TOGGLE\"}", sizeof(s_pending_cmd) - 1);
}

static void btn_cp_cb(lv_event_t *e) {
    if (!s_cp_overlay) return;
    s_cp_open = !s_cp_open;
    if (s_cp_open) {
        s_cp_prev_enc = s_encoder_mode;
        lv_obj_clear_flag(s_cp_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_cp_overlay);
        update_popup_bri_label(s_last_bri);
    } else {
        lv_obj_add_flag(s_cp_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void btn_cp_done_cb(lv_event_t *e) {
    s_cp_open = false;
    s_encoder_mode = s_cp_prev_enc;
    lv_obj_add_flag(s_cp_overlay, LV_OBJ_FLAG_HIDDEN);
    update_mode_buttons();
}

static void colorwheel_cb(lv_event_t *e) {
    if (!s_cp_wheel) return;
    lv_color_hsv_t hsv = lv_colorwheel_get_hsv(s_cp_wheel);
    snprintf(s_pending_cmd, sizeof(s_pending_cmd),
             "{\"color\":{\"hue\":%d,\"saturation\":%d}}",
             (int)hsv.h, (int)hsv.s);
}

// ---------------------------------------------------------------------------
// light_screen_create
// ---------------------------------------------------------------------------

void light_screen_create() {
    // ---- Screen ----
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    // Helper: create a round icon-only button
    auto make_btn = [&](lv_align_t align, int x_ofs, int y_ofs,
                        const char *icon_text, lv_event_cb_t cb,
                        lv_obj_t **btn_out, lv_obj_t **icon_out) {
        lv_obj_t *btn = lv_btn_create(s_screen);
        lv_obj_set_size(btn, 60, 60);
        lv_obj_align(btn, align, x_ofs, y_ofs);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x252525), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x383838), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
        if (btn_out) *btn_out = btn;

        lv_obj_t *icon = lv_label_create(btn);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(0xAAAAAA), 0);
        lv_label_set_text(icon, icon_text);
        lv_obj_center(icon);
        if (icon_out) *icon_out = icon;
    };

    // Brightness mode button — left  (80, 180) → CENTER (-100, 0)
    make_btn(LV_ALIGN_CENTER, -100, 0,
             LV_SYMBOL_IMAGE, btn_bri_mode_cb, &s_btn_bri, NULL);

    // Colour Temp mode button — right (280, 180) → CENTER (+100, 0)
    make_btn(LV_ALIGN_CENTER, +100, 0,
             LV_SYMBOL_SETTINGS, btn_ct_mode_cb, &s_btn_ct, NULL);

    // Colour Picker button — top   (180, 80)  → CENTER (0, -100)
    make_btn(LV_ALIGN_CENTER, 0, -100,
             LV_SYMBOL_EDIT, btn_cp_cb, &s_btn_cp, NULL);

    // Power Toggle button — bottom (180, 280) → CENTER (0, +100)
    make_btn(LV_ALIGN_CENTER, 0, +100,
             LV_SYMBOL_POWER, btn_pwr_cb, &s_btn_pwr, &s_pwr_icon);

    // Centre label — shows brightness%, kelvin, or "Office Light"
    s_centre_lbl = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_centre_lbl, &lv_font_montserrat_30, 0);
    lv_obj_set_style_text_color(s_centre_lbl, lv_color_hex(0x00BFFF), 0);
    lv_obj_set_style_text_align(s_centre_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_centre_lbl, "Office\nLight");
    lv_obj_align(s_centre_lbl, LV_ALIGN_CENTER, 0, 0);

    // Pre-light the brightness button (default encoder mode on screen entry)
    update_mode_buttons();

    // ---- Colour picker popup (hidden until Colour Picker button tapped) ----
    s_cp_overlay = lv_obj_create(s_screen);
    lv_obj_set_size(s_cp_overlay, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(s_cp_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_cp_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_cp_overlay, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_cp_overlay, 0, 0);
    lv_obj_set_style_radius(s_cp_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_cp_overlay, 0, 0);
    lv_obj_clear_flag(s_cp_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_cp_overlay, LV_OBJ_FLAG_HIDDEN);

    // Brightness label at top of popup
    s_cp_bri_lbl = lv_label_create(s_cp_overlay);
    lv_obj_set_style_text_font(s_cp_bri_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_cp_bri_lbl, lv_color_hex(0xCCCCCC), 0);
    lv_label_set_text(s_cp_bri_lbl, "Brightness: --");
    lv_obj_align(s_cp_bri_lbl, LV_ALIGN_TOP_MID, 0, 15);

    // Colorwheel — 240×240, shifted up slightly to make room for Done button
    s_cp_wheel = lv_colorwheel_create(s_cp_overlay, true);
    lv_obj_set_size(s_cp_wheel, 240, 240);
    lv_obj_align(s_cp_wheel, LV_ALIGN_CENTER, 0, -10);
    lv_obj_add_event_cb(s_cp_wheel, colorwheel_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // "Done" button — closes popup, restores encoder mode
    s_cp_done = lv_btn_create(s_cp_overlay);
    lv_obj_set_size(s_cp_done, 100, 36);
    lv_obj_align(s_cp_done, LV_ALIGN_CENTER, 0, 140);
    lv_obj_set_style_bg_color(s_cp_done, lv_color_hex(0x003050), 0);
    lv_obj_set_style_bg_color(s_cp_done, lv_color_hex(0x004A70), LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_cp_done, 8, 0);
    lv_obj_set_style_border_width(s_cp_done, 0, 0);
    lv_obj_set_style_shadow_width(s_cp_done, 0, 0);
    lv_obj_add_event_cb(s_cp_done, btn_cp_done_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *done_lbl = lv_label_create(s_cp_done);
    lv_obj_set_style_text_font(done_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(done_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(done_lbl, "Done");
    lv_obj_center(done_lbl);
}

// ---------------------------------------------------------------------------
// light_screen_update — called from Core 1 on state change or pending target
// ---------------------------------------------------------------------------

void light_screen_update(bool on, int brightness, int colortemp, float hue, float sat) {
    if (!s_screen) return;

    s_last_on  = on;
    s_last_bri = brightness;

    // Power button: green tint when on, grey when off
    if (s_pwr_icon) {
        lv_obj_set_style_bg_color(s_btn_pwr,
            on ? lv_color_hex(0x1A3A1A) : lv_color_hex(0x252525), 0);
        lv_obj_set_style_text_color(s_pwr_icon,
            on ? lv_color_hex(0x44EE44) : lv_color_hex(0x666666), 0);
    }

    // Centre label
    update_centre_label(brightness, colortemp);

    // If colorpicker popup is open, keep its brightness label in sync
    if (s_cp_open) {
        update_popup_bri_label(brightness);
    }

    // Sync colorwheel to current hue/sat (only if popup is visible and hue is non-zero)
    if (s_cp_open && s_cp_wheel && (hue > 0.0f || sat > 0.0f)) {
        lv_color_hsv_t hsv = lv_colorwheel_get_hsv(s_cp_wheel);
        hsv.h = (uint16_t)hue;
        hsv.s = (uint8_t)sat;
        lv_colorwheel_set_hsv(s_cp_wheel, hsv);
    }
}

// ---------------------------------------------------------------------------
// light_screen_take_cmd
// ---------------------------------------------------------------------------

const char *light_screen_take_cmd() {
    if (s_pending_cmd[0] == '\0') return nullptr;
    static char out[192];
    strncpy(out, s_pending_cmd, sizeof(out) - 1);
    out[sizeof(out) - 1] = '\0';
    s_pending_cmd[0] = '\0';
    return out;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

lv_obj_t *light_screen_get_obj() { return s_screen; }

int light_screen_get_encoder_mode() {
    if (s_cp_open) return LIGHT_ENC_BRIGHTNESS;
    return s_encoder_mode;
}

bool light_screen_is_colorpicker_open() { return s_cp_open; }
