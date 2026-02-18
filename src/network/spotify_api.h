#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * Spotify Web API wrapper — now-playing data for USB source mode.
 *
 * Requires Authorization Code Flow credentials in config_local.h:
 *   SPOTIFY_CLIENT_ID, SPOTIFY_CLIENT_SECRET, SPOTIFY_REFRESH_TOKEN
 *
 * See CLAUDE.md "Spotify Setup" for how to obtain a refresh token.
 * Call only from the network task (Core 0).
 */

/**
 * Store credentials. Call once before spotify_get_now_playing().
 */
void spotify_init(const char *client_id,
                  const char *client_secret,
                  const char *refresh_token);

/**
 * Fetch the currently-playing track from Spotify.
 *
 * @param title/artist/cover_url  Output buffers — filled on success.
 * @param out_playing             true if actively playing, false if paused.
 * @param out_nothing             Set true on HTTP 204 (no active playback).
 *                                Caller should clear the display when true.
 *
 * Returns true  — valid track data written to output buffers.
 * Returns false — either a network error (keep previous display state)
 *                 or nothing playing (*out_nothing = true, clear display).
 */
bool spotify_get_now_playing(char *title,        size_t title_len,
                              char *artist,       size_t artist_len,
                              char *cover_url,    size_t cover_url_len,
                              bool     *out_playing,
                              bool     *out_nothing,
                              uint32_t *out_progress_ms,
                              uint32_t *out_duration_ms);

/**
 * Playback control — require Spotify Premium and the
 * user-modify-playback-state scope in your refresh token.
 * Call only from the network task (Core 0).
 */
bool spotify_play();
bool spotify_pause();
bool spotify_next();
bool spotify_previous();
