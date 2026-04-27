# OTA Flashing

OTA is HTTP-based. The device runs a small `WebServer` on port 80 that accepts a multipart POST of `firmware.bin` at `/update` and applies it via the ESP32 `Update.h` library, then reboots.

## Why not ArduinoOTA?

ArduinoOTA / espota.py uses UDP on port 3232. On this hardware combo
(Arduino-ESP32 3.x + ESP32-S3 + `ARDUINO_USB_CDC_ON_BOOT=1`) talking to a
macOS host, UDP delivery is unreliable:

- macOS `sendto()` on unconnected UDP can fail with `EHOSTUNREACH` due to
  interface-scoped routes.
- Even with `connect()`+`send()` (and a patched `espota.py`), packets to
  port 3232 returned `EPIPE`, indicating they never reached the device or
  the device's UDP listener was unreachable, despite ArduinoOTA logging
  a successful bind.

HTTP over TCP sidesteps all of that — and gives you a browser-based
fallback when the CLI fails.

## Usage

CLI:
```
pio run -e ota -t upload
```
This invokes `scripts/ota_upload.py`, which POSTs the built `firmware.bin`
to `http://deskknob.local/update`.

Browser fallback: visit `http://deskknob.local/` and use the form.

## First flash

The device must already be running firmware that includes the HTTP-OTA
endpoint. Bootstrap once over USB serial:
```
pio run -e esp32-s3-devkitc-1 -t upload --upload-port /dev/cu.usbmodem101
```
After that, all subsequent updates can go over WiFi.
