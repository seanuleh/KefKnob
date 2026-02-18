# KefKnob

A physical desk controller for KEF LSX II speakers, built on the Waveshare ESP32-S3 1.8" Touch LCD with rotary encoder.

Control volume, switch inputs, skip tracks, and see what's playing — all from a small round display on your desk.

---

## Features

- **Volume control** via rotary encoder with real-time arc display
- **Now playing** — track title, artist, and album art
  - WiFi source: metadata pulled directly from the KEF speaker API
  - USB source: metadata pulled from the Spotify Web API when Spotify is active
- **Track progress bar** — live playback position shown below the track info
- **Playback control** — on-screen prev/play-pause/next buttons
  - WiFi source: routes through KEF track control API
  - USB source: routes through Spotify Web API (requires Spotify Premium)
- **Mute toggle** — shown on USB source when Spotify is not active
- **Input switching** — WiFi and USB via swipe-down control panel
- **Standby screen** — shown when speaker is off; tap WiFi or USB to wake
- **Power control** — toggle standby from the control panel

---

## Hardware

| Component | Part |
|---|---|
| MCU | [Waveshare ESP32-S3 1.8" Touch LCD](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.8) (ESP32-S3, 240MHz, 8MB PSRAM, 16MB Flash) |
| Display | SH8601 360×360 round AMOLED, QSPI |
| Touch | CST816S capacitive touch, I2C |
| Input | Quadrature rotary encoder (built-in) |

---

## Setup

### 1. Prerequisites

- [PlatformIO](https://platformio.org/) CLI or VS Code extension
- KEF LSX II speaker on your local network
- Your speaker's IP address (find it in the KEF Connect app: Settings → speaker → (i))

### 2. Configure credentials

Copy the example config and fill in your details:

```bash
cp include/config_local.h.example include/config_local.h
```

Edit `include/config_local.h`:

```cpp
#define WIFI_SSID      "your-network"
#define WIFI_PASSWORD  "your-password"
#define KEF_SPEAKER_IP "192.168.1.XXX"
```

`config_local.h` is gitignored and will never be committed.

### 3. Spotify (optional — USB source now-playing + playback control)

Spotify integration enables album art, track info, and playback buttons when playing Spotify over USB. Playback control requires Spotify Premium.

1. Create an app at [developer.spotify.com/dashboard](https://developer.spotify.com/dashboard)
2. Add redirect URI: `http://127.0.0.1:8888/callback`
3. Add your Client ID and Secret to `config_local.h`
4. Run the one-time auth script embedded in the `config_local.h` comments to get a refresh token
5. Paste the refresh token into `config_local.h`

Without Spotify credentials the device still works fully — the USB screen shows a mute button instead.

### 4. Build and flash

```bash
pio run -t upload --upload-port /dev/cu.usbmodem101
```

Replace the port with yours (`pio device list` to find it).

---

## Usage

| Gesture / Control | Action |
|---|---|
| Rotate encoder | Adjust volume |
| Tap prev button | Previous track |
| Tap play/pause button | Play / pause |
| Tap next button | Next track |
| Tap mute button (USB, no Spotify) | Toggle mute |
| Swipe down from top | Open control panel (power, WiFi input, USB input) |
| Tap WiFi on standby screen | Wake speaker on WiFi input |
| Tap USB on standby screen | Wake speaker on USB input |

---

## Project Structure

```
KefKnob/
├── include/
│   ├── config.h                # Pin assignments, timeouts, constants
│   ├── config_local.h          # WiFi, speaker IP, Spotify credentials (gitignored)
│   ├── config_local.h.example  # Template for config_local.h
│   └── lv_conf.h               # LVGL configuration
├── src/
│   ├── main.cpp                # App wiring: tasks, LVGL init, touch/encoder input
│   ├── drivers/                # SH8601 display, CST816S touch, encoder
│   ├── network/
│   │   ├── kef_api.cpp/.h      # KEF HTTP API (volume, player data, source, power)
│   │   └── spotify_api.cpp/.h  # Spotify Web API (now-playing + playback control)
│   └── ui/                     # LVGL screen layout and updates
├── poc/                        # Proof-of-concept scripts (Python)
├── platformio.ini
├── CLAUDE.md                   # Developer / AI agent reference
└── README.md
```

---

## Acknowledgments

- [roon-knob](https://github.com/muness/roon-knob) by muness — the original inspiration for this project; a Waveshare ESP32-S3 knob controller for Roon, LMS, and OpenHome
- [pykefcontrol](https://github.com/N0ciple/pykefcontrol) — KEF HTTP API reference implementation
- [LVGL](https://lvgl.io/) — embedded graphics library
- [Waveshare](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.8) — hardware reference and drivers
