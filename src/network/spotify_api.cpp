#include "spotify_api.h"
#include "config.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>

// ---------------------------------------------------------------------------
// Credentials + token state
// ---------------------------------------------------------------------------

static char s_client_id[64]      = "";
static char s_client_secret[64]  = "";
static char s_refresh_token[256] = "";
static char s_access_token[256]  = "";
static uint32_t s_token_exp_ms   = 0;   // millis() at which to proactively refresh

void spotify_init(const char *client_id,
                  const char *client_secret,
                  const char *refresh_token) {
    strncpy(s_client_id,     client_id,     sizeof(s_client_id)     - 1);
    strncpy(s_client_secret, client_secret, sizeof(s_client_secret) - 1);
    strncpy(s_refresh_token, refresh_token, sizeof(s_refresh_token) - 1);
}

// ---------------------------------------------------------------------------
// Token refresh (POST accounts.spotify.com/api/token)
// ---------------------------------------------------------------------------

static bool do_token_refresh() {
    if (!s_client_id[0] || !s_client_secret[0] || !s_refresh_token[0]) {
        DEBUG_PRINTLN("[Spotify] Credentials not configured — skipping");
        return false;
    }

    // Base64-encode "client_id:client_secret" for Basic auth
    char creds[128];
    snprintf(creds, sizeof(creds), "%s:%s", s_client_id, s_client_secret);
    uint8_t b64[192];
    size_t  b64_len = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &b64_len,
                          (const uint8_t *)creds, strlen(creds));
    b64[b64_len] = '\0';

    NetworkClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT);

    if (!http.begin(client, "https://accounts.spotify.com/api/token")) {
        DEBUG_PRINTLN("[Spotify] Token refresh: http.begin failed");
        return false;
    }
    http.addHeader("Authorization",
                   String("Basic ") + (const char *)b64);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    char body[320];
    snprintf(body, sizeof(body),
             "grant_type=refresh_token&refresh_token=%s", s_refresh_token);

    int code = http.POST(body);
    if (code != 200) {
        DEBUG_PRINTF("[Spotify] Token refresh HTTP %d\n", code);
        http.end();
        return false;
    }

    String resp = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, resp)) {
        DEBUG_PRINTLN("[Spotify] Token refresh JSON parse error");
        return false;
    }

    const char *tok = doc["access_token"] | "";
    if (!tok[0]) {
        DEBUG_PRINTLN("[Spotify] access_token missing in response");
        return false;
    }

    strncpy(s_access_token, tok, sizeof(s_access_token) - 1);
    int expires_in = doc["expires_in"] | 3600;
    // Refresh 5 minutes before actual expiry
    s_token_exp_ms = (uint32_t)millis() + (uint32_t)(expires_in - 300) * 1000;
    DEBUG_PRINTLN("[Spotify] Token refreshed");
    return true;
}

static bool ensure_token() {
    if (!s_access_token[0] || (uint32_t)millis() >= s_token_exp_ms) {
        return do_token_refresh();
    }
    return true;
}

// ---------------------------------------------------------------------------
// Now-playing (GET api.spotify.com/v1/me/player/currently-playing)
// ---------------------------------------------------------------------------

bool spotify_get_now_playing(char *title,        size_t title_len,
                              char *artist,       size_t artist_len,
                              char *cover_url,    size_t cover_url_len,
                              bool     *out_playing,
                              bool     *out_nothing,
                              uint32_t *out_progress_ms,
                              uint32_t *out_duration_ms) {
    *out_playing     = false;
    *out_nothing     = false;
    *out_progress_ms = 0;
    *out_duration_ms = 0;

    if (!ensure_token()) return false;

    NetworkClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT);

    if (!http.begin(client,
                    "https://api.spotify.com/v1/me/player/currently-playing")) {
        DEBUG_PRINTLN("[Spotify] now-playing: http.begin failed");
        return false;
    }
    http.addHeader("Authorization",
                   String("Bearer ") + s_access_token);

    int code = http.GET();

    if (code == 204) {
        // No active playback session
        DEBUG_PRINTLN("[Spotify] 204 — nothing playing");
        http.end();
        *out_nothing = true;
        return false;
    }

    if (code == 401) {
        // Token was rejected — refresh now so the next poll succeeds
        http.end();
        DEBUG_PRINTLN("[Spotify] 401 — forcing token refresh");
        s_access_token[0] = '\0';
        do_token_refresh();
        return false;
    }

    if (code != 200) {
        DEBUG_PRINTF("[Spotify] now-playing HTTP %d\n", code);
        http.end();
        return false;
    }

    // Filter: parse only the fields we actually use, avoiding buffering the
    // full large response body (~3-5 KB) into a heap String.
    JsonDocument filter;
    filter["is_playing"]                        = true;
    filter["progress_ms"]                       = true;
    filter["item"]["name"]                      = true;
    filter["item"]["duration_ms"]               = true;
    filter["item"]["artists"][0]["name"]        = true;
    filter["item"]["album"]["images"][0]["url"] = true;

    JsonDocument doc;
    Stream *stream = http.getStreamPtr();
    DeserializationError err = deserializeJson(doc, *stream,
                                               DeserializationOption::Filter(filter));
    http.end();

    if (err) {
        DEBUG_PRINTF("[Spotify] JSON parse error: %s\n", err.c_str());
        return false;
    }

    *out_playing     = doc["is_playing"]       | false;
    *out_progress_ms = doc["progress_ms"]      | 0;
    *out_duration_ms = doc["item"]["duration_ms"] | 0;

    const char *t = doc["item"]["name"] | "";
    strncpy(title, t[0] ? t : "--", title_len - 1);
    title[title_len - 1] = '\0';

    const char *a = doc["item"]["artists"][0]["name"] | "";
    strncpy(artist, a[0] ? a : "--", artist_len - 1);
    artist[artist_len - 1] = '\0';

    // First image is always the largest (640×640 from Spotify CDN)
    const char *img = doc["item"]["album"]["images"][0]["url"] | "";
    strncpy(cover_url, img, cover_url_len - 1);
    cover_url[cover_url_len - 1] = '\0';

    DEBUG_PRINTF("[Spotify] %s - %s (%s)\n",
                 title, artist, *out_playing ? "playing" : "paused");
    return true;
}

// ---------------------------------------------------------------------------
// Playback control (PUT/POST api.spotify.com/v1/me/player/*)
// Requires user-modify-playback-state scope + Spotify Premium.
// ---------------------------------------------------------------------------

static bool do_playback_cmd(const char *method, const char *path) {
    if (!ensure_token()) return false;

    NetworkClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT);

    String url = String("https://api.spotify.com") + path;
    if (!http.begin(client, url)) {
        DEBUG_PRINTF("[Spotify] %s %s: http.begin failed\n", method, path);
        return false;
    }
    http.addHeader("Authorization", String("Bearer ") + s_access_token);
    http.addHeader("Content-Length", "0");

    int code;
    if (strcmp(method, "PUT") == 0) {
        code = http.PUT((uint8_t *)"", 0);
    } else {
        code = http.POST((uint8_t *)"", 0);
    }
    http.end();

    bool ok = (code == 200 || code == 204);
    if (!ok) DEBUG_PRINTF("[Spotify] %s %s → HTTP %d\n", method, path, code);
    return ok;
}

bool spotify_play()     { return do_playback_cmd("PUT",  "/v1/me/player/play"); }
bool spotify_pause()    { return do_playback_cmd("PUT",  "/v1/me/player/pause"); }
bool spotify_next()     { return do_playback_cmd("POST", "/v1/me/player/next"); }
bool spotify_previous() { return do_playback_cmd("POST", "/v1/me/player/previous"); }
