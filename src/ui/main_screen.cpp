#include "main_screen.h"
#include "config.h"

#include <TJpg_Decoder.h>

// ---------------------------------------------------------------------------
// Widget handles (file-local)
// ---------------------------------------------------------------------------

static lv_obj_t *s_screen       = NULL;
static lv_obj_t *s_art_canvas   = NULL;   // full-screen background art
static lv_obj_t *s_arc_outline     = NULL;  // slightly larger arc, black — border effect
static lv_obj_t *s_arc             = NULL;
static lv_obj_t *s_vol_shadow[4]   = {};   // outline: 4 cardinal-offset black labels
static lv_obj_t *s_vol_label       = NULL;
static lv_obj_t *s_title_label      = NULL;
static lv_obj_t *s_artist_label     = NULL;
static lv_obj_t *s_progress_outline = NULL;  // black border bar behind progress
static lv_obj_t *s_progress_bar     = NULL;  // Spotify-green fill bar

// Bottom playback control buttons (created on s_screen, below overlays)
static lv_obj_t *s_btn_mute      = NULL;  // USB source: mute toggle (center)
static lv_obj_t *s_btn_mute_icon = NULL;
static lv_obj_t *s_btn_play      = NULL;  // WiFi source: play/pause (center)
static lv_obj_t *s_btn_pp_icon   = NULL;
static lv_obj_t *s_btn_prev      = NULL;  // WiFi source: previous track
static lv_obj_t *s_btn_next      = NULL;  // WiFi source: next track

// Cached playback state (used by button callbacks on Core 1)
static bool s_is_playing = false;
static bool s_is_muted   = false;

// Control panel (swipe-from-top overlay)
static lv_obj_t *s_ctrl_panel      = NULL;
static lv_obj_t *s_btn_power       = NULL;
static lv_obj_t *s_btn_power_icon  = NULL;
static lv_obj_t *s_btn_power_label = NULL;
static lv_obj_t *s_btn_wifi        = NULL;
static lv_obj_t *s_btn_usb         = NULL;
static bool      s_ctrl_visible    = false;
static char      s_pending_cmd[16]       = "";
static char      s_pending_track_cmd[12] = "";

// Standby screen (full-screen overlay when speaker is off)
static lv_obj_t *s_standby_panel   = NULL;
static bool      s_standby_visible = false;

// PSRAM-backed RGB565 buffer for album art (360×360×2 = 259 200 bytes)
static uint16_t *s_art_buf = NULL;

// ---------------------------------------------------------------------------
// TJpgDec callback — center-crops the 640×640 JPEG into s_art_buf (360×360)
//
// Spotify delivers 640×640 JPEGs. TJpgDec at scale=1 decodes MCU blocks
// (16×16 px each) and passes each block to this callback. We keep only the
// center 360×360 region:
//   offset = (ALBUM_ART_JPEG_SRC - ALBUM_ART_SIZE) / 2 = 140 px
// ---------------------------------------------------------------------------

static bool art_decode_cb(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
    if (!s_art_buf) return false;

    constexpr int CROP = (ALBUM_ART_JPEG_SRC - ALBUM_ART_SIZE) / 2; // = 140

    for (int row = 0; row < (int)h; row++) {
        int dst_y = (y + row) - CROP;
        if (dst_y < 0 || dst_y >= ALBUM_ART_SIZE) continue;

        int src_left  = x;
        int src_right = x + (int)w;   // exclusive
        int cl = (src_left  < CROP)               ? CROP               : src_left;
        int cr = (src_right > CROP + ALBUM_ART_SIZE) ? CROP + ALBUM_ART_SIZE : src_right;
        int copy_len = cr - cl;
        if (copy_len <= 0) continue;

        int dst_x   = cl - CROP;
        int src_off = cl - src_left;
        memcpy(&s_art_buf[dst_y * ALBUM_ART_SIZE + dst_x],
               &bitmap[row * w + src_off],
               copy_len * sizeof(uint16_t));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Control panel button callbacks
// ---------------------------------------------------------------------------

static void btn_pwr_wifi_cb(lv_event_t *e) {
    DEBUG_PRINTLN("[UI] Standby WiFi button tapped");
    strncpy(s_pending_cmd, "pwr_wifi", sizeof(s_pending_cmd) - 1);
}

static void btn_pwr_usb_cb(lv_event_t *e) {
    DEBUG_PRINTLN("[UI] Standby USB button tapped");
    strncpy(s_pending_cmd, "pwr_usb", sizeof(s_pending_cmd) - 1);
}

static void btn_power_cb(lv_event_t *e) {
    strncpy(s_pending_cmd, "power", sizeof(s_pending_cmd) - 1);
    main_screen_toggle_control_panel();
}

static void btn_wifi_cb(lv_event_t *e) {
    strncpy(s_pending_cmd, "src_wifi", sizeof(s_pending_cmd) - 1);
    main_screen_toggle_control_panel();
}

static void btn_usb_cb(lv_event_t *e) {
    strncpy(s_pending_cmd, "src_usb", sizeof(s_pending_cmd) - 1);
    main_screen_toggle_control_panel();
}

static void btn_mute_cb(lv_event_t *e) {
    strncpy(s_pending_track_cmd, s_is_muted ? "unmute" : "mute",
            sizeof(s_pending_track_cmd) - 1);
}

static void btn_play_cb(lv_event_t *e) {
    strncpy(s_pending_track_cmd, "pause", sizeof(s_pending_track_cmd) - 1);
}

static void btn_prev_cb(lv_event_t *e) {
    strncpy(s_pending_track_cmd, "previous", sizeof(s_pending_track_cmd) - 1);
}

static void btn_next_cb(lv_event_t *e) {
    strncpy(s_pending_track_cmd, "next", sizeof(s_pending_track_cmd) - 1);
}

// ---------------------------------------------------------------------------
// main_screen_create
//
// Layer order (back → front):
//   [1] art canvas 360×360 (background, hidden until art arrives)
//   [2] arc 280×280
//   [3] vol label
//   [4] title + artist labels
//   [5] bottom playback buttons (mute / prev+play+next)
//   [6] control panel overlay (hidden until swipe-from-top)
//
// Layout:
//   CENTER,   0, -20  → vol number   (Montserrat 30)
//   CENTER,   0, +20  → title        (Montserrat 20, scroll)
//   CENTER,   0, +50  → artist       (Montserrat 16, scroll)
//   CENTER,   0, +100 → mute / play-pause button  (56×56 round)
//   CENTER, ±80, +100 → prev / next buttons        (48×48 round, WiFi only)
// ---------------------------------------------------------------------------

void main_screen_create() {
    // ---- Screen ----
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    // ---- [1] Background art canvas (full screen, initially hidden) ----
    s_art_buf = (uint16_t *)heap_caps_malloc(
        ALBUM_ART_SIZE * ALBUM_ART_SIZE * sizeof(uint16_t), MALLOC_CAP_SPIRAM);

    s_art_canvas = lv_canvas_create(s_screen);
    if (s_art_buf) {
        lv_canvas_set_buffer(s_art_canvas, s_art_buf,
                             ALBUM_ART_SIZE, ALBUM_ART_SIZE,
                             LV_IMG_CF_TRUE_COLOR);
        lv_canvas_fill_bg(s_art_canvas, lv_color_hex(0x0A0A0A), LV_OPA_COVER);
    }
    lv_obj_set_size(s_art_canvas, ALBUM_ART_SIZE, ALBUM_ART_SIZE);
    lv_obj_set_pos(s_art_canvas, 0, 0);
    lv_obj_add_flag(s_art_canvas, LV_OBJ_FLAG_HIDDEN);

    // ---- [2] Arc outline — 3 px larger radius, 6 px wider track, pure black
    //          Drawn first so it sits behind the coloured arc.
    s_arc_outline = lv_arc_create(s_screen);
    lv_obj_set_size(s_arc_outline, 286, 286);
    lv_obj_align(s_arc_outline, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_bg_angles(s_arc_outline, 150, 30);
    lv_arc_set_range(s_arc_outline, VOLUME_MIN, VOLUME_MAX);
    lv_arc_set_value(s_arc_outline, 50);
    lv_obj_set_style_arc_color(s_arc_outline, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc_outline, 24, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc_outline, lv_color_hex(0x000000), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_arc_outline, 24, LV_PART_INDICATOR);
    lv_obj_set_style_opa(s_arc_outline, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(s_arc_outline, LV_OBJ_FLAG_CLICKABLE);

    // ---- [2] Volume arc (240° sweep) ----
    s_arc = lv_arc_create(s_screen);
    lv_obj_set_size(s_arc, 280, 280);
    lv_obj_align(s_arc, LV_ALIGN_CENTER, 0, 0);

    lv_arc_set_bg_angles(s_arc, 150, 30);
    lv_arc_set_range(s_arc, VOLUME_MIN, VOLUME_MAX);
    lv_arc_set_value(s_arc, 50);

    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x1E1E1E), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 18, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x00BFFF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_arc, 18, LV_PART_INDICATOR);
    lv_obj_set_style_opa(s_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);

    // ---- [3] Volume label — white text with black outline ----
    // LVGL 8 has no text stroke, so we layer four black labels at ±2 px
    // (N/S/E/W) behind one white label to produce a clean text outline.
    static const int8_t kShadowOff[4][2] = {{0,-2},{0,2},{-2,0},{2,0}};
    for (int i = 0; i < 4; i++) {
        s_vol_shadow[i] = lv_label_create(s_screen);
        lv_obj_set_style_text_font(s_vol_shadow[i], &lv_font_montserrat_40, 0);
        lv_obj_set_style_text_color(s_vol_shadow[i], lv_color_hex(0x000000), 0);
        lv_label_set_text(s_vol_shadow[i], "50");
        lv_obj_align(s_vol_shadow[i], LV_ALIGN_CENTER,
                     kShadowOff[i][0], -68 + kShadowOff[i][1]);
    }
    s_vol_label = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_vol_label, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(s_vol_label, lv_color_hex(0x00BFFF), 0);
    lv_label_set_text(s_vol_label, "50");
    lv_obj_align(s_vol_label, LV_ALIGN_CENTER, 0, -68);

    // ---- [4] Track title — scrolls if too long ----
    s_title_label = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_title_label, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_title_label, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(s_title_label, 220, 0);
    lv_label_set_text(s_title_label, "--");
    lv_obj_align(s_title_label, LV_ALIGN_CENTER, 0, -5);
    // Dark pill — high opacity so album art doesn't bleed through
    lv_obj_set_style_bg_color(s_title_label, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_bg_opa(s_title_label, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_title_label, 6, 0);
    lv_obj_set_style_pad_hor(s_title_label, 10, 0);
    lv_obj_set_style_pad_ver(s_title_label, 5, 0);
    lv_obj_set_style_shadow_color(s_title_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_width(s_title_label, 12, 0);
    lv_obj_set_style_shadow_spread(s_title_label, 3, 0);
    lv_obj_set_style_shadow_opa(s_title_label, LV_OPA_60, 0);
    lv_obj_set_style_shadow_ofs_x(s_title_label, 0, 0);
    lv_obj_set_style_shadow_ofs_y(s_title_label, 0, 0);

    // ---- [4] Artist name — scrolls if too long ----
    s_artist_label = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_artist_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_artist_label, lv_color_hex(0xBBCCDD), 0);
    lv_label_set_long_mode(s_artist_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_artist_label, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(s_artist_label, 220, 0);
    lv_label_set_text(s_artist_label, "--");
    lv_obj_align(s_artist_label, LV_ALIGN_CENTER, 0, +20);
    // Dark pill — same high opacity as title, cooler tint to distinguish hierarchy
    lv_obj_set_style_bg_color(s_artist_label, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_bg_opa(s_artist_label, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_artist_label, 6, 0);
    lv_obj_set_style_pad_hor(s_artist_label, 10, 0);
    lv_obj_set_style_pad_ver(s_artist_label, 4, 0);
    lv_obj_set_style_shadow_color(s_artist_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_width(s_artist_label, 12, 0);
    lv_obj_set_style_shadow_spread(s_artist_label, 3, 0);
    lv_obj_set_style_shadow_opa(s_artist_label, LV_OPA_60, 0);
    lv_obj_set_style_shadow_ofs_x(s_artist_label, 0, 0);
    lv_obj_set_style_shadow_ofs_y(s_artist_label, 0, 0);

    // ---- [5] Track progress bar — black outline + Spotify-green fill ----
    // Outline bar (slightly larger, all black) — drawn first so it sits behind.
    s_progress_outline = lv_bar_create(s_screen);
    lv_obj_set_size(s_progress_outline, 206, 16);
    lv_obj_align(s_progress_outline, LV_ALIGN_CENTER, 0, 81);
    lv_bar_set_range(s_progress_outline, 0, 100);
    lv_bar_set_value(s_progress_outline, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(s_progress_outline, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(s_progress_outline, 8, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_progress_outline, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_progress_outline, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_progress_outline, lv_color_hex(0x000000), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_progress_outline, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_clear_flag(s_progress_outline, LV_OBJ_FLAG_CLICKABLE);

    // Main progress bar — centered on the outline, Spotify green fill
    s_progress_bar = lv_bar_create(s_screen);
    lv_obj_set_size(s_progress_bar, 200, 10);
    lv_obj_align(s_progress_bar, LV_ALIGN_CENTER, 0, 81);
    lv_bar_set_range(s_progress_bar, 0, 100);
    lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(s_progress_bar, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(s_progress_bar, 5, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(0x1E1E1E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_progress_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(0x00BFFF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_progress_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_clear_flag(s_progress_bar, LV_OBJ_FLAG_CLICKABLE);

    // ---- [6] Bottom playback buttons ----
    // Round icon-only buttons. Visibility toggled in main_screen_update().
    auto make_round_btn = [&](lv_align_t align, int x_ofs, int y_ofs, int size,
                              const char *icon_text, lv_event_cb_t cb,
                              lv_obj_t **btn_out, lv_obj_t **icon_out) {
        lv_obj_t *btn = lv_btn_create(s_screen);
        lv_obj_set_size(btn, size, size);
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

    // USB source: mute toggle (center, larger to anchor the bottom)
    make_round_btn(LV_ALIGN_CENTER, 0, 126, 68,
                   LV_SYMBOL_VOLUME_MAX, btn_mute_cb,
                   &s_btn_mute, &s_btn_mute_icon);
    lv_obj_set_style_text_color(s_btn_mute_icon, lv_color_hex(0x00BFFF), 0);

    // WiFi source: previous track (left, flanking center)
    make_round_btn(LV_ALIGN_CENTER, -70, 126, 56,
                   LV_SYMBOL_PREV, btn_prev_cb,
                   &s_btn_prev, NULL);

    // WiFi source: play/pause (center, larger to anchor the bottom)
    make_round_btn(LV_ALIGN_CENTER, 0, 126, 68,
                   LV_SYMBOL_PLAY, btn_play_cb,
                   &s_btn_play, &s_btn_pp_icon);
    lv_obj_set_style_text_color(s_btn_pp_icon, lv_color_hex(0x00BFFF), 0);

    // WiFi source: next track (right, flanking center)
    make_round_btn(LV_ALIGN_CENTER, +70, 126, 56,
                   LV_SYMBOL_NEXT, btn_next_cb,
                   &s_btn_next, NULL);

    // All hidden until first state update reveals the correct set
    lv_obj_add_flag(s_btn_mute, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_prev, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_play, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_next, LV_OBJ_FLAG_HIDDEN);

    // ---- [6] Control panel — dark overlay, swipe-from-top to reveal ----
    s_ctrl_panel = lv_obj_create(s_screen);
    lv_obj_set_size(s_ctrl_panel, 260, 130);
    lv_obj_align(s_ctrl_panel, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_bg_color(s_ctrl_panel, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(s_ctrl_panel, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_ctrl_panel, 16, 0);
    lv_obj_set_style_border_color(s_ctrl_panel, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(s_ctrl_panel, 1, 0);
    lv_obj_set_style_pad_all(s_ctrl_panel, 8, 0);
    lv_obj_clear_flag(s_ctrl_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ctrl_panel, LV_OBJ_FLAG_HIDDEN);

    // Panel header
    lv_obj_t *ctrl_title = lv_label_create(s_ctrl_panel);
    lv_obj_set_style_text_font(ctrl_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ctrl_title, lv_color_hex(0x888888), 0);
    lv_label_set_text(ctrl_title, "Controls");
    lv_obj_align(ctrl_title, LV_ALIGN_TOP_MID, 0, 0);

    // Helper: create a control button with icon + text
    auto make_ctrl_btn = [&](lv_align_t align, int x_ofs,
                             const char *icon_text, const char *btn_text,
                             lv_event_cb_t cb,
                             lv_obj_t **btn_out,
                             lv_obj_t **icon_out,
                             lv_obj_t **label_out) {
        lv_obj_t *btn = lv_btn_create(s_ctrl_panel);
        lv_obj_set_size(btn, 72, 60);
        lv_obj_align(btn, align, x_ofs, -4);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A2A2A), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x3A3A3A), LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
        if (btn_out) *btn_out = btn;

        lv_obj_t *icon = lv_label_create(btn);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(0xCCCCCC), 0);
        lv_label_set_text(icon, icon_text);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 4);
        if (icon_out) *icon_out = icon;

        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xAAAAAA), 0);
        lv_label_set_text(lbl, btn_text);
        lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -4);
        if (label_out) *label_out = lbl;
    };

    // Power button (left)
    make_ctrl_btn(LV_ALIGN_BOTTOM_LEFT, 0,
                  LV_SYMBOL_POWER, "ON",
                  btn_power_cb,
                  &s_btn_power, &s_btn_power_icon, &s_btn_power_label);
    lv_obj_set_style_bg_color(s_btn_power, lv_color_hex(0x1A3A1A), 0);
    lv_obj_set_style_text_color(s_btn_power_icon,  lv_color_hex(0x44EE44), 0);
    lv_obj_set_style_text_color(s_btn_power_label, lv_color_hex(0x44EE44), 0);

    // WiFi button (center)
    lv_obj_t *wifi_icon = NULL;
    make_ctrl_btn(LV_ALIGN_BOTTOM_MID, 0,
                  LV_SYMBOL_WIFI, "WiFi",
                  btn_wifi_cb,
                  &s_btn_wifi, &wifi_icon, NULL);

    // USB button (right)
    lv_obj_t *usb_icon = NULL;
    make_ctrl_btn(LV_ALIGN_BOTTOM_RIGHT, 0,
                  LV_SYMBOL_USB, "USB",
                  btn_usb_cb,
                  &s_btn_usb, &usb_icon, NULL);

    // ---- [6] Standby screen — full-screen overlay, shown when speaker is off ----
    s_standby_panel = lv_obj_create(s_screen);
    lv_obj_set_size(s_standby_panel, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(s_standby_panel, 0, 0);
    lv_obj_set_style_bg_color(s_standby_panel, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_bg_opa(s_standby_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_standby_panel, 0, 0);
    lv_obj_set_style_radius(s_standby_panel, 0, 0);
    lv_obj_set_style_pad_all(s_standby_panel, 0, 0);
    lv_obj_clear_flag(s_standby_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_standby_panel, LV_OBJ_FLAG_HIDDEN);   // hidden while on

    // "Standby" header text
    lv_obj_t *sby_label = lv_label_create(s_standby_panel);
    lv_obj_set_style_text_font(sby_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sby_label, lv_color_hex(0x555555), 0);
    lv_label_set_text(sby_label, LV_SYMBOL_POWER "  Standby");
    lv_obj_align(sby_label, LV_ALIGN_TOP_MID, 0, 85);

    // Helper: big standby input button
    auto make_sby_btn = [&](int x_ofs, const char *icon, const char *label_text,
                            lv_color_t bg_col, lv_color_t icon_col,
                            lv_event_cb_t cb) {
        lv_obj_t *btn = lv_btn_create(s_standby_panel);
        lv_obj_set_size(btn, 130, 110);
        lv_obj_align(btn, LV_ALIGN_CENTER, x_ofs, 20);
        lv_obj_set_style_bg_color(btn, bg_col, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x404040), LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 18, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *ico = lv_label_create(btn);
        lv_obj_set_style_text_font(ico, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(ico, icon_col, 0);
        lv_label_set_text(ico, icon);
        lv_obj_align(ico, LV_ALIGN_TOP_MID, 0, 12);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(lbl, label_text);
        lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -12);
    };

    make_sby_btn(-75,
                 LV_SYMBOL_WIFI, "WiFi",
                 lv_color_hex(0x0E1E3A), lv_color_hex(0x4488FF),
                 btn_pwr_wifi_cb);

    make_sby_btn(+75,
                 LV_SYMBOL_USB, "USB",
                 lv_color_hex(0x2A1A06), lv_color_hex(0xFFAA22),
                 btn_pwr_usb_cb);

    lv_scr_load(s_screen);
}

// ---------------------------------------------------------------------------
// main_screen_update
// ---------------------------------------------------------------------------

void main_screen_update(int volume, const char *title,
                        const char *artist, bool is_playing,
                        bool source_is_usb, bool is_muted,
                        bool spotify_active, int progress_pct) {
    if (s_screen == NULL) return;

    // Cache for button callbacks (Core 1 only)
    s_is_playing = is_playing;
    s_is_muted   = is_muted;

    lv_arc_set_value(s_arc, volume);
    lv_arc_set_value(s_arc_outline, volume);
    lv_label_set_text_fmt(s_vol_label, "%d", volume);
    for (int i = 0; i < 4; i++) {
        lv_label_set_text_fmt(s_vol_shadow[i], "%d", volume);
    }

    // Track progress bar — hidden on USB when Spotify is not active
    bool show_progress = !source_is_usb || spotify_active;
    if (show_progress) {
        int pct = (progress_pct < 0) ? 0 : (progress_pct > 100) ? 100 : progress_pct;
        lv_bar_set_value(s_progress_outline, pct, LV_ANIM_OFF);
        lv_bar_set_value(s_progress_bar,     pct, LV_ANIM_OFF);
        lv_obj_clear_flag(s_progress_outline, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_progress_bar,     LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_progress_outline, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_progress_bar,     LV_OBJ_FLAG_HIDDEN);
    }

    // Show correct button set and update state-dependent icons
    if (source_is_usb) {
        if (spotify_active) {
            // Spotify is playing — show prev/play-pause/next
            lv_obj_add_flag(s_btn_mute, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_btn_play, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_btn_prev, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_btn_next, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_btn_pp_icon,
                is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        } else {
            // Nothing playing on Spotify — show mute button only
            lv_obj_clear_flag(s_btn_mute, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_btn_play, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_btn_prev, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_btn_next, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_btn_mute_icon,
                is_muted ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
            lv_obj_set_style_text_color(s_btn_mute_icon,
                is_muted ? lv_color_hex(0xFF6B00) : lv_color_hex(0x00BFFF), 0);
            lv_obj_set_style_bg_color(s_btn_mute,
                is_muted ? lv_color_hex(0x2A1200) : lv_color_hex(0x252525), 0);
        }
    } else {
        lv_obj_add_flag(s_btn_mute, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_btn_play, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_btn_prev, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_btn_next, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_btn_pp_icon,
            is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }

    if (source_is_usb && !spotify_active) {
        lv_label_set_text(s_title_label, "USB");
        lv_obj_add_flag(s_artist_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_title_label, (title && title[0]) ? title : "--");
        bool has_artist = artist && artist[0] && strcmp(artist, "--") != 0;
        if (has_artist) {
            lv_label_set_text(s_artist_label, artist);
            lv_obj_clear_flag(s_artist_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_artist_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ---------------------------------------------------------------------------
// main_screen_update_art
// Called from Core 1. Decodes JPEG via TJpgDec, blits to background canvas.
// Pass nullptr to clear art (black background, scrim hidden).
// ---------------------------------------------------------------------------

void main_screen_update_art(const uint8_t *jpeg_buf, size_t jpeg_size) {
    if (!s_art_canvas || !s_art_buf) return;

    if (!jpeg_buf || jpeg_size == 0) {
        // No art: hide canvas
        lv_obj_add_flag(s_art_canvas, LV_OBJ_FLAG_HIDDEN);
        // Fill black so stale pixels don't flash on next show
        lv_canvas_fill_bg(s_art_canvas, lv_color_hex(0x0A0A0A), LV_OPA_COVER);
        return;
    }

    TJpgDec.setJpgScale(ALBUM_ART_JPEG_SCALE);
    TJpgDec.setCallback(art_decode_cb);
    TJpgDec.drawJpg(0, 0, jpeg_buf, jpeg_size);

    // Show canvas, force redraw
    lv_obj_clear_flag(s_art_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_art_canvas);

    DEBUG_PRINTLN("[Art] Background canvas updated");
}

// ---------------------------------------------------------------------------
// main_screen_toggle_control_panel
// ---------------------------------------------------------------------------

void main_screen_toggle_control_panel() {
    if (!s_ctrl_panel) return;
    s_ctrl_visible = !s_ctrl_visible;
    if (s_ctrl_visible) {
        lv_obj_clear_flag(s_ctrl_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_ctrl_panel);
    } else {
        lv_obj_add_flag(s_ctrl_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

bool main_screen_is_control_panel_visible() {
    return s_ctrl_visible;
}

bool main_screen_is_standby_visible() {
    return s_standby_visible;
}

// ---------------------------------------------------------------------------
// main_screen_update_power_source
// Updates button highlight states to reflect current power and source.
// ---------------------------------------------------------------------------

void main_screen_update_power_source(bool power_on, bool source_is_usb) {
    // Standby screen: show when off, hide when on
    if (s_standby_panel) {
        s_standby_visible = !power_on;
        if (s_standby_visible) {
            lv_obj_clear_flag(s_standby_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_standby_panel);
            // Also force-close the control panel
            if (s_ctrl_visible) {
                s_ctrl_visible = false;
                lv_obj_add_flag(s_ctrl_panel, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_add_flag(s_standby_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (!s_btn_power) return;

    // Power button in control panel
    lv_obj_set_style_bg_color(s_btn_power,
        power_on ? lv_color_hex(0x1A3A1A) : lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_text_color(s_btn_power_icon,
        power_on ? lv_color_hex(0x44EE44) : lv_color_hex(0x666666), 0);
    lv_label_set_text(s_btn_power_label, power_on ? "ON" : "OFF");
    lv_obj_set_style_text_color(s_btn_power_label,
        power_on ? lv_color_hex(0x44EE44) : lv_color_hex(0x666666), 0);

    // Source buttons — highlight the active one
    lv_obj_set_style_bg_color(s_btn_wifi,
        !source_is_usb ? lv_color_hex(0x1A2A4A) : lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_bg_color(s_btn_usb,
        source_is_usb  ? lv_color_hex(0x3A2A0E) : lv_color_hex(0x2A2A2A), 0);
}

// ---------------------------------------------------------------------------
// main_screen_take_control_cmd
// Returns a pointer to the pending command string set by a button tap.
// The caller must act on it before calling again (it's cleared here).
// ---------------------------------------------------------------------------

const char *main_screen_take_control_cmd() {
    if (s_pending_cmd[0] == '\0') return nullptr;
    static char out[16];
    strncpy(out, s_pending_cmd, sizeof(out) - 1);
    out[sizeof(out) - 1] = '\0';
    s_pending_cmd[0] = '\0';
    return out;
}

const char *main_screen_take_track_cmd() {
    if (s_pending_track_cmd[0] == '\0') return nullptr;
    static char out[12];
    strncpy(out, s_pending_track_cmd, sizeof(out) - 1);
    out[sizeof(out) - 1] = '\0';
    s_pending_track_cmd[0] = '\0';
    return out;
}
