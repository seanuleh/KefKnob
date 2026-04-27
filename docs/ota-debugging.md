# OTA Flashing — Debugging Notes

## Problem

OTA flashing via `pio run -e ota -t upload` fails. The espota.py tool reports:
```
Sending invitation to deskknob.local failed
Host deskknob.local Not Found
```

This also fails when using the direct IP (`192.168.1.90`).

---

## What We Confirmed Works

- Device is reachable via ping: `ping deskknob.local` → `192.168.1.90`
- ARP entry is valid: `d0:cf:13:1e:1a:ac on en0`
- Single network interface (`en0`, `192.168.1.252`) — no routing ambiguity
- macOS firewall is disabled
- KEF polling, volume control, source switching all work (TCP/HTTP from ESP32 → Mac network is fine)

---

## Root Cause Analysis

### ESP32 side — ArduinoOTA IS initialising

After enabling `CORE_DEBUG_LEVEL=4` in `platformio.ini`, the internal ESP32 log confirms ArduinoOTA successfully binds:

```
[  4851][I][ArduinoOTA.cpp:190] begin(): OTA server at: deskknob.local:3232
```

This log line is only reached **after** `_initialized = true` and `_udp_ota.begin(3232)` succeeds. The "udp bind failed" log is never seen.

We also added a manual `WiFiUDP` test socket on port 4444 in `loop()`, which also reports bind OK.

### Mac side — UDP sending fails for ALL ports on 192.168.1.90

Python `socket.connect() + send()` to the ESP32 returns **`EPIPE (errno 32)`** on `send()` for:
- Port 3232 (ArduinoOTA)
- Port 4444 (our test socket)

The same EPIPE pattern occurs for the router (`192.168.1.1:9999`) and KEF speaker (`192.168.1.217:9999`) — **this is expected** for those because nothing is listening. EPIPE on a connected UDP socket means the remote sent ICMP port unreachable.

**Key finding**: EPIPE for port 4444 on the ESP32, despite the device logging a successful bind, strongly suggests the device is **crashing and rebooting** after init (before it can process packets). During the reboot window, port 4444 is unbound → ICMP port unreachable → EPIPE.

### espota.py uses unconnected `sendto` — fails differently on macOS

The espota.py tool uses `sock.sendto(msg, (ip, port))` without calling `connect()` first. On macOS with interface-scoped routes (`ifscope`), unconnected UDP `sendto` fails immediately with `EHOSTUNREACH (errno 65)` — the packet never leaves the Mac. This is a **macOS socket scoping bug**.

We patched espota.py to use `connect() + send()` instead:
```
~/.platformio/packages/framework-arduinoespressif32/tools/espota.py
```
Lines ~113-115 and ~194-197. The patch makes the packet leave the Mac, but it then hits the ESP32 crash issue above.

---

## What We Changed (and may need reverting)

### `src/main.cpp`
- Added `#include <WiFiUdp.h>` (diagnostic, should be removed)
- Added `static WiFiUDP s_test_udp` UDP echo server on port 4444 in `loop()` (diagnostic, should be removed)
- Added `delay(500)` before `initOTA()` in `setup()` (may keep — harmless)
- Added `-DCORE_DEBUG_LEVEL=4` to `platformio.ini` build flags (diagnostic, should be removed)

### `platformio.ini`
- `-DCORE_DEBUG_LEVEL=4` added — causes verbose internal ESP32 logging, increases flash slightly

---

## Next Steps to Investigate

1. **Is the device crashing after init?** Read full serial output (no filtering) for 30+ seconds after boot. Look for `Guru Meditation Error`, `assert failed`, `Backtrace`, or repeated boot messages.

2. **Is CORE_DEBUG_LEVEL=4 causing a crash?** Try reverting `platformio.ini` and the diagnostic `loop()` changes, flash clean, then probe port 3232.

3. **Test if ArduinoOTA works WITHOUT the networkTask running** — try commenting out `createTasks()` temporarily to isolate whether Core 0 network activity is somehow closing the UDP socket.

4. **Known ESP32 Arduino 3.x issue** — ArduinoOTA may have a bug with ESP32-S3 + `ARDUINO_USB_CDC_ON_BOOT=1`. Check [github.com/espressif/arduino-esp32/issues](https://github.com/espressif/arduino-esp32/issues) for `ArduinoOTA` + `esp32s3` + `3.x`.

5. **Alternative OTA method** — If ArduinoOTA remains broken, implement `ElegantOTA` (HTTP-based, no UDP, works reliably on ESP32 Arduino 3.x). Drop-in replacement, accessible via browser at `http://deskknob.local/update`.

---

## Current State of Files

Clean up before starting fresh:
- Revert `src/main.cpp`: remove `WiFiUdp.h` include and `s_test_udp` block in `loop()`
- Revert `platformio.ini`: remove `-DCORE_DEBUG_LEVEL=4`
- The `delay(500)` before `initOTA()` is harmless to keep
- The espota.py patch at `~/.platformio/packages/framework-arduinoespressif32/tools/espota.py` is in place (connect+send instead of sendto)
