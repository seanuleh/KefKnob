# ESP32-S3 DeskKnob: KEF LSX II Controller Implementation Plan

## Context

You've purchased a Waveshare ESP32-S3 Knob Touch LCD 1.8" device and want to create a custom controller for your KEF LSX II speakers. This addresses the need for a dedicated physical controller with a tactile rotary knob and touchscreen interface, providing more intuitive control than using a phone app. The device will replace or complement the KEF Connect app for common operations like volume adjustment, playback control, and source switching.

**Why this approach:** The ESP32-S3 device combines the perfect hardware elements (360×360 circular display, capacitive touch with gesture support, rotary encoder, WiFi) with sufficient processing power (dual-core 240MHz, 8MB PSRAM) to create a responsive, feature-rich controller. The KEF speakers expose a well-documented HTTP REST API, making integration straightforward without requiring reverse engineering.

## Technology Stack

- **Framework**: Arduino + PlatformIO (rapid development, mature libraries, excellent LVGL support)
- **GUI**: LVGL 8.3.8 (industry-standard embedded GUI framework)
- **Display Driver**: `esp_lcd_st77916` (official Espressif driver for ST77916 QSPI)
- **Touch Driver**: `esp_lcd_touch_cst816s` (I2C with gesture support)
- **Key Libraries**: ArduinoJson 6.21+, ESP32RotaryEncoder, WiFi/HTTPClient (built-in)
- **UI Design**: Modern dark theme

## Project Structure

```
DeskKnob/
├── platformio.ini                 # PlatformIO configuration
├── include/
│   └── config.h                   # WiFi credentials, KEF speaker IP
├── src/
│   ├── main.cpp                   # Entry point, FreeRTOS tasks setup
│   ├── drivers/
│   │   ├── display_driver.cpp/h   # ST77916 QSPI initialization
│   │   ├── touch_driver.cpp/h     # CST816 touch + gesture handling
│   │   └── encoder_driver.cpp/h   # Rotary encoder ISR handling
│   ├── network/
│   │   ├── wifi_manager.cpp/h     # WiFi connection management
│   │   └── kef_api_client.cpp/h   # KEF HTTP API wrapper
│   ├── ui/
│   │   ├── ui_manager.cpp/h       # Screen navigation state machine
│   │   ├── screens/
│   │   │   ├── screen_main.cpp/h  # Main KEF control screen
│   │   │   └── screen_nowplaying.cpp/h # Now playing with artwork
│   │   └── widgets/
│   │       ├── volume_widget.cpp/h
│   │       ├── playback_controls.cpp/h
│   │       └── artwork_widget.cpp/h
│   ├── state/
│   │   └── speaker_state.cpp/h    # Centralized state management
│   └── utils/
│       ├── image_downloader.cpp/h # HTTP image fetch & cache
│       └── logger.cpp/h           # Serial debug logging
└── data/
    └── (fonts, icons as needed)
```

## Critical Files to Create

### 1. **src/main.cpp**
- **Purpose**: Application entry point, orchestrates all components
- **Key Responsibilities**:
  - Initialize hardware (display, touch, encoder)
  - Create FreeRTOS tasks (UI task on Core 1, Network task on Core 0)
  - Set up LVGL tick timer
  - Main loop: process LVGL, handle inputs, update UI

### 2. **src/network/kef_api_client.cpp/h**
- **Purpose**: KEF speaker API wrapper (port logic from pykefcontrol)
- **Key Methods**:
  - `setVolume(int)`, `getVolume()`, `mute()`, `unmute()`
  - `setSource(String)` - switch between "wifi" (USB-C/Spotify) and other sources
  - `togglePlayPause()`, `nextTrack()`, `prevTrack()`
  - `getSongInformation()` - returns JSON with title, artist, album, cover_url
  - `pollSpeaker(timeout)` - KEF event queue polling system
- **Reference**: Existing pykefcontrol implementation patterns
- **Protocol**: HTTP REST API at `http://{speaker-ip}/api/`

### 3. **src/drivers/display_driver.cpp/h**
- **Purpose**: ST77916 QSPI display initialization
- **Key Tasks**:
  - Configure QSPI pins (SCK, D0-D3, CS, RST, DC)
  - Initialize esp_lcd driver with DMA
  - Create frame buffers in PSRAM (2× 360×360×2 bytes)
  - Integrate with LVGL display callbacks

### 4. **src/state/speaker_state.cpp/h**
- **Purpose**: Single source of truth for all speaker and UI state
- **Data Structure**:
  ```cpp
  struct SpeakerState {
      bool connected;
      String source;           // "wifi", "bluetooth", "standby"
      int volume;              // 0-100
      bool is_muted;
      bool is_playing;
      String title, artist, album;
      String cover_url;
      lv_img_dsc_t* artwork_img;  // Cached decoded album art
      uint8_t current_screen;  // 0=main, 1=now_playing
  };
  ```

### 5. **src/ui/screens/screen_main.cpp/h**
- **Purpose**: Main control screen layout
- **Components**:
  - Large volume slider (controlled by rotary encoder)
  - Play/Pause/Prev/Next/Mute buttons (bottom row)
  - Source indicator (top: "USB-C" or "WiFi • Spotify")
  - Source toggle button (switch between USB-C and WiFi)
  - Connection status icon

## Implementation Phases

### Phase 1: Hardware Foundation (Days 1-3)
**Goal**: Get display, touch, and encoder working with basic LVGL demo

1. Configure `platformio.ini` with ESP32-S3 settings, PSRAM support, LVGL flags
2. Initialize ST77916 display via QSPI with DMA frame buffers
3. Initialize CST816 touch controller with IRQ-based reading
4. Initialize rotary encoder with interrupt-based debouncing
5. Deploy simple LVGL test UI (colored rectangles, button, label)

**Verification**: Display shows LVGL content, touch prints coordinates, encoder prints rotation count

### Phase 2: KEF API Integration (Days 4-6)
**Goal**: Establish network communication with speaker

1. Implement WiFi Manager with auto-reconnect
2. Port KEF API client from pykefcontrol Python code:
   - Volume control endpoints
   - Playback control (play/pause/next/prev)
   - Source switching
   - Metadata retrieval
   - Event polling system (50s timeout)
3. Test all endpoints via Serial commands

**Verification**: Can control speaker volume, playback, retrieve metadata via Serial monitor

### Phase 3: UI Screens (Days 7-10)
**Goal**: Build main and now-playing screens with navigation

1. Design **Screen 0 (Main Control)**:
   - Volume slider (large, center) - linked to rotary encoder
   - Play/Pause/Prev/Next buttons (bottom)
   - Mute button
   - Source display ("USB-C" or "WiFi • Spotify")
   - Source toggle button
2. Design **Screen 1 (Now Playing)**:
   - Album artwork placeholder (240×240)
   - Song title, artist, album labels
   - Same playback controls as Screen 0
3. Implement swipe gesture navigation (CST816 LEFT/RIGHT)
4. Connect UI buttons to KEF API calls
5. Wire rotary encoder to volume control (only on Screen 0)

**Verification**: Can navigate screens, control speaker from touchscreen, rotary encoder adjusts volume

### Phase 4: Album Artwork (Days 11-13)
**Goal**: Display real Spotify album art

1. Implement HTTP image downloader
   - Fetch from `cover_url` (provided by KEF API)
   - Store in PSRAM (up to 200KB buffer)
   - LRU cache for last 3 artworks
2. Integrate LVGL SJPEG decoder
   - Enable `LV_USE_SJPG` in lv_conf.h
   - Decode JPEG to RGB565
   - Scale/crop to 240×240 if needed
3. Update Now Playing screen with real artwork
4. Implement metadata polling (every 10s when on Now Playing screen)

**Verification**: Album artwork displays correctly, updates when song changes

### Phase 5: Real-Time State Sync (Days 14-15)
**Goal**: Keep UI in sync with speaker state

1. Implement KEF event polling in background FreeRTOS task (Core 0)
   - Use `pollSpeaker(50)` with 50s timeout
   - Parse JSON for changed properties
2. Update `SpeakerState` singleton from polling results
3. Trigger UI updates via thread-safe queue (LVGL message system)
4. Handle edge cases:
   - Speaker offline → show "Disconnected" overlay
   - WiFi lost → reconnect with backoff
   - Source switched externally → update UI

**Verification**: UI reflects changes made from KEF Connect app or speaker hardware buttons

### Phase 6: Polish & Optimization (Days 16-18)
**Goal**: Production-ready firmware

1. Memory optimization (monitor PSRAM usage, reduce fragmentation)
2. Performance tuning (target 30 FPS, optimize HTTP latency)
3. Error handling (timeouts, offline speaker, network issues)
4. UI polish (smooth animations, custom fonts/icons, dark theme implementation)
5. Power management (display sleep after 60s, wake on touch)
6. 24+ hour stability testing

**Verification**: Stable 24h operation, smooth UI, graceful error handling

## Architecture Overview

### Component Interaction
```
┌─────────────────────────────────────────────────────────────┐
│                         main.cpp                            │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │ FreeRTOS     │  │ FreeRTOS     │  │ Arduino      │      │
│  │ Task: UI     │  │ Task: Network│  │ Loop: Inputs │      │
│  │ (Core 1)     │  │ (Core 0)     │  │ (Core 1)     │      │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘      │
└─────────┼──────────────────┼──────────────────┼─────────────┘
          ↓                  ↓                  ↓
    UI Manager         WiFi Manager       Input Drivers
    - LVGL Handler     - Connection       - Touch (I2C)
    - Screen Nav       - Reconnect        - Encoder (ISR)
          ↓                  ↓                  ↓
    LVGL Screens       KEF API Client    Display Driver
    - Main             - HTTP Requests   - ST77916 QSPI
    - Now Playing      - Event Polling   - DMA Transfer
          ↓                  ↓
    Speaker State  ←───Image Cache
    (Singleton)        - PSRAM Store
    - Volume           - HTTP Fetch
    - Source           - JPEG Decode
    - Playback
    - Metadata
```

### FreeRTOS Task Structure
- **UI Task (Core 1, Priority 10)**: LVGL rendering, input handling, screen updates
- **Network Task (Core 0, Priority 5)**: KEF API polling, WiFi management, image downloads
- **Queue**: Thread-safe communication from Network to UI (state changes)

### Memory Allocation
- **SRAM (512KB)**: Core system, FreeRTOS, LVGL heap (64KB), network buffers, application code
- **PSRAM (8MB)**: Frame buffers (2× 518KB), artwork cache (3× ~200KB), JPEG decode buffers

## Key Pin Configuration

*(Verify with actual hardware - these are typical for Waveshare ESP32-S3 Knob)*

```cpp
// ST77916 Display (QSPI)
#define LCD_QSPI_SCK   GPIO_NUM_12
#define LCD_QSPI_D0    GPIO_NUM_11
#define LCD_QSPI_D1    GPIO_NUM_13
#define LCD_QSPI_D2    GPIO_NUM_14
#define LCD_QSPI_D3    GPIO_NUM_15
#define LCD_CS         GPIO_NUM_10
#define LCD_RST        GPIO_NUM_9
#define LCD_DC         GPIO_NUM_8
#define LCD_BL         GPIO_NUM_46  // Backlight PWM

// CST816 Touch (I2C)
#define TOUCH_SDA      GPIO_NUM_6
#define TOUCH_SCL      GPIO_NUM_7
#define TOUCH_INT      GPIO_NUM_5
#define TOUCH_RST      GPIO_NUM_4

// Rotary Encoder
#define ENCODER_CLK    GPIO_NUM_1
#define ENCODER_DT     GPIO_NUM_2
```

## Important Implementation Notes

### KEF API Details (from pykefcontrol)
- **Base URL**: `http://{speaker-ip}/api/`
- **Volume**: `GET /setData?path=settings:/mediaPlayer/volume&roles=value&i32_=<0-100>`
- **Source**: `GET /setData?path=settings:/kef/play/physicalSource&roles=value&string_=wifi` (use "wifi" for both USB-C and Spotify Connect)
- **Metadata**: `GET /getData?path=player:player/data` returns JSON with `trackRoles.mediaData.metaData` containing artist, album, serviceID
- **Polling**: POST to `/event/modifyQueue` to create queue, then GET `/event/pollQueue` with 50s timeout

### Reusable Functions
Look for these existing utilities in pykefcontrol to port:
- **File**: `pykefcontrol/kef_connector.py`
  - `_get_player_data()` - Parse metadata JSON (lines ~150-200)
  - `get_song_information()` - Extract title/artist/album/cover_url (lines ~200-250)
  - Event polling system (lines ~300-400)

### Critical Challenges

1. **ST77916 QSPI Display**: Use Espressif's official `esp_lcd_st77916` component; reference Tasmota discussion for Waveshare-specific init sequence
2. **CST816 Touch**: Only responds after IRQ assertion; attach interrupt to INT pin before I2C read
3. **Album Artwork**: Download/decode in separate task to avoid blocking UI; use LVGL's SJPEG decoder
4. **LVGL Thread Safety**: Never call LVGL functions from network task; use queue to pass state changes to UI task
5. **Rotary Encoder Noise**: Use hardware debouncing (capacitors) + software debouncing in ISR

## Testing Plan

### Unit Testing
- Mock HTTP responses to test KEF API client parsing
- Test state transitions (playing→paused, source changes)
- Test LRU cache for artwork

### Integration Testing
1. Display + Touch: Multi-touch, gesture accuracy
2. Encoder + Volume: Smooth changes, no dropped counts
3. WiFi + KEF API: Connection resilience, error handling
4. Full Stack: Change song on KEF app → verify ESP32 UI updates

### Performance Targets
- UI: 30 FPS minimum
- Touch response: <50ms (touch to screen update)
- Encoder response: <20ms (rotation to volume change)
- KEF command latency: <200ms (button to speaker action)
- Album art load: <2s (fetch to display)

## Configuration Requirements

**User must provide:**
- WiFi SSID and password (stored in `include/config.h`)
- KEF speaker IP address (obtainable from KEF Connect app: Settings → Speaker Name → Info → IP Address)

## Future Enhancements (Post-MVP)

- Settings screen (WiFi config, brightness, sleep timeout)
- EQ presets visualization
- OTA firmware updates
- Additional screens (use swipe navigation to access)
- RGB LED ring ambient lighting
- Haptic feedback for encoder

---

This plan provides a structured, phase-by-phase approach to building a production-quality KEF speaker controller. The initial 2-screen implementation (Main + Now Playing) will be fully functional within 2-3 weeks, with room for future expansion through the multi-screen navigation system.
