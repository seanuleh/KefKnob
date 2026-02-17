# KefKnob

A physical desk controller for KEF LSX II speakers, built on the Waveshare ESP32-S3 1.8" Touch LCD with rotary encoder.

Control volume, switch inputs, and see what's playing — all from a small round display on your desk.

---

## Features

- **Volume control** via rotary encoder
- **Now playing** — track title, artist, and album art from Spotify (via KEF)
- **Playback control** — tap to play/pause, swipe left/right to skip
- **Input switching** — WiFi, USB via swipe-down control panel
- **Standby screen** — shown when speaker is off; tap WiFi or USB to wake
- **Power control** — toggle standby from the control panel

---

## Hardware

| Component | Part |
|---|---|
| MCU | Waveshare ESP32-S3 1.8" Touch LCD (ESP32-S3, 240MHz, 8MB PSRAM, 16MB Flash) |
| Display | SH8601 360×360 round AMOLED, QSPI |
| Touch | CST816S capacitive touch, I2C |
| Input | Quadrature rotary encoder |

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
#define WIFI_SSID     "your-network"
#define WIFI_PASSWORD "your-password"
#define KEF_SPEAKER_IP "192.168.1.XXX"
```

`config_local.h` is gitignored and will never be committed.

### 3. Build and flash

```bash
pio run -t upload --upload-port /dev/cu.usbmodem101
```

Replace the port with yours (`pio device list` to find it).

---

## Usage

| Gesture | Action |
|---|---|
| Rotate encoder | Adjust volume |
| Tap screen | Play / pause |
| Swipe left | Next track |
| Swipe right | Previous track |
| Swipe down from top | Open control panel (power, WiFi, USB) |
| Tap WiFi on standby screen | Wake speaker on WiFi input |
| Tap USB on standby screen | Wake speaker on USB input |

---

## Project Structure

```
KefKnob/
├── include/
│   ├── config.h                # Pin assignments, timeouts, constants
│   ├── config_local.h          # WiFi + speaker IP (gitignored)
│   ├── config_local.h.example  # Template for config_local.h
│   └── lv_conf.h               # LVGL configuration
├── src/
│   ├── main.cpp                # App wiring: tasks, LVGL init, touch/encoder input
│   ├── drivers/                # SH8601 display, CST816S touch, encoder
│   ├── network/                # KEF HTTP API wrapper
│   └── ui/                     # LVGL screen layout and updates
├── poc/                        # Proof-of-concept scripts (Python)
│   ├── pykefcontrol/           # KEF API reference implementation
│   ├── test_wake.py            # Test waking speaker via HTTP API
│   ├── test_sleep.py           # Test standby via HTTP API
│   └── test_album_art.py       # Test album art fetch
├── platformio.ini
├── CLAUDE.md                   # Developer/agent reference
└── README.md
```

---

## Acknowledgments

- [pykefcontrol](https://github.com/N0ciple/pykefcontrol) — KEF HTTP API reference
- [LVGL](https://lvgl.io/) — embedded graphics library
- [Waveshare](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.8) — hardware reference and drivers
