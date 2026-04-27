"""Custom upload step for [env:ota] — POSTs the built firmware.bin to the
device's /update endpoint via HTTP. Replaces ArduinoOTA/espota.py (UDP-based,
unreliable on macOS + Arduino-ESP32 3.x + ESP32-S3).

Usage: pio run -e ota -t upload
"""
import subprocess
import sys

Import("env")  # noqa: F821 — provided by PlatformIO


def on_upload(source, target, env):
    firmware = str(source[0])
    host = env.GetProjectOption("upload_port")
    url = f"http://{host}/update"
    print(f"[ota] POST {firmware} -> {url}")

    # Use curl: Python's urllib picks bad routes on macOS for .local hostnames,
    # while curl uses getaddrinfo + happy-eyeballs and works reliably.
    rc = subprocess.call([
        "curl", "--fail", "--show-error", "--max-time", "180",
        "-F", f"firmware=@{firmware}",
        url,
    ])
    if rc != 0:
        print(f"[ota] curl exited {rc}")
        sys.exit(rc)


env.Replace(UPLOADCMD=on_upload)  # noqa: F821
