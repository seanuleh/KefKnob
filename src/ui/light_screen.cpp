#include "light_screen.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <esp_heap_caps.h>

// ---------------------------------------------------------------------------
// Widget handles
// ---------------------------------------------------------------------------

static lv_obj_t *s_screen     = NULL;
static lv_obj_t *s_btn_bri    = NULL;
static lv_obj_t *s_btn_ct     = NULL;
static lv_obj_t *s_btn_pwr    = NULL;
static lv_obj_t *s_btn_cp     = NULL;
static lv_obj_t *s_pwr_icon   = NULL;

// Arc slider
static lv_obj_t  *s_arc_outline    = NULL;
static lv_obj_t  *s_arc_bri        = NULL;
static lv_obj_t  *s_arc_ct_segs[24]= {};
static lv_obj_t  *s_arc_ct         = NULL;

// Arc value label
static lv_obj_t *s_arc_val_lbl    = NULL;
static lv_obj_t *s_arc_val_shd[4] = {};

// Colour picker popup
static lv_obj_t  *s_cp_overlay   = NULL;  // full-screen container
static lv_obj_t  *s_cp_canvas    = NULL;  // pre-rendered colour disc
static lv_obj_t  *s_cp_indicator = NULL;  // circle showing selected colour
static lv_obj_t  *s_cp_done      = NULL;
static uint16_t  *s_cp_buf       = NULL;  // PSRAM buffer for disc canvas
static bool       s_cp_open      = false;
static int        s_cp_prev_enc  = LIGHT_ENC_BRIGHTNESS;

static int   s_encoder_mode = LIGHT_ENC_BRIGHTNESS;
static char  s_pending_cmd[192] = "";

static bool  s_last_on  = false;
static int   s_last_bri = 127;
static int   s_last_ct  = 370;
static float s_last_hue = 0.0f;
static float s_last_sat = 0.0f;

// Colour disc radius in pixels (canvas is 360×360, disc centred at 180,180)
static constexpr int kDiscRadius = 170;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static lv_color_t mired_to_color(int mired) {
    float t = (float)(mired - LIGHT_COLORTEMP_MIN) /
              (float)(LIGHT_COLORTEMP_MAX - LIGHT_COLORTEMP_MIN);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    static const uint8_t stops[4][3] = {
        {168, 196, 255},
        {255, 232, 200},
        {255, 200, 100},
        {255, 120,   0},
    };
    float s = t * 3.0f;
    int   i = (int)s; if (i > 2) i = 2;
    float f = s - (float)i;
    return lv_color_make(
        (uint8_t)(stops[i][0] + f * (int)(stops[i+1][0] - stops[i][0])),
        (uint8_t)(stops[i][1] + f * (int)(stops[i+1][1] - stops[i][1])),
        (uint8_t)(stops[i][2] + f * (int)(stops[i+1][2] - stops[i][2])));
}

// Render a HSV colour disc into a 360×360 RGB565 buffer.
// Hue = angle around the circle (atan2), Saturation = distance from centre.
static void render_color_disc(uint16_t *buf) {
    const float cx = 180.0f, cy = 180.0f;
    const float r_max = (float)kDiscRadius;
    const float r_max2 = r_max * r_max;
    const lv_color_t bg = lv_color_hex(0x0A0A0A);

    for (int y = 0; y < 360; y++) {
        float dy = (float)y - cy;
        for (int x = 0; x < 360; x++) {
            float dx = (float)x - cx;
            float r2 = dx*dx + dy*dy;
            if (r2 > r_max2) {
                buf[y * 360 + x] = bg.full;
            } else {
                float r     = sqrtf(r2);
                float angle = atan2f(dy, dx) * (180.0f / 3.14159265f);
                if (angle < 0.0f) angle += 360.0f;
                uint8_t sat = (uint8_t)(r / r_max * 100.0f + 0.5f);
                buf[y * 360 + x] = lv_color_hsv_to_rgb((uint16_t)angle, sat, 100).full;
            }
        }
    }
}

static void update_mode_buttons() {
    if (!s_btn_bri || !s_btn_ct) return;
    lv_obj_set_style_bg_color(s_btn_bri,
        s_encoder_mode == LIGHT_ENC_BRIGHTNESS
            ? lv_color_hex(0x003050) : lv_color_hex(0x252525), 0);
    lv_obj_set_style_bg_color(s_btn_ct,
        s_encoder_mode == LIGHT_ENC_COLORTEMP
            ? lv_color_hex(0x003050) : lv_color_hex(0x252525), 0);
}

static void update_arc_label(int bri, int ct) {
    if (!s_arc_val_lbl) return;
    char buf[10];
    lv_color_t col;
    if (s_encoder_mode == LIGHT_ENC_BRIGHTNESS) {
        snprintf(buf, sizeof(buf), "%d%%", (bri * 100 + 127) / 254);
        col = (s_last_hue > 0.0f || s_last_sat > 0.0f)
            ? lv_color_hsv_to_rgb((uint16_t)s_last_hue, (uint8_t)s_last_sat, 100)
            : mired_to_color(ct);
    } else {
        int k = (ct > 0) ? (int)(roundf(1000000.0f / ct / 100.0f) * 100) : 0;
        snprintf(buf, sizeof(buf), "%dK", k);
        col = mired_to_color(ct);
    }
    lv_label_set_text(s_arc_val_lbl, buf);
    lv_obj_set_style_text_color(s_arc_val_lbl, col, 0);
    for (int i = 0; i < 4; i++) lv_label_set_text(s_arc_val_shd[i], buf);
}

static void show_bri_arcs() {
    if (!s_arc_outline) return;
    s_encoder_mode = LIGHT_ENC_BRIGHTNESS;
    update_mode_buttons();
    lv_obj_clear_flag(s_arc_outline, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_arc_bri,     LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 24; i++) lv_obj_add_flag(s_arc_ct_segs[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_arc_ct, LV_OBJ_FLAG_HIDDEN);
}

static void show_ct_arcs() {
    if (!s_arc_outline) return;
    s_encoder_mode = LIGHT_ENC_COLORTEMP;
    update_mode_buttons();
    lv_obj_clear_flag(s_arc_outline, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_arc_bri, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 24; i++) lv_obj_clear_flag(s_arc_ct_segs[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_arc_ct, LV_OBJ_FLAG_HIDDEN);
}

// Position the indicator circle at polar coords (r, angle_deg) on the disc.
static void cp_set_indicator(float r, float angle_deg) {
    if (!s_cp_indicator) return;
    float rad = angle_deg * (3.14159265f / 180.0f);
    int ix = (int)(180.0f + r * cosf(rad)) - 10;
    int iy = (int)(180.0f + r * sinf(rad)) - 10;
    lv_obj_set_pos(s_cp_indicator, ix, iy);
}

// ---------------------------------------------------------------------------
// Button / touch callbacks
// ---------------------------------------------------------------------------

static void btn_bri_mode_cb(lv_event_t *e) {
    show_bri_arcs();
    update_arc_label(s_last_bri, s_last_ct);
}

static void btn_ct_mode_cb(lv_event_t *e) {
    show_ct_arcs();
    update_arc_label(s_last_bri, s_last_ct);
}

static void btn_pwr_cb(lv_event_t *e) {
    strncpy(s_pending_cmd, "{\"state\":\"TOGGLE\"}", sizeof(s_pending_cmd) - 1);
}

static void cp_touch_cb(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    float dx = (float)pt.x - 180.0f;
    float dy = (float)pt.y - 180.0f;
    float r  = sqrtf(dx*dx + dy*dy);
    if (r > (float)kDiscRadius) r = (float)kDiscRadius;

    float angle = atan2f(dy, dx) * (180.0f / 3.14159265f);
    if (angle < 0.0f) angle += 360.0f;
    uint8_t sat = (uint8_t)(r / (float)kDiscRadius * 100.0f + 0.5f);

    snprintf(s_pending_cmd, sizeof(s_pending_cmd),
             "{\"color\":{\"hue\":%d,\"saturation\":%d}}", (int)angle, sat);

    cp_set_indicator(r, angle);
}

static void btn_cp_cb(lv_event_t *e) {
    if (!s_cp_overlay) return;
    s_cp_open = !s_cp_open;
    if (s_cp_open) {
        s_cp_prev_enc = s_encoder_mode;
        // Position indicator at current hue/sat (centre if unknown)
        if (s_last_hue > 0.0f || s_last_sat > 0.0f) {
            float r = s_last_sat / 100.0f * (float)kDiscRadius;
            cp_set_indicator(r, s_last_hue);
        } else {
            lv_obj_set_pos(s_cp_indicator, 170, 170);  // centre
        }
        lv_obj_clear_flag(s_cp_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_cp_overlay);
    } else {
        lv_obj_add_flag(s_cp_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void btn_cp_done_cb(lv_event_t *e) {
    s_cp_open = false;
    lv_obj_add_flag(s_cp_overlay, LV_OBJ_FLAG_HIDDEN);
    // Restore mode before calling show_*_arcs, which sets s_encoder_mode itself.
    // Use s_cp_prev_enc to decide which arc to restore to.
    if (s_cp_prev_enc == LIGHT_ENC_COLORTEMP) show_ct_arcs();
    else                                       show_bri_arcs();
    update_arc_label(s_last_bri, s_last_ct);
}

// ---------------------------------------------------------------------------
// light_screen_create
// ---------------------------------------------------------------------------

void light_screen_create() {
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Arc outline ----
    s_arc_outline = lv_arc_create(s_screen);
    lv_obj_set_size(s_arc_outline, 286, 286);
    lv_obj_align(s_arc_outline, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_bg_angles(s_arc_outline, 150, 30);
    lv_arc_set_range(s_arc_outline, LIGHT_BRIGHTNESS_MIN, LIGHT_BRIGHTNESS_MAX);
    lv_arc_set_value(s_arc_outline, s_last_bri);
    lv_obj_set_style_arc_color(s_arc_outline, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc_outline, 24, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc_outline, lv_color_hex(0x000000), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_arc_outline, 24, LV_PART_INDICATOR);
    lv_obj_set_style_opa(s_arc_outline, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(s_arc_outline, LV_OBJ_FLAG_CLICKABLE);

    // ---- Brightness arc ----
    s_arc_bri = lv_arc_create(s_screen);
    lv_obj_set_size(s_arc_bri, 280, 280);
    lv_obj_align(s_arc_bri, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_bg_angles(s_arc_bri, 150, 30);
    lv_arc_set_range(s_arc_bri, LIGHT_BRIGHTNESS_MIN, LIGHT_BRIGHTNESS_MAX);
    lv_arc_set_value(s_arc_bri, s_last_bri);
    lv_obj_set_style_arc_color(s_arc_bri, lv_color_hex(0x1E1E1E), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc_bri, 18, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc_bri, mired_to_color(s_last_ct), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_arc_bri, 18, LV_PART_INDICATOR);
    lv_obj_set_style_opa(s_arc_bri, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(s_arc_bri, LV_OBJ_FLAG_CLICKABLE);

    // ---- CT gradient segments (24 × 10°) ----
    for (int i = 0; i < 24; i++) {
        uint16_t seg_start = (uint16_t)((150 + i * 10) % 360);
        uint16_t seg_end   = (uint16_t)((150 + (i + 1) * 10) % 360);
        float    mid_mired = LIGHT_COLORTEMP_MIN +
                             (i + 0.5f) / 24.0f * (LIGHT_COLORTEMP_MAX - LIGHT_COLORTEMP_MIN);
        lv_obj_t *seg = lv_arc_create(s_screen);
        lv_obj_set_size(seg, 280, 280);
        lv_obj_align(seg, LV_ALIGN_CENTER, 0, 0);
        lv_arc_set_bg_angles(seg, seg_start, seg_end);
        lv_arc_set_range(seg, 0, 1);
        lv_arc_set_value(seg, 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, 0);
        lv_obj_set_style_arc_color(seg, mired_to_color((int)mid_mired), LV_PART_MAIN);
        lv_obj_set_style_arc_width(seg, 18, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(seg, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_clear_flag(seg, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(seg, LV_OBJ_FLAG_HIDDEN);
        s_arc_ct_segs[i] = seg;
    }

    // ---- CT control arc (knob only) ----
    s_arc_ct = lv_arc_create(s_screen);
    lv_obj_set_size(s_arc_ct, 280, 280);
    lv_obj_align(s_arc_ct, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_bg_angles(s_arc_ct, 150, 30);
    lv_arc_set_range(s_arc_ct, LIGHT_COLORTEMP_MIN, LIGHT_COLORTEMP_MAX);
    lv_arc_set_value(s_arc_ct, s_last_ct);
    lv_obj_set_style_bg_opa(s_arc_ct, LV_OPA_TRANSP, 0);
    lv_obj_set_style_arc_width(s_arc_ct, 18, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc_ct, 18, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_arc_ct, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_arc_ct, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_arc_ct, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_arc_ct, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_arc_ct, 5, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(s_arc_ct, lv_color_hex(0x000000), LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s_arc_ct, 8, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(s_arc_ct, LV_OPA_60, LV_PART_KNOB);
    lv_obj_clear_flag(s_arc_ct, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_arc_ct, LV_OBJ_FLAG_HIDDEN);

    // ---- Arc value label (Montserrat 20, y=-105) ----
    static const int8_t kShd[4][2] = {{0,-2},{0,2},{-2,0},{2,0}};
    for (int i = 0; i < 4; i++) {
        s_arc_val_shd[i] = lv_label_create(s_screen);
        lv_obj_set_style_text_font(s_arc_val_shd[i], &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_arc_val_shd[i], lv_color_hex(0x000000), 0);
        lv_label_set_text(s_arc_val_shd[i], "50%");
        lv_obj_align(s_arc_val_shd[i], LV_ALIGN_CENTER, kShd[i][0], -105 + kShd[i][1]);
    }
    s_arc_val_lbl = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_arc_val_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_arc_val_lbl, lv_color_hex(0x00BFFF), 0);
    lv_label_set_text(s_arc_val_lbl, "50%");
    lv_obj_align(s_arc_val_lbl, LV_ALIGN_CENTER, 0, -105);

    // ---- Four buttons (2×2 grid, 76×76, centres at ±45) ----
    auto make_btn = [&](lv_align_t align, int x_ofs, int y_ofs,
                        const char *icon_text, lv_event_cb_t cb,
                        lv_obj_t **btn_out, lv_obj_t **icon_out) {
        lv_obj_t *btn = lv_btn_create(s_screen);
        lv_obj_set_size(btn, 76, 76);
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

    make_btn(LV_ALIGN_CENTER, -45, -45, LV_SYMBOL_POWER,  btn_pwr_cb,      &s_btn_pwr, &s_pwr_icon);
    make_btn(LV_ALIGN_CENTER, +45, -45, LV_SYMBOL_EDIT,   btn_cp_cb,       &s_btn_cp,  NULL);
    make_btn(LV_ALIGN_CENTER, -45, +45, LV_SYMBOL_CHARGE, btn_bri_mode_cb, &s_btn_bri, NULL);
    make_btn(LV_ALIGN_CENTER, +45, +45, LV_SYMBOL_TINT,   btn_ct_mode_cb,  &s_btn_ct,  NULL);

    show_bri_arcs();
    update_arc_label(s_last_bri, s_last_ct);

    // ---- Colour picker popup ----
    // Full-screen overlay: canvas with pre-rendered HSV disc + touch handler + indicator.
    s_cp_overlay = lv_obj_create(s_screen);
    lv_obj_set_size(s_cp_overlay, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(s_cp_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_cp_overlay, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_bg_opa(s_cp_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_cp_overlay, 0, 0);
    lv_obj_set_style_radius(s_cp_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_cp_overlay, 0, 0);
    lv_obj_clear_flag(s_cp_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_cp_overlay, LV_OBJ_FLAG_HIDDEN);
    // Touch handler on the overlay itself — fires while finger is down
    lv_obj_add_flag(s_cp_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_cp_overlay, cp_touch_cb, LV_EVENT_PRESSING, NULL);

    // Pre-render the colour disc into PSRAM, then bind to a canvas widget.
    s_cp_buf = (uint16_t *)heap_caps_malloc(360 * 360 * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (s_cp_buf) {
        render_color_disc(s_cp_buf);
        s_cp_canvas = lv_canvas_create(s_cp_overlay);
        lv_canvas_set_buffer(s_cp_canvas, s_cp_buf, 360, 360, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_size(s_cp_canvas, 360, 360);
        lv_obj_set_pos(s_cp_canvas, 0, 0);
        lv_obj_clear_flag(s_cp_canvas, LV_OBJ_FLAG_CLICKABLE);  // touch goes to overlay
    }

    // Indicator: transparent circle with white border, positioned by touch/open
    s_cp_indicator = lv_obj_create(s_cp_overlay);
    lv_obj_set_size(s_cp_indicator, 20, 20);
    lv_obj_set_style_radius(s_cp_indicator, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_cp_indicator, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_cp_indicator, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(s_cp_indicator, 2, 0);
    lv_obj_set_style_shadow_color(s_cp_indicator, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_width(s_cp_indicator, 6, 0);
    lv_obj_set_style_shadow_opa(s_cp_indicator, LV_OPA_80, 0);
    lv_obj_clear_flag(s_cp_indicator, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_cp_indicator, 170, 170);  // default: centre

    // Done button — sits at bottom of disc
    s_cp_done = lv_btn_create(s_cp_overlay);
    lv_obj_set_size(s_cp_done, 80, 80);
    lv_obj_align(s_cp_done, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_cp_done, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_cp_done, LV_OPA_70, 0);
    lv_obj_set_style_radius(s_cp_done, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(s_cp_done, lv_color_hex(0x666666), 0);
    lv_obj_set_style_border_width(s_cp_done, 2, 0);
    lv_obj_set_style_shadow_width(s_cp_done, 0, 0);
    lv_obj_add_event_cb(s_cp_done, btn_cp_done_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *done_lbl = lv_label_create(s_cp_done);
    lv_obj_set_style_text_font(done_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(done_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(done_lbl, "Done");
    lv_obj_center(done_lbl);
}

// ---------------------------------------------------------------------------
// light_screen_update
// ---------------------------------------------------------------------------

void light_screen_update(bool on, int brightness, int colortemp, float hue, float sat) {
    if (!s_screen) return;

    s_last_on  = on;
    s_last_bri = brightness;
    s_last_ct  = colortemp;
    s_last_hue = hue;
    s_last_sat = sat;

    if (s_pwr_icon) {
        lv_obj_set_style_bg_color(s_btn_pwr,
            on ? lv_color_hex(0x1A3A1A) : lv_color_hex(0x252525), 0);
        lv_obj_set_style_text_color(s_pwr_icon,
            on ? lv_color_hex(0x44EE44) : lv_color_hex(0x666666), 0);
    }

    if (s_arc_bri) {
        lv_arc_set_value(s_arc_bri, brightness);
        lv_arc_set_value(s_arc_outline, brightness);
        lv_color_t col = (hue > 0.0f || sat > 0.0f)
            ? lv_color_hsv_to_rgb((uint16_t)hue, (uint8_t)sat, 100)
            : mired_to_color(colortemp);
        lv_obj_set_style_arc_color(s_arc_bri, col, LV_PART_INDICATOR);
    }

    if (s_arc_ct) {
        lv_arc_set_value(s_arc_ct, colortemp);
    }

    update_arc_label(brightness, colortemp);
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
