#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * KEF HTTP API wrapper for LSX II speakers.
 *
 * All functions are synchronous and block until the HTTP request completes
 * or times out (HTTP_TIMEOUT from config.h).
 *
 * Call only from the network task (Core 0).
 */

/**
 * Get current speaker volume (0-100).
 */
bool kef_get_volume(int *out_volume);

/**
 * Set speaker volume (0-100).
 */
bool kef_set_volume(int volume);

/**
 * Get current player data: track title, artist, playback state, and cover URL.
 * Buffers are null-terminated on success. On failure the buffers are unchanged.
 *
 * @param cover_url     Output buffer for album art URL (https://i.scdn.co/...).
 *                      Set to "" when no artwork is available.
 * @param cover_url_len Size of cover_url buffer.
 */
bool kef_get_player_data(char *title, size_t title_len,
                         char *artist, size_t artist_len,
                         bool *out_playing,
                         bool *out_is_standby,
                         char *cover_url, size_t cover_url_len);

/**
 * Send a playback control command.
 * @param cmd  One of: "pause", "next", "previous"
 */
bool kef_track_control(const char *cmd);

/**
 * Get the current physical input source (e.g. "wifi", "usb", "bluetooth").
 * @param source      Output buffer.
 * @param source_len  Size of output buffer.
 */
bool kef_get_source(char *source, size_t source_len);

/**
 * Set or clear the speaker mute.
 */
bool kef_set_mute(bool muted);

/**
 * Get power state. Returns true if on (not standby).
 */
bool kef_get_power(bool *out_is_on);

/**
 * Get physical speaker power state via settings:/kef/host/speakerStatus.
 * Returns true and sets *out_is_on=true when kefSpeakerStatus=="powerOn".
 * More reliable than player state for standby detection — player state reports
 * "stopped" for both powered-off and powered-on-but-idle conditions.
 */
bool kef_get_speaker_status(bool *out_is_on);

/**
 * Set power state. true = on, false = standby.
 */
bool kef_set_power(bool on);

/**
 * Wake the speaker from stopped/standby using physicalSource="powerOn" with
 * roles=value. This is the only reliable wake method when the speaker is in
 * stopped state (roles=activate returns HTTP 500 in that state).
 */
bool kef_power_on();

/**
 * Set the physical input source.
 * @param source  "wifi", "usb", "bluetooth", "optical", etc.
 */
bool kef_set_source(const char *source);

/**
 * Fetch a JPEG from an HTTPS URL into a PSRAM buffer.
 * Caller must free() the returned pointer.
 *
 * Returns nullptr on failure (network error, too large, wrong content type).
 * @param url       Full HTTPS URL of the image.
 * @param out_size  Receives the number of bytes in the returned buffer.
 */
uint8_t *kef_fetch_jpeg(const char *url, size_t *out_size);
