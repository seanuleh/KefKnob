#!/usr/bin/env python3
"""
Probe KEF LSX II album art for ESP32-S3 display compatibility.

Investigates trackRoles.icon from player:player/data to determine:
  - URL format and fetchability
  - Image format (JPEG vs PNG)
  - Content-Length presence (required: esp_jpeg pre-allocates exact buffer)
  - Transfer-Encoding (chunked = bad)
  - Dimensions and file size at native + scaled sizes
  - Whether URL accepts width/height query params

Usage:
    python3 test_album_art.py
"""

import json
import sys
import struct
import requests

SPEAKER_IP = "192.168.1.217"
BASE_URL = f"http://{SPEAKER_IP}/api"

# From image_render.md: target sizes for 360x360 round display
TARGET_SIZES = [128, 180, 240]

# From image_render.md: safe cap to avoid PSRAM pressure
MAX_SAFE_BYTES = 64 * 1024  # 64 KB


def hr(char="-", width=60):
    print(char * width)


def get_player_data():
    """Fetch full player:player/data and return raw JSON."""
    payload = {"path": "player:player/data", "roles": "value"}
    resp = requests.get(f"{BASE_URL}/getData", params=payload, timeout=5)
    resp.raise_for_status()
    return resp.json()[0]


def detect_format(data: bytes) -> str:
    """Identify image format from magic bytes."""
    if data[:2] == b'\xff\xd8':
        return "JPEG"
    if data[:8] == b'\x89PNG\r\n\x1a\n':
        return "PNG"
    if data[:4] == b'RIFF' and data[8:12] == b'WEBP':
        return "WEBP"
    if data[:4] in (b'GIF8', b'GIF9'):
        return "GIF"
    return f"UNKNOWN (first 4 bytes: {data[:4].hex()})"


def jpeg_dimensions(data: bytes):
    """Parse JPEG SOF marker to extract width/height. Returns (w, h) or None."""
    i = 2  # skip SOI
    while i < len(data) - 8:
        if data[i] != 0xFF:
            break
        marker = data[i + 1]
        # SOF markers: 0xC0-0xC3, 0xC5-0xC7, 0xC9-0xCB, 0xCD-0xCF
        if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
            h = struct.unpack('>H', data[i + 5:i + 7])[0]
            w = struct.unpack('>H', data[i + 7:i + 9])[0]
            return w, h
        length = struct.unpack('>H', data[i + 2:i + 4])[0]
        i += 2 + length
    return None


def png_dimensions(data: bytes):
    """Parse PNG IHDR to extract width/height. Returns (w, h) or None."""
    if len(data) < 24:
        return None
    w = struct.unpack('>I', data[16:20])[0]
    h = struct.unpack('>I', data[20:24])[0]
    return w, h


def probe_url(url: str, label: str = ""):
    """Fetch a URL and report all properties relevant to ESP32-S3 rendering."""
    print(f"\n  URL: {url}")
    try:
        resp = requests.get(url, timeout=10, allow_redirects=True)
    except requests.exceptions.RequestException as e:
        print(f"  ERROR fetching: {e}")
        return None

    print(f"  HTTP status:       {resp.status_code}")
    if resp.status_code != 200:
        return None

    # Redirect chain
    if resp.history:
        hops = " -> ".join(r.url for r in resp.history) + f" -> {resp.url}"
        print(f"  Redirect chain:    {hops}")
        print(f"  Final URL:         {resp.url}")

    # Headers relevant to ESP32-S3 HTTP client
    ct = resp.headers.get("Content-Type", "MISSING")
    cl = resp.headers.get("Content-Length", None)
    te = resp.headers.get("Transfer-Encoding", None)
    etag = resp.headers.get("ETag", None)
    cc = resp.headers.get("Cache-Control", None)

    print(f"  Content-Type:      {ct}")
    print(f"  Content-Length:    {cl if cl else '⚠️  MISSING — esp_jpeg cannot pre-allocate!'}")
    if te:
        print(f"  Transfer-Encoding: {te}  ⚠️  chunked breaks platform_http_idf buffer alloc")
    if etag:
        print(f"  ETag:              {etag}  ✅ cache revalidation possible")
    if cc:
        print(f"  Cache-Control:     {cc}")

    data = resp.content
    size_kb = len(data) / 1024
    fmt = detect_format(data)

    print(f"  Actual size:       {len(data)} bytes ({size_kb:.1f} KB)")
    print(f"  Image format:      {fmt}")

    dims = None
    if fmt == "JPEG":
        dims = jpeg_dimensions(data)
    elif fmt == "PNG":
        dims = png_dimensions(data)
    if dims:
        print(f"  Dimensions:        {dims[0]}x{dims[1]} px")

    # Compatibility verdict
    print()
    ok = True
    if cl is None:
        print("  ❌ No Content-Length — must add workaround in http_get() to buffer until EOF")
        ok = False
    else:
        print("  ✅ Content-Length present")

    if te and "chunked" in te.lower():
        print("  ❌ Chunked encoding — platform_http_idf allocates on Content-Length")
        ok = False

    if fmt == "JPEG":
        print("  ✅ JPEG — decodable with esp_jpeg (JPEG_PIXEL_FORMAT_RGB565_BE)")
    elif fmt == "PNG":
        print("  ⚠️  PNG — heavier decode; consider server-side re-encode to JPEG")
    else:
        print(f"  ❌ {fmt} — not directly usable; needs conversion")

    if len(data) <= MAX_SAFE_BYTES:
        print(f"  ✅ Size {size_kb:.1f} KB ≤ {MAX_SAFE_BYTES//1024} KB safe cap")
    else:
        print(f"  ❌ Size {size_kb:.1f} KB > {MAX_SAFE_BYTES//1024} KB — will pressure PSRAM")

    if ok and fmt == "JPEG":
        print(f"\n  Overall: ✅ COMPATIBLE with esp_jpeg + LVGL canvas render")
    else:
        print(f"\n  Overall: ⚠️  NEEDS WORKAROUND — see notes above")

    return data


def try_resize_params(base_url: str):
    """Try common resize query param conventions to see if the server accepts them."""
    print("\n--- Testing resize query params ---")
    conventions = [
        ("width/height", {"width": 180, "height": 180}),
        ("w/h", {"w": 180, "h": 180}),
        ("size", {"size": 180}),
        ("resize", {"resize": "180x180"}),
    ]
    # Strip existing query string for clean test
    clean_url = base_url.split("?")[0]
    for label, params in conventions:
        try:
            resp = requests.get(clean_url, params=params, timeout=5, allow_redirects=True)
            cl = resp.headers.get("Content-Length")
            size = len(resp.content)
            print(f"  {label:16s}  HTTP {resp.status_code}  size={size}B  Content-Length={'yes' if cl else 'no'}")
        except Exception as e:
            print(f"  {label:16s}  ERROR: {e}")


def main():
    hr("=")
    print("KEF LSX II Album Art — ESP32-S3 Compatibility Probe")
    hr("=")
    print(f"\nSpeaker: {SPEAKER_IP}\n")

    # Step 1: full player data dump
    hr()
    print("1. Fetching player:player/data")
    hr()
    try:
        data = get_player_data()
    except Exception as e:
        print(f"❌ Cannot reach speaker: {e}")
        sys.exit(1)

    # Pretty-print the full response for inspection
    print(json.dumps(data, indent=2))

    # Step 2: extract the fields we care about
    hr()
    print("2. Extracting artwork fields")
    hr()

    state = data.get("state", "MISSING")
    track_roles = data.get("trackRoles", {})
    title = track_roles.get("title", "")
    icon = track_roles.get("icon", None)
    media_data = track_roles.get("mediaData", {})
    meta = media_data.get("metaData", {})
    artist = meta.get("artist", "")
    album = meta.get("album", "")
    service_id = meta.get("serviceID", "")

    print(f"  state:      {state}")
    print(f"  title:      {title}")
    print(f"  artist:     {artist}")
    print(f"  album:      {album}")
    print(f"  serviceID:  {service_id}")
    print(f"  icon (cover_url): {icon}")

    # Also scan for any other URL-like fields in the full tree
    raw_str = json.dumps(data)
    url_fields = [w for w in raw_str.replace('"', ' ').split() if w.startswith("http")]
    if url_fields:
        print(f"\n  All HTTP URLs found in response:")
        for u in sorted(set(url_fields)):
            print(f"    {u}")

    if not icon:
        print("\n⚠️  No icon/cover URL in player data.")
        print("   Possible reasons:")
        print("   - Speaker is in standby or paused with no track loaded")
        print("   - Spotify Connect does not populate trackRoles.icon for this firmware")
        print("   - Field name differs — check full JSON dump above for image URLs")
        sys.exit(0)

    # Step 3: probe the cover URL
    hr()
    print("3. Probing cover URL")
    hr()
    image_data = probe_url(icon, label="native")

    # Step 4: try resize params if we got something
    if image_data:
        try_resize_params(icon)

    # Step 5: summary for ESP32-S3 implementation
    hr("=")
    print("4. Implementation notes for ESP32-S3 / SH8601")
    hr("=")
    print("""
  Rendering path (from image_render.md):
    1. HTTP GET cover_url -> buffer (pre-alloc from Content-Length)
    2. esp_jpeg decode with JPEG_PIXEL_FORMAT_RGB565_BE (big-endian, matches SH8601)
    3. Blit decoded buffer into LVGL canvas or lv_img descriptor
    4. Place at target size (128x128 fast / 180x180 balanced) on 360x360 round display

  C implementation sketch:
    // In networkTask() — fetch bytes into PSRAM buffer:
    uint8_t *jpeg_buf = heap_caps_malloc(content_length, MALLOC_CAP_SPIRAM);
    http_get(cover_url, jpeg_buf, content_length);

    // Decode on Core 1 (LVGL thread) via posted message:
    esp_jpeg_image_cfg_t cfg = {
        .indata = jpeg_buf,  .indata_size = content_length,
        .outbuf = rgb565_buf, .outbuf_size = W*H*2,
        .out_format = JPEG_PIXEL_FORMAT_RGB565_BE,
        .out_scale = JPEG_IMAGE_SCALE_0,  // or SCALE_1/2 for 50%
    };
    esp_jpeg_process(&cfg, &img_out);
    free(jpeg_buf);

    // Set on LVGL canvas (Core 1 only):
    lv_canvas_set_buffer(canvas, rgb565_buf, W, H, LV_IMG_CF_TRUE_COLOR);
""")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n\nCancelled.")
    except Exception as e:
        print(f"\n❌ Unexpected error: {e}")
        raise
