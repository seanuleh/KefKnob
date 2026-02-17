#include "kef_api.h"
#include "config.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <ArduinoJson.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool http_get(const char *url, String &response_out) {
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT);
    http.begin(url);

    int code = http.GET();
    if (code != 200) {
        DEBUG_PRINTF("[KEF] HTTP error %d for %s\n", code, url);
        http.end();
        return false;
    }

    response_out = http.getString();
    http.end();
    return true;
}

// ---------------------------------------------------------------------------
// Volume
// ---------------------------------------------------------------------------

bool kef_get_volume(int *out_volume) {
    char url[256];
    snprintf(url, sizeof(url),
             "http://" KEF_SPEAKER_IP "/api/getData?path=player%%3Avolume&roles=value");

    String body;
    if (!http_get(url, body)) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        DEBUG_PRINTF("[KEF] JSON parse error (volume): %s\n", err.c_str());
        return false;
    }

    if (!doc.is<JsonArray>() || doc.size() == 0) {
        DEBUG_PRINTLN("[KEF] Unexpected volume response");
        return false;
    }

    JsonObject obj = doc[0];
    if (!obj["i32_"].is<int>()) {
        DEBUG_PRINTLN("[KEF] Volume key missing");
        return false;
    }

    *out_volume = obj["i32_"].as<int>();
    return true;
}

bool kef_set_volume(int volume) {
    if (volume < VOLUME_MIN) volume = VOLUME_MIN;
    if (volume > VOLUME_MAX) volume = VOLUME_MAX;

    char url[256];
    snprintf(url, sizeof(url),
             "http://" KEF_SPEAKER_IP
             "/api/setData?path=player%%3Avolume&roles=value"
             "&value=%%7B%%22type%%22%%3A%%22i32_%%22%%2C%%22i32_%%22%%3A%d%%7D",
             volume);

    String body;
    bool ok = http_get(url, body);
    if (ok) {
        DEBUG_PRINTF("[KEF] Volume set to %d\n", volume);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Player data (title, artist, state, cover URL)
// ---------------------------------------------------------------------------

bool kef_get_player_data(char *title, size_t title_len,
                         char *artist, size_t artist_len,
                         bool *out_playing,
                         bool *out_is_standby,
                         char *cover_url, size_t cover_url_len) {
    char url[256];
    snprintf(url, sizeof(url),
             "http://" KEF_SPEAKER_IP
             "/api/getData?path=player%%3Aplayer%%2Fdata&roles=value");

    String body;
    if (!http_get(url, body)) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        DEBUG_PRINTF("[KEF] JSON parse error (player data): %s\n", err.c_str());
        return false;
    }

    if (!doc.is<JsonArray>() || doc.size() == 0) {
        DEBUG_PRINTLN("[KEF] Unexpected player data response");
        return false;
    }

    JsonObject root = doc[0];

    const char *state = root["state"] | "unknown";
    *out_playing    = (strcmp(state, "playing") == 0);
    // "stopped" = idle/standby, "standby" = deep standby — both mean off
    *out_is_standby = (strcmp(state, "playing") != 0 && strcmp(state, "pause") != 0);

    const char *t = root["trackRoles"]["title"] | "--";
    strncpy(title, t, title_len - 1);
    title[title_len - 1] = '\0';

    const char *a = root["trackRoles"]["mediaData"]["metaData"]["artist"] | "--";
    strncpy(artist, a, artist_len - 1);
    artist[artist_len - 1] = '\0';

    // Album art URL: trackRoles.icon (direct HTTPS CDN URL from Spotify)
    const char *icon = root["trackRoles"]["icon"] | "";
    strncpy(cover_url, icon, cover_url_len - 1);
    cover_url[cover_url_len - 1] = '\0';

    DEBUG_PRINTF("[KEF] State: %s | Now playing: %s - %s\n", state, title, artist);
    return true;
}

// ---------------------------------------------------------------------------
// Track control
// ---------------------------------------------------------------------------

bool kef_track_control(const char *cmd) {
    char url[256];
    snprintf(url, sizeof(url),
             "http://" KEF_SPEAKER_IP
             "/api/setData?path=player%%3Aplayer%%2Fcontrol&roles=activate"
             "&value=%%7B%%22control%%22%%3A%%22%s%%22%%7D",
             cmd);

    String body;
    bool ok = http_get(url, body);
    if (ok) {
        DEBUG_PRINTF("[KEF] Track control: %s\n", cmd);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Source
// ---------------------------------------------------------------------------

bool kef_get_source(char *source, size_t source_len) {
    char url[256];
    snprintf(url, sizeof(url),
             "http://" KEF_SPEAKER_IP
             "/api/getData?path=settings%%3A%%2Fkef%%2Fplay%%2FphysicalSource&roles=value");

    String body;
    if (!http_get(url, body)) return false;

    JsonDocument doc;
    if (deserializeJson(doc, body) || !doc.is<JsonArray>() || doc.size() == 0)
        return false;

    const char *src = doc[0]["kefPhysicalSource"] | "";
    strncpy(source, src, source_len - 1);
    source[source_len - 1] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
// Mute
// ---------------------------------------------------------------------------

bool kef_set_mute(bool muted) {
    // value={"type":"bool_","bool_":true/false}
    char url[320];
    snprintf(url, sizeof(url),
             "http://" KEF_SPEAKER_IP
             "/api/setData?path=settings%%3A%%2FmediaPlayer%%2Fmute&roles=value"
             "&value=%%7B%%22type%%22%%3A%%22bool_%%22%%2C%%22bool_%%22%%3A%s%%7D",
             muted ? "true" : "false");

    String body;
    bool ok = http_get(url, body);
    if (ok) DEBUG_PRINTF("[KEF] Mute set to %s\n", muted ? "true" : "false");
    return ok;
}

// ---------------------------------------------------------------------------
// Power
// ---------------------------------------------------------------------------

bool kef_get_power(bool *out_is_on) {
    char url[256];
    snprintf(url, sizeof(url),
             "http://" KEF_SPEAKER_IP
             "/api/getData?path=player%%3Apower&roles=value");

    String body;
    if (!http_get(url, body)) return false;

    JsonDocument doc;
    if (deserializeJson(doc, body) || !doc.is<JsonArray>() || doc.size() == 0)
        return false;

    const char *ps = doc[0]["kefPowerState"] | "standby";
    *out_is_on = (strcmp(ps, "on") == 0);
    return true;
}

bool kef_get_speaker_status(bool *out_is_on) {
    // Uses settings:/kef/host/speakerStatus — returns kefSpeakerStatus="powerOn"
    // when speaker is physically on, regardless of playback state.
    // This is the reliable way to detect power state; player "state" field reports
    // "stopped" for both powered-off AND powered-on-but-idle, making it unusable
    // for standby detection.
    char url[256];
    snprintf(url, sizeof(url),
             "http://" KEF_SPEAKER_IP
             "/api/getData?path=settings%%3A%%2Fkef%%2Fhost%%2FspeakerStatus&roles=value");

    String body;
    if (!http_get(url, body)) return false;

    JsonDocument doc;
    if (deserializeJson(doc, body) || !doc.is<JsonArray>() || doc.size() == 0)
        return false;

    const char *status = doc[0]["kefSpeakerStatus"] | "";
    *out_is_on = (strcmp(status, "powerOn") == 0);
    DEBUG_PRINTF("[KEF] Speaker status: %s\n", status);
    return true;
}

bool kef_set_power(bool on) {
    // Power on/off both go through physicalSource with roles=value.
    // Power on  = physicalSource "powerOn"
    // Power off = physicalSource "standby"
    return kef_set_source(on ? "powerOn" : "standby");
}

bool kef_power_on() {
    // Wake from stopped/standby using roles=value with "powerOn" source.
    // This is the only mechanism that works when the speaker is in stopped state
    // (roles=activate returns HTTP 500 in that state).
    char url[320];
    snprintf(url, sizeof(url),
             "http://" KEF_SPEAKER_IP
             "/api/setData?path=settings%%3A%%2Fkef%%2Fplay%%2FphysicalSource&roles=value"
             "&value=%%7B%%22type%%22%%3A%%22kefPhysicalSource%%22%%2C%%22kefPhysicalSource%%22%%3A%%22powerOn%%22%%7D");

    String body;
    bool ok = http_get(url, body);
    if (ok) DEBUG_PRINTLN("[KEF] Wake (powerOn) sent");
    else    DEBUG_PRINTLN("[KEF] Wake (powerOn) failed");
    return ok;
}

// ---------------------------------------------------------------------------
// Source switching
// ---------------------------------------------------------------------------

bool kef_set_source(const char *source) {
    // All physicalSource commands (source switch, power on, standby) use roles=value.
    // roles=activate returns HTTP 500 on this firmware — confirmed via pykefcontrol.
    char url[320];
    snprintf(url, sizeof(url),
             "http://" KEF_SPEAKER_IP
             "/api/setData?path=settings%%3A%%2Fkef%%2Fplay%%2FphysicalSource&roles=value"
             "&value=%%7B%%22type%%22%%3A%%22kefPhysicalSource%%22%%2C%%22kefPhysicalSource%%22%%3A%%22%s%%22%%7D",
             source);

    String body;
    bool ok = http_get(url, body);
    if (ok) DEBUG_PRINTF("[KEF] Source set to %s\n", source);
    else    DEBUG_PRINTF("[KEF] Source set FAILED for %s\n", source);
    return ok;
}

// ---------------------------------------------------------------------------
// JPEG fetch (HTTPS) — allocates into PSRAM, caller must free()
// ---------------------------------------------------------------------------

uint8_t *kef_fetch_jpeg(const char *url, size_t *out_size) {
    NetworkClientSecure client;
    client.setInsecure();  // Album art: cert verification not required

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT);
    if (!http.begin(client, url)) {
        DEBUG_PRINTF("[Art] http.begin failed for %s\n", url);
        return nullptr;
    }

    int code = http.GET();
    if (code != 200) {
        DEBUG_PRINTF("[Art] HTTP %d fetching JPEG\n", code);
        http.end();
        return nullptr;
    }

    // Reject non-JPEG and oversize responses
    String ct = http.header("Content-Type");
    int content_len = http.getSize();
    if (content_len <= 0 || content_len > (int)ALBUM_ART_MAX_JPEG) {
        DEBUG_PRINTF("[Art] Bad size %d (max %d)\n", content_len, ALBUM_ART_MAX_JPEG);
        http.end();
        return nullptr;
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(content_len, MALLOC_CAP_SPIRAM);
    if (!buf) {
        DEBUG_PRINTLN("[Art] PSRAM alloc failed for JPEG");
        http.end();
        return nullptr;
    }

    Stream *stream = http.getStreamPtr();
    int total = 0;
    while (total < content_len) {
        int avail = stream->available();
        if (avail > 0) {
            int chunk = stream->readBytes(buf + total, min(avail, content_len - total));
            total += chunk;
        } else {
            delay(1);
        }
    }

    http.end();

    if (total != content_len) {
        DEBUG_PRINTF("[Art] Short read: got %d of %d bytes\n", total, content_len);
        free(buf);
        return nullptr;
    }

    *out_size = (size_t)content_len;
    DEBUG_PRINTF("[Art] Fetched %d bytes JPEG\n", content_len);
    return buf;
}
