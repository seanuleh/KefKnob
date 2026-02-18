#ifndef CONFIG_H
#define CONFIG_H

// ============================================================================
// LOCAL CONFIGURATION (WiFi & KEF credentials)
// ============================================================================
// Include local configuration if it exists (gitignored)
// Copy config_local.h.example to config_local.h and add your credentials
#if __has_include("config_local.h")
    #include "config_local.h"
#else
    #warning "config_local.h not found! Copy config_local.h.example to config_local.h"
    #define WIFI_SSID "YOUR_WIFI_SSID"
    #define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
    #define KEF_SPEAKER_IP "192.168.1.217"
#endif

// Spotify credentials — set in config_local.h. Leave empty to disable.
#ifndef SPOTIFY_CLIENT_ID
    #define SPOTIFY_CLIENT_ID     ""
    #define SPOTIFY_CLIENT_SECRET ""
    #define SPOTIFY_REFRESH_TOKEN ""
#endif

// ============================================================================
// NETWORK CONFIGURATION
// ============================================================================

// OTA
#define OTA_HOSTNAME "deskknob"
// #define OTA_PASSWORD "changeme"  // Uncomment to require a password

// KEF API Configuration
#define KEF_API_PORT 80
#define KEF_API_BASE_URL "http://" KEF_SPEAKER_IP "/api"

// Network timeouts (milliseconds)
#define HTTP_TIMEOUT 5000
#define WIFI_CONNECT_TIMEOUT 20000
#define KEF_POLL_TIMEOUT 50000  // KEF uses 50s polling timeout

// ============================================================================
// HARDWARE PIN CONFIGURATION - Waveshare ESP32-S3 1.8" LCD
// ============================================================================

// SH8601 Display (QSPI Interface) - Based on Waveshare demo pinout
#define LCD_QSPI_SCK    13
#define LCD_QSPI_D0     15
#define LCD_QSPI_D1     16
#define LCD_QSPI_D2     17
#define LCD_QSPI_D3     18
#define LCD_CS          14
#define LCD_RST         21
#define LCD_DC          -1  // Not used in QSPI mode
#define LCD_BL          47  // Backlight PWM

// Display properties
#define LCD_WIDTH       360
#define LCD_HEIGHT      360
#define LCD_ROTATION    180  // USB connector on opposite side

// CST816S Touch Controller (I2C Interface)
#define TOUCH_SDA       11
#define TOUCH_SCL       12
#define TOUCH_INT       -1  // Optional interrupt pin
#define TOUCH_RST       -1  // Optional reset pin
#define TOUCH_I2C_ADDR  0x15

// Rotary Encoder
#define ENCODER_A       8   // Encoder A channel
#define ENCODER_B       7   // Encoder B channel
#define ENCODER_BTN     -1  // Encoder button (if available)

// ============================================================================
// DISPLAY CONFIGURATION
// ============================================================================

// Backlight settings
#define LCD_BL_PWM_CHANNEL  0
#define LCD_BL_PWM_FREQ     5000
#define LCD_BL_PWM_RES      8
#define LCD_BRIGHTNESS_MAX  255
#define LCD_BRIGHTNESS_MIN  10
#define LCD_BRIGHTNESS_DEFAULT 200

// Display sleep timeout (milliseconds)
#define DISPLAY_SLEEP_TIMEOUT 60000  // 60 seconds

// ============================================================================
// LVGL CONFIGURATION
// ============================================================================

// LVGL buffer size (pixels) - adjust based on PSRAM availability
#define LVGL_BUFFER_SIZE (LCD_WIDTH * LCD_HEIGHT / 10)

// LVGL refresh rate
#define LVGL_TICK_PERIOD_MS 5

// LVGL DMA buffer count (double buffering)
#define LVGL_BUFFER_COUNT 2

// ============================================================================
// APPLICATION CONFIGURATION
// ============================================================================

// Volume control
#define VOLUME_MIN 0
#define VOLUME_MAX 100
#define VOLUME_STEP 1  // Volume change per encoder click

// Polling intervals (milliseconds)
#define KEF_STATE_POLL_INTERVAL 1000   // Poll KEF state every second
#define UI_UPDATE_INTERVAL 50           // Update UI every 50ms (20 FPS)

// Input control
#define VOLUME_DEBOUNCE_MS  250   // Delay before sending volume to speaker
#define SWIPE_THRESHOLD     50    // Pixels of horizontal movement = swipe

// Album artwork
#define ALBUM_ART_SIZE       360          // Decoded canvas px — fills the round display
#define ALBUM_ART_JPEG_SCALE 1            // TJpgDec full-res; center-crop in callback
#define ALBUM_ART_JPEG_SRC   640          // Spotify standard JPEG dimension (px)
#define ALBUM_ART_MAX_JPEG   (256 * 1024) // Max JPEG bytes to allocate in PSRAM

// ============================================================================
// DEBUG CONFIGURATION
// ============================================================================

// Enable serial debug output
#define DEBUG_ENABLED 1

#if DEBUG_ENABLED
    #define DEBUG_PRINT(x) Serial.print(x)
    #define DEBUG_PRINTLN(x) Serial.println(x)
    #define DEBUG_PRINTF(fmt, ...) Serial.printf(fmt, ##__VA_ARGS__)
#else
    #define DEBUG_PRINT(x)
    #define DEBUG_PRINTLN(x)
    #define DEBUG_PRINTF(fmt, ...)
#endif

// ============================================================================
// FREERTOS TASK CONFIGURATION
// ============================================================================

// Task priorities (higher number = higher priority)
#define UI_TASK_PRIORITY 10
#define NETWORK_TASK_PRIORITY 5
#define INPUT_TASK_PRIORITY 8

// Task stack sizes
#define UI_TASK_STACK_SIZE      (4 * 1024)   // UI handled in loop(), minimal task
#define NETWORK_TASK_STACK_SIZE (36 * 1024)  // HTTP + JSON + TLS (KEF + Spotify HTTPS)

// Task core assignments
#define UI_TASK_CORE 1
#define NETWORK_TASK_CORE 0

#endif // CONFIG_H
