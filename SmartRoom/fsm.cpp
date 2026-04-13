#include "fsm.h"
#include "config.h"
#include "event_queue.h"
#include <Arduino.h>
#include <time.h>

static BookingSlot slots[MAX_SLOTS];
static uint8_t slotCount = 0;
static unsigned long pendingStartMs = 0;
static unsigned long buzzerArmedMs  = 0;
static bool          buzzerFired    = false;

static void transitionTo(BookingSlot* slot, FSMState newState);
static BookingSlot* findSlot(const char* bookingId);
static BookingSlot* findFreeSlot();

void fsmInit() {
  memset(slots, 0, sizeof(slots));
  slotCount = 0;
  Serial.println(F("[FSM] Initialised."));
}

void fsmAddBooking(const BookingSlot& incoming) {
  BookingSlot* existing = findSlot(incoming.bookingId);
  if (existing) {
    strlcpy(existing->occupantName, incoming.occupantName, sizeof(existing->occupantName));
    existing->startTime = incoming.startTime;
    existing->endTime   = incoming.endTime;
    existing->active    = true;
    Serial.printf("[FSM] Updated booking %s\n", incoming.bookingId);
    return;
  }
  BookingSlot* slot = findFreeSlot();
  if (!slot) { Serial.println(F("[FSM] No free slot.")); return; }
  *slot = incoming;
  slot->state  = STATE_SCHEDULED;
  slot->active = true;
  slotCount++;
  Serial.printf("[FSM] Added %s for %s\n", incoming.bookingId, incoming.occupantName);
}

void fsmCancelBooking(const char* bookingId) {
  BookingSlot* slot = findSlot(bookingId);
  if (slot) {
    slot->active = false;
    slot->state  = STATE_COMPLETED;
    Serial.printf("[FSM] Cancelled %s\n", bookingId);
  }
}

void fsmTick(bool presenceDetected) {
  time_t now = time(nullptr) + 7200; // 2 hour offset for Kigali
  if (now < 1000000) return;

  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    BookingSlot* s = &slots[i];
    if (!s->active) continue;

    switch (s->state) {
      case STATE_SCHEDULED:
        if (now >= s->startTime) {
          transitionTo(s, STATE_PENDING);
          pendingStartMs = millis();
          buzzerFired = false;
          Serial.printf("[FSM] %s -> PENDING\n", s->bookingId);
        }
        break;

      case STATE_PENDING:
        if (presenceDetected) {
          transitionTo(s, STATE_ACTIVE);
          unsigned long sessionMs = ((unsigned long)(s->endTime - now)) * 1000UL;
          buzzerArmedMs = (sessionMs > BUZZER_WARNING_MS)
                          ? millis() + (sessionMs - BUZZER_WARNING_MS) : 0;
          Serial.printf("[FSM] %s -> ACTIVE\n", s->bookingId);
          FsmEvent evt;
          evt.type = EVT_OCCUPANCY_CONFIRMED;
          strlcpy(evt.bookingId, s->bookingId, sizeof(evt.bookingId));
          strlcpy(evt.roomId, ROOM_ID, sizeof(evt.roomId));
          evt.timestamp = now;
          eventQueuePush(evt);
        } else if (millis() - pendingStartMs >= GRACE_PERIOD_MS) {
          transitionTo(s, STATE_GHOST);
          Serial.printf("[FSM] %s -> GHOST\n", s->bookingId);
          FsmEvent evt;
          evt.type = EVT_GHOST_RELEASED;
          strlcpy(evt.bookingId, s->bookingId, sizeof(evt.bookingId));
          strlcpy(evt.roomId, ROOM_ID, sizeof(evt.roomId));
          evt.timestamp = now;
          eventQueuePush(evt);
          s->active = false;
        }
        break;

      case STATE_ACTIVE:
        if (buzzerArmedMs > 0 && millis() >= buzzerArmedMs && !buzzerFired) {
          buzzerFired = true;
          extern void buzzerDoubleBeep();
          buzzerDoubleBeep();
          Serial.println(F("[FSM] 5-min warning."));
        }
        if (now >= s->endTime) {
          transitionTo(s, STATE_COMPLETED);
          Serial.printf("[FSM] %s -> COMPLETED\n", s->bookingId);
          FsmEvent evt;
          evt.type = EVT_SESSION_COMPLETED;
          strlcpy(evt.bookingId, s->bookingId, sizeof(evt.bookingId));
          strlcpy(evt.roomId, ROOM_ID, sizeof(evt.roomId));
          evt.timestamp = now;
          eventQueuePush(evt);
          s->active = false;
        }
        break;

      case STATE_GHOST:
      case STATE_COMPLETED:
        break;
    }
  }
}

void fsmCreateWalkUpBooking(const char* occupantName, uint16_t durationMins) {
  BookingSlot* slot = findFreeSlot();
  if (!slot) { Serial.println(F("[FSM] No free slot for walk-up.")); return; }
  time_t now = time(nullptr) + 7200; // 2 hour offset for Kigali
  snprintf(slot->bookingId, sizeof(slot->bookingId), "wu_%lu", (unsigned long)now);
  strlcpy(slot->occupantName, occupantName, sizeof(slot->occupantName));
  slot->startTime = now;
  slot->endTime   = now + (durationMins * 60);
  slot->state     = STATE_ACTIVE;
  slot->active    = true;
  unsigned long sessionMs = (unsigned long)durationMins * 60 * 1000UL;
  buzzerArmedMs = (sessionMs > BUZZER_WARNING_MS)
                  ? millis() + (sessionMs - BUZZER_WARNING_MS) : 0;
  buzzerFired = false;
  Serial.printf("[FSM] Walk-up: %s for %u min\n", slot->bookingId, durationMins);
  FsmEvent evt;
  evt.type = EVT_WALK_UP_BOOKING;
  strlcpy(evt.bookingId, slot->bookingId, sizeof(evt.bookingId));
  strlcpy(evt.roomId, ROOM_ID, sizeof(evt.roomId));
  evt.timestamp = now;
  eventQueuePush(evt);
}

// FIX: Priority ordering: ACTIVE > PENDING > SCHEDULED > others
// Previously returned first active slot found, which could be a future SCHEDULED
// slot when an ACTIVE one also existed.
FSMState fsmGetCurrentState() {
  FSMState best = STATE_COMPLETED;  // lowest priority (= available)
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    if (!slots[i].active) continue;
    FSMState s = slots[i].state;
    // Priority: ACTIVE(2) > PENDING(1) > SCHEDULED(0) > GHOST(3) > COMPLETED(4)
    // Map to urgency numbers for comparison
    auto urgency = [](FSMState st) -> int {
      switch (st) {
        case STATE_ACTIVE:    return 4;
        case STATE_PENDING:   return 3;
        case STATE_SCHEDULED: return 2;
        case STATE_GHOST:     return 1;
        default:              return 0;
      }
    };
    if (urgency(s) > urgency(best)) best = s;
  }
  return best;
}

bool fsmRoomIsAvailable() {
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    if (slots[i].active &&
        (slots[i].state == STATE_ACTIVE || slots[i].state == STATE_PENDING))
      return false;
  }
  return true;
}

// FIX: fsmGetActiveSlot also uses priority - returns ACTIVE before PENDING
BookingSlot* fsmGetActiveSlot() {
  BookingSlot* best = nullptr;
  int bestUrgency = -1;
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    if (!slots[i].active) continue;
    int u = 0;
    if (slots[i].state == STATE_ACTIVE)  u = 2;
    if (slots[i].state == STATE_PENDING) u = 1;
    if (u > bestUrgency) { bestUrgency = u; best = &slots[i]; }
  }
  return best;
}

uint16_t fsmCountdownMins() {
  BookingSlot* s = fsmGetActiveSlot();
  if (!s) return 0;
  time_t now = time(nullptr) + 7200; // 2 hour offset for Kigali
  if (now >= s->endTime) return 0;
  return (uint16_t)((s->endTime - now) / 60);
}

BookingSlot* fsmGetSlots() { return slots; }

static void transitionTo(BookingSlot* slot, FSMState newState) {
  slot->state = newState;
}
static BookingSlot* findSlot(const char* bookingId) {
  for (uint8_t i = 0; i < MAX_SLOTS; i++)
    if (slots[i].active && strcmp(slots[i].bookingId, bookingId) == 0)
      return &slots[i];
  return nullptr;
}
static BookingSlot* findFreeSlot() {
  for (uint8_t i = 0; i < MAX_SLOTS; i++)
    if (!slots[i].active) return &slots[i];
  return nullptr;
}
