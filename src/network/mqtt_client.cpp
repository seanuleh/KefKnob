#include "mqtt_client.h"
#include "config.h"

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ---------------------------------------------------------------------------
// Exported volatile state (definitions)
// ---------------------------------------------------------------------------

volatile bool  g_light_on          = false;
volatile int   g_light_brightness  = 127;
volatile int   g_light_colortemp   = 370;
volatile float g_light_color_hue   = 0.0f;
volatile float g_light_color_sat   = 0.0f;
volatile bool  g_light_state_dirty = false;

// ---------------------------------------------------------------------------
// Module-private state
// ---------------------------------------------------------------------------

static WiFiClient   s_wifi_client;
static PubSubClient s_mqtt(s_wifi_client);

static bool     s_enabled           = false;
static char     s_broker_ip[32]     = "";
static int      s_broker_port       = 1883;
static uint32_t s_last_reconnect_ms = 0;

// ---------------------------------------------------------------------------
// MQTT receive callback — fires on Core 0 inside mqtt_client_loop()
// ---------------------------------------------------------------------------

static void on_message(const char *topic, uint8_t *payload, unsigned int len) {
    // Only handle the light state topic
    if (strncmp(topic, MQTT_LIGHT_TOPIC, strlen(MQTT_LIGHT_TOPIC)) != 0) return;
    if (len == 0 || len > 1024) return;

    char buf[1025];
    memcpy(buf, payload, len);
    buf[len] = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, buf) != DeserializationError::Ok) return;

    if (!doc["state"].isNull()) {
        const char *s = doc["state"].as<const char *>();
        if (s) g_light_on = (strcmp(s, "ON") == 0);
    }
    if (!doc["brightness"].isNull()) {
        g_light_brightness = doc["brightness"].as<int>();
    }
    if (!doc["color_temp"].isNull()) {
        g_light_colortemp = doc["color_temp"].as<int>();
    }
    if (!doc["color"].isNull()) {
        JsonObject c = doc["color"].as<JsonObject>();
        if (!c["hue"].isNull())        g_light_color_hue = c["hue"].as<float>();
        if (!c["saturation"].isNull()) g_light_color_sat = c["saturation"].as<float>();
    }

    g_light_state_dirty = true;
    DEBUG_PRINTF("[MQTT] State: on=%d bri=%d ct=%d\n",
                 (int)g_light_on, (int)g_light_brightness, (int)g_light_colortemp);
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static bool do_connect() {
    if (!s_mqtt.connect("deskknob-light")) return false;
    s_mqtt.subscribe(MQTT_LIGHT_TOPIC);
    DEBUG_PRINTF("[MQTT] Connected and subscribed to %s\n", MQTT_LIGHT_TOPIC);
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void mqtt_client_begin(const char *broker_ip, int port) {
    if (!broker_ip || broker_ip[0] == '\0') {
        s_enabled = false;
        DEBUG_PRINTLN("[MQTT] Disabled (no broker IP configured)");
        return;
    }

    strncpy(s_broker_ip, broker_ip, sizeof(s_broker_ip) - 1);
    s_broker_ip[sizeof(s_broker_ip) - 1] = '\0';
    s_broker_port = port;

    s_mqtt.setServer(s_broker_ip, (uint16_t)s_broker_port);
    s_mqtt.setCallback(on_message);
    s_mqtt.setBufferSize(1024);  // Z2M state payloads include OTA URLs, easily >512 bytes
    s_enabled = true;

    DEBUG_PRINTF("[MQTT] Broker: %s:%d\n", s_broker_ip, s_broker_port);
    do_connect();
}

void mqtt_client_loop() {
    if (!s_enabled) return;

    if (!s_mqtt.connected()) {
        uint32_t now = (uint32_t)millis();
        if (now - s_last_reconnect_ms >= 5000) {
            s_last_reconnect_ms = now;
            if (do_connect()) {
                s_last_reconnect_ms = 0;
            }
        }
        return;  // skip loop() while disconnected
    }

    s_mqtt.loop();
}

bool mqtt_light_publish(const char *json_payload) {
    if (!s_enabled || !s_mqtt.connected()) return false;
    bool ok = s_mqtt.publish(MQTT_LIGHT_SET_TOPIC, json_payload);
    if (ok) {
        DEBUG_PRINTF("[MQTT] Published: %s\n", json_payload);
    }
    return ok;
}
