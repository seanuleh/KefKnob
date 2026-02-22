#pragma once

#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Volatile light state — written by Core 0 (MQTT receive callback),
// read by Core 1 (loop).  Single-byte/int volatile reads are atomic on ESP32.
// ---------------------------------------------------------------------------

extern volatile bool  g_light_on;
extern volatile int   g_light_brightness;   // 0–254
extern volatile int   g_light_colortemp;    // Mired 153–500
extern volatile float g_light_color_hue;    // 0–360
extern volatile float g_light_color_sat;    // 0–100
extern volatile bool  g_light_state_dirty;  // Core 0 → Core 1 paint signal

// ---------------------------------------------------------------------------
// API — all functions must be called from Core 0 (networkTask)
// ---------------------------------------------------------------------------

// Connect to broker and subscribe to the light state topic.
// No-op if broker_ip is null or empty (MQTT_BROKER_IP not set in config_local.h).
void mqtt_client_begin(const char *broker_ip, int port);

// Process incoming messages and maintain the connection.
// Call every networkTask loop iteration.
void mqtt_client_loop();

// Publish a JSON payload to the light /set topic.
// Returns true on success.  False if disconnected or MQTT disabled.
bool mqtt_light_publish(const char *json_payload);
