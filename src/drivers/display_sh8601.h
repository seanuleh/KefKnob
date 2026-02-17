/**
 * Display Driver for SH8601 (360x360 Round LCD)
 * QSPI interface
 */

#ifndef DISPLAY_SH8601_H
#define DISPLAY_SH8601_H

#include <Arduino.h>
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize display hardware (SPI bus and backlight)
 * Call this first, before LVGL init
 */
bool display_init_hardware(void);

/**
 * Initialize display panel with LVGL callback
 * Call this during LVGL init, after you have the display driver
 * Parameters: callback function and user_ctx (usually &disp_drv)
 */
bool display_init_panel(void *on_color_trans_done, void *user_ctx);

/**
 * Set display backlight brightness (0-255)
 */
void display_set_brightness(uint8_t brightness);

/**
 * Get the display panel handle (for LVGL integration)
 */
esp_lcd_panel_handle_t display_get_panel_handle(void);

#ifdef __cplusplus
}
#endif

#endif // DISPLAY_SH8601_H
