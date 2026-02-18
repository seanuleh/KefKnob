#pragma once

#include <lvgl.h>
#include <stdint.h>
#include <stddef.h>

/**
 * Create and load the main DeskKnob screen.
 * Call once after LVGL is initialized.
 */
void main_screen_create();

/**
 * Update all widgets with current state.
 * Must be called only from Core 1 (the LVGL thread).
 */
void main_screen_update(int volume, const char *title,
                        const char *artist, bool is_playing,
                        bool source_is_usb, bool is_muted,
                        bool spotify_active, int progress_pct);

/**
 * Decode a JPEG and blit it to the album art canvas.
 * Must be called only from Core 1 (the LVGL thread).
 *
 * @param jpeg_buf  Raw JPEG bytes (allocated in PSRAM by kef_fetch_jpeg).
 *                  Pass nullptr to clear the canvas to the background color.
 * @param jpeg_size Number of bytes in jpeg_buf.
 */
void main_screen_update_art(const uint8_t *jpeg_buf, size_t jpeg_size);

/**
 * Toggle the control panel overlay (swipe-from-top reveals it).
 * Must be called only from Core 1.
 */
void main_screen_toggle_control_panel();

/**
 * Returns true if the control panel is currently visible.
 */
bool main_screen_is_control_panel_visible();

/**
 * Returns true if the standby screen is currently visible (speaker off).
 */
bool main_screen_is_standby_visible();

/**
 * Update the power / source indicator states in the control panel.
 * Must be called only from Core 1.
 *
 * @param power_on      true = speaker is on, false = standby.
 * @param source_is_usb true = USB input active, false = WiFi/other.
 */
void main_screen_update_power_source(bool power_on, bool source_is_usb);

/**
 * Take (and clear) a pending command string set by a control panel button tap.
 * Returns nullptr if no command is pending.
 * Must be called only from Core 1.
 */
const char *main_screen_take_control_cmd();

/**
 * Take (and clear) a pending track command set by a bottom playback button tap.
 * Returns nullptr if no command is pending.
 * Must be called only from Core 1.
 */
const char *main_screen_take_track_cmd();

/**
 * Redraw the waveform visualiser with amplitude history.
 * levels[0] is the oldest sample, levels[count-1] the newest (0–255 each).
 * count should equal MIC_N_BARS from config.h.  Core 1 only.
 */
void main_screen_update_waveform(const uint8_t *levels, int count);
