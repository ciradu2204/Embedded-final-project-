#ifndef CONFIG_H
#define CONFIG_H

// ════════════════════════════════════════════════════════════════════
//  SmartRoom ESP32 Configuration 
// ════════════════════════════════════════════════════════════════════

// ── WiFi ─────────────────────────────────────────────────────────────
#define WIFI_SSID     "Michael's S21 Ultra"
#define WIFI_PASSWORD "kjwu2737"

// ── HiveMQ Cloud ─────────────────────────────────────────────────────
#define MQTT_HOST  "f44c52fe5a9c41f99db1e4b17a8e84e2.s1.eu.hivemq.cloud"
#define MQTT_PORT  8883
#define MQTT_USER  "smartroom"
#define MQTT_PASS  "ESD2026a"

// ── Room identity — set by teammate to match backend UUID ─────────────
#define ROOM_ID    "aaba834a-51ed-46c7-9512-60d5f696cff2"
#define ROOM_NAME  "Room A203"

// ── MQTT topics ───────────────────────────────────────────────────────
// Updated to ensure they match the database mapping exactly
#define TOPIC_BOOKING   "smartroom/" ROOM_ID "/booking"
#define TOPIC_SNAPSHOT  "smartroom/" ROOM_ID "/bookings/snapshot"
#define TOPIC_STATUS    "smartroom/" ROOM_ID "/status"

// ── GPIO pins ─────────────────────────────────────────────────────────
#define PIN_GREEN_LED   25
#define PIN_RED_LED     26
#define PIN_BUZZER      27
#define PIN_PIR         34

// UART2 to Arduino Mega
#define UART_MEGA_RX    16
#define UART_MEGA_TX    17
#define UART_MEGA_BAUD  115200

// ── FSM timing ────────────────────────────────────────────────────────
#define GRACE_PERIOD_MS    (10UL * 60 * 1000)
#define BUZZER_WARNING_MS  ( 5UL * 60 * 1000)
#define PIR_DEBOUNCE_MS    3000

// ── Buzzer ────────────────────────────────────────────────────────────
#define ACTIVE_BUZZER   0
#define BUZZER_FREQ     2300

// ── PWM settings ──────────────────────────────────────────────────────
#define PWM_FREQ_LED    5000
#define PWM_RESOLUTION  8

// ── Event buffer ──────────────────────────────────────────────────────
#define EVENT_BUFFER_SIZE  20

// ── NVS ───────────────────────────────────────────────────────────────
#define NVS_NAMESPACE    "smartroom"
#define NVS_KEY_BOOKINGS "bookings"

#endif
