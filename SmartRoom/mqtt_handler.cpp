#include "mqtt_handler.h"
#include "config.h"
#include "event_queue.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <time.h>

/*
 * MQTT TLS connection to HiveMQ Cloud.
 * setInsecure() skips certificate verification — acceptable for course project.
 *
 * PubSubClient state codes:
 *  -2 = MQTT_CONNECT_FAILED  (TLS layer failed)
 *   0 = MQTT_CONNECTED
 *   4 = MQTT_CONNECT_BAD_CREDENTIALS
 */

static WiFiClientSecure _wifiClient;
static PubSubClient     _mqtt(_wifiClient);
static BookingCallback  _bookingCb       = nullptr;
static bool             _wasConnected    = false;
static unsigned long    _lastReconnectMs = 0;
static const uint32_t   RECONNECT_INTERVAL = 5000;

// WiFi reconnect state — non-blocking
// Instead of blocking with while()+delay(), we attempt one connection step
// per mqttLoop() call, so buzzerTick(), ledTick() etc. keep running.
static bool          _wifiConnecting  = false;
static unsigned long _wifiRetryMs     = 0;
static const uint32_t WIFI_RETRY_MS   = 500;   // poll interval during connect
static uint8_t       _wifiTries       = 0;
static const uint8_t WIFI_MAX_TRIES   = 40;    // 40 * 500ms = 20s max

static void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  // Snapshot can be larger than a single booking — give it room.
  static char buf[2048];
  uint16_t len = min((unsigned int)(sizeof(buf) - 1), length);
  memcpy(buf, payload, len);
  buf[len] = '\0';
  Serial.printf("[MQTT] Received on %s (%u bytes)\n", topic, (unsigned)len);

  if (!_bookingCb) return;

  // Snapshot topic carries an array of bookings — split into single-object
  // chunks and dispatch each through the same parser used for /booking.
  if (strstr(topic, "/bookings/snapshot")) {
    const char* p = strchr(buf, '[');
    if (!p) return;
    p++;
    while (*p) {
      while (*p && *p != '{' && *p != ']') p++;
      if (*p != '{') break;
      const char* start = p;
      int depth = 0;
      while (*p) {
        if (*p == '{') depth++;
        else if (*p == '}') {
          depth--;
          if (depth == 0) { p++; break; }
        }
        p++;
      }
      size_t objLen = (size_t)(p - start);
      if (objLen > 0 && objLen < sizeof(buf) - 1) {
        char obj[512];
        if (objLen >= sizeof(obj)) objLen = sizeof(obj) - 1;
        memcpy(obj, start, objLen);
        obj[objLen] = '\0';
        _bookingCb(obj);
      }
    }
    return;
  }

  if (strstr(topic, "/booking")) {
    _bookingCb(buf);
  }
}

// ── Non-blocking WiFi connect ─────────────────────────────────────────────────
// Call this every loop iteration. Returns true when connected.
static bool stepWiFiConnect() {
  if (WiFi.status() == WL_CONNECTED) {
    _wifiConnecting = false;
    return true;
  }

  if (!_wifiConnecting) {
    // Start a new connection attempt
    Serial.printf("[WiFi] Connecting to %s\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    _wifiConnecting = true;
    _wifiTries      = 0;
    _wifiRetryMs    = millis();
    return false;
  }

  // Connecting: poll every WIFI_RETRY_MS
  if (millis() - _wifiRetryMs < WIFI_RETRY_MS) return false;
  _wifiRetryMs = millis();
  _wifiTries++;

  if (WiFi.status() == WL_CONNECTED) {
    _wifiConnecting = false;
    Serial.printf("[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
    // Sync NTP time (non-blocking config — actual sync happens in background)
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    Serial.println(F("[NTP] Time sync requested (background)."));
    return true;
  }

  if (_wifiTries >= WIFI_MAX_TRIES) {
    _wifiConnecting = false;
    _wifiTries      = 0;
    Serial.println(F("[WiFi] Connect timeout. Will retry."));
    WiFi.disconnect(true);
  }

  return false;
}

static bool connectMQTT() {
  if (_mqtt.connected()) return true;

  Serial.printf("[MQTT] Connecting to %s:%d\n", MQTT_HOST, MQTT_PORT);

  char clientId[40];
  snprintf(clientId, sizeof(clientId), "smartroom_%s_%04X",
           ROOM_ID, (uint16_t)(ESP.getEfuseMac() & 0xFFFF));

  if (_mqtt.connect(clientId, MQTT_USER, MQTT_PASS)) {
    Serial.println(F("[MQTT] Connected OK."));
    _mqtt.subscribe(TOPIC_BOOKING);
    _mqtt.subscribe(TOPIC_SNAPSHOT);
    Serial.printf("[MQTT] Subscribed: %s\n", TOPIC_BOOKING);
    Serial.printf("[MQTT] Subscribed: %s\n", TOPIC_SNAPSHOT);
    return true;
  }

  int state = _mqtt.state();
  Serial.printf("[MQTT] Failed. State: %d", state);
  if (state == 4)  Serial.print(F(" <- Wrong credentials"));
  if (state == -2) Serial.print(F(" <- TLS failed"));
  Serial.println();
  return false;
}

void mqttInit() {
  _wifiClient.setInsecure();
  _mqtt.setServer(MQTT_HOST, MQTT_PORT);
  _mqtt.setCallback(onMqttMessage);
  _mqtt.setBufferSize(2048);  // snapshot payloads can hold up to ~20 bookings
  _mqtt.setKeepAlive(30);
  _mqtt.setSocketTimeout(10);

  // Initial blocking connect only at startup (before loop runs).
  // This is the only place we block — acceptable because setup() hasn't finished yet
  // and the buzzer has not been started yet, so nothing is interrupted.
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint8_t tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < WIFI_MAX_TRIES) {
    delay(500);
    Serial.print('.');
    tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    Serial.print(F("[NTP] Syncing"));
    time_t now = 0;
    uint8_t ntpTries = 0;
    while (now < 1000000 && ntpTries < 20) {
      delay(500); Serial.print('.'); now = time(nullptr); ntpTries++;
    }
    Serial.println(now > 1000000 ? F(" OK") : F(" FAILED"));
    connectMQTT();
  } else {
    Serial.println(F("\n[WiFi] Initial connect failed. Will retry in loop."));
  }
}

// ── Main loop — fully non-blocking after setup ────────────────────────────────
void mqttLoop() {
  // Non-blocking WiFi reconnect — one small step per loop iteration
  if (WiFi.status() != WL_CONNECTED) {
    stepWiFiConnect();
    return;   // Cannot do MQTT without WiFi
  }

  if (!_mqtt.connected()) {
    if (_wasConnected) {
      Serial.println(F("[MQTT] Connection lost."));
      _wasConnected = false;
    }
    unsigned long now = millis();
    if (now - _lastReconnectMs >= RECONNECT_INTERVAL) {
      _lastReconnectMs = now;
      if (connectMQTT()) {
        _wasConnected = true;
        mqttFlushQueue();
      }
    }
  } else {
    if (!_wasConnected) {
      _wasConnected = true;
      mqttFlushQueue();
    }
    _mqtt.loop();
  }
}

bool mqttConnected() { return _mqtt.connected(); }

void mqttPublishStatus(const FsmEvent& evt) {
  // Backend (server/services/mqttBridge.js::handleRoomStatus) reads:
  //   payload.state  ∈ {"active","ghost_released","completed"}
  //   payload.timestamp (ISO string OR epoch — backend falls back to "now")
  // Map our internal event vocabulary onto that contract so events are
  // actually persisted instead of being logged as "Missing state".
  const char* state = "unknown";
  switch (evt.type) {
    case EVT_OCCUPANCY_CONFIRMED: state = "active";         break;
    case EVT_GHOST_RELEASED:      state = "ghost_released"; break;
    case EVT_SESSION_COMPLETED:   state = "completed";      break;
    // Walk-up bookings have no exact backend equivalent — represent them
    // as an "active" transition so the room is at least marked occupied.
    case EVT_WALK_UP_BOOKING:     state = "active";         break;
  }
  char payload[256];
  snprintf(payload, sizeof(payload),
           "{\"roomId\":\"%s\",\"bookingId\":\"%s\","
           "\"state\":\"%s\",\"timestamp\":%lu}",
           evt.roomId, evt.bookingId, state, (unsigned long)evt.timestamp);

  if (!_mqtt.publish(TOPIC_STATUS, payload, false)) {
    Serial.println(F("[MQTT] Publish failed — re-queuing."));
    eventQueuePush(evt);
  } else {
    Serial.printf("[MQTT] Published: %s\n", payload);
  }
}

void mqttFlushQueue() {
  if (!_mqtt.connected()) return;
  FsmEvent evt;
  uint8_t flushed = 0;
  while (eventQueuePop(evt)) {
    mqttPublishStatus(evt);
    flushed++;
    delay(50);
  }
  if (flushed) Serial.printf("[MQTT] Flushed %u queued events.\n", flushed);
}

void mqttSetBookingCallback(BookingCallback cb) { _bookingCb = cb; }
