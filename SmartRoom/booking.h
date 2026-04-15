#ifndef BOOKING_H
#define BOOKING_H

#include <Arduino.h>
#include <time.h>

// ── FSM states ────────────────────────────────────────────────────────────────
typedef enum {
  STATE_SCHEDULED = 0,   // Booking exists, start time not yet reached
  STATE_PENDING   = 1,   // Start time reached, waiting for occupancy confirmation
  STATE_ACTIVE    = 2,   // Occupancy confirmed, session running
  STATE_GHOST     = 3,   // Grace period expired with no presence — room released
  STATE_COMPLETED = 4    // Session ended normally
} FSMState;

// ── Event types published to MQTT broker ─────────────────────────────────────
typedef enum {
  EVT_OCCUPANCY_CONFIRMED = 0,
  EVT_GHOST_RELEASED      = 1,
  EVT_SESSION_COMPLETED   = 2,
  EVT_WALK_UP_BOOKING     = 3
} EventType;

// ── Single booking slot ───────────────────────────────────────────────────────
// bookingId is 40 chars to hold a full UUID (36 chars + null terminator + margin)
struct BookingSlot {
  char          bookingId[40];     // UUID from cloud backend e.g. "aaba834a-51ed-46c7-9512-60d5f696cff2"
  char          occupantName[32];  // Display name of person who booked
  char          title[40];         // Booking purpose / title (e.g. "Meeting", "Class")
  time_t        startTime;         // Unix timestamp
  time_t        endTime;           // Unix timestamp
  FSMState      state;
  bool          active;            // False = slot can be overwritten
  unsigned long pendingStartMs;    // Per-slot grace-period anchor (millis at PENDING entry)
  bool          buzzerFired;       // Per-slot 5-min warning latch
};

// ── Outbound event (for store-and-forward queue) ──────────────────────────────
// roomId is 40 chars to hold a full UUID room ID
struct FsmEvent {
  EventType type;
  char      bookingId[40];   // UUID (or local walk-up id like "wu_123")
  time_t    timestamp;
  char      roomId[40];      // UUID
  // Extra context for walk-up events — unused for other event types.
  char      title[40];
  char      occupantName[32];
  time_t    startTime;
  time_t    endTime;
};

// ── Maximum booking slots held in memory ─────────────────────────────────────
// Must be >= the backend snapshot limit in mqttBridge.js (currently 20).
#define MAX_SLOTS 24

#endif
