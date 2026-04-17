#include "fsm.h"
#include "config.h"
#include "event_queue.h"
#include <Arduino.h>
#include <time.h>

static BookingSlot slots[MAX_SLOTS];
static uint8_t slotCount = 0;

static void transitionTo(BookingSlot* slot, FSMState newState);
static BookingSlot* findSlot(const char* bookingId);
static BookingSlot* findFreeSlot();

void fsmInit() {
  memset(slots, 0, sizeof(slots));
  slotCount = 0;
  Serial.println(F("[FSM] Initialised."));
}

void fsmAddBooking(const BookingSlot& incoming) {
  // Reject bookings whose end time has already passed — the backend
  // sometimes re-publishes completed sessions via realtime UPDATE after
  // handleRoomStatus fails to update their status in time, and without
  // this guard the ESP32 would resurrect them as STATE_SCHEDULED and
  // keep the LCD stuck on PENDING for the full grace period.
  time_t nowKigali = time(nullptr) + 7200;
  if (nowKigali > 1000000000L && incoming.endTime <= nowKigali) {
    Serial.printf("[FSM] Ignoring past booking %s (end=%lu now=%lu)\n",
                  incoming.bookingId,
                  (unsigned long)incoming.endTime,
                  (unsigned long)nowKigali);
    // Also make sure any stale copy of this booking is not hanging around.
    for (uint8_t i = 0; i < MAX_SLOTS; i++) {
      if (strcmp(slots[i].bookingId, incoming.bookingId) == 0) {
        slots[i].active = false;
        slots[i].state  = STATE_COMPLETED;
      }
    }
    return;
  }

  BookingSlot* existing = findSlot(incoming.bookingId);
  if (existing) {
    strlcpy(existing->occupantName, incoming.occupantName, sizeof(existing->occupantName));
    strlcpy(existing->title,        incoming.title,        sizeof(existing->title));
    existing->startTime      = incoming.startTime;
    existing->endTime        = incoming.endTime;
    existing->active         = true;
    existing->seenInSnapshot = true;
    Serial.printf("[FSM] Updated booking %s\n", incoming.bookingId);
    return;
  }
  BookingSlot* slot = findFreeSlot();
  if (!slot) { Serial.println(F("[FSM] No free slot.")); return; }
  *slot = incoming;
  slot->state          = STATE_SCHEDULED;
  slot->active         = true;
  slot->pendingStartMs = 0;
  slot->buzzerFired    = false;
  slot->seenInSnapshot = true;
  slotCount++;
  Serial.printf("[FSM] Added %s for %s\n", incoming.bookingId, incoming.occupantName);
}

// Snapshot reconciliation — see fsm.h for the contract. Marks every active
// non-walk-up slot as "unseen" so that the subsequent fsmAddBooking calls
// (driven by the snapshot payload) can flag the ones still present.
void fsmBeginSnapshot() {
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    slots[i].seenInSnapshot = false;
  }
}

// Drop any slot the backend no longer reports. Walk-ups (id prefix "wu_")
// are skipped because the backend may not have echoed them back yet; their
// eventual replacement via the server-assigned UUID is handled by the
// normal fsmAddBooking path when the next snapshot arrives.
void fsmPruneUnseen() {
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    if (!slots[i].active) continue;
    if (strncmp(slots[i].bookingId, "wu_", 3) == 0) continue;
    if (slots[i].seenInSnapshot) continue;
    Serial.printf("[FSM] Pruning stale booking %s (deleted upstream)\n",
                  slots[i].bookingId);
    slots[i].active = false;
    slots[i].state  = STATE_COMPLETED;
  }
}

void fsmCancelBooking(const char* bookingId) {
  BookingSlot* slot = findSlot(bookingId);
  if (slot) {
    slot->active = false;
    slot->state  = STATE_COMPLETED;
    Serial.printf("[FSM] Cancelled %s\n", bookingId);
  }
}

// FIX (#1): Per-slot grace timer — pendingStartMs lives on each BookingSlot,
// so concurrent bookings each track their own grace window correctly.
//
// FIX (#4): Buzzer warning is now driven by absolute wall-clock time
// (slot->endTime - now <= warning seconds), so it survives a reboot.
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
          s->pendingStartMs = millis();   // FIX: per-slot
          s->buzzerFired    = false;      // FIX: per-slot
          Serial.printf("[FSM] %s -> PENDING\n", s->bookingId);
        }
        break;

      case STATE_PENDING:
        if (presenceDetected) {
          transitionTo(s, STATE_ACTIVE);
          Serial.printf("[FSM] %s -> ACTIVE\n", s->bookingId);
          FsmEvent evt;
          evt.type = EVT_OCCUPANCY_CONFIRMED;
          strlcpy(evt.bookingId, s->bookingId, sizeof(evt.bookingId));
          strlcpy(evt.roomId, ROOM_ID, sizeof(evt.roomId));
          evt.timestamp = now;
          eventQueuePush(evt);
        } else if (millis() - s->pendingStartMs >= GRACE_PERIOD_MS) {
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

      case STATE_ACTIVE: {
        // FIX (#4): absolute-time buzzer trigger. Survives reboot because
        // it depends on wall clock, not millis() since boot.
        long remaining = (long)s->endTime - (long)now;
        long warningSecs = (long)(BUZZER_WARNING_MS / 1000UL);
        if (!s->buzzerFired && remaining > 0 && remaining <= warningSecs) {
          s->buzzerFired = true;
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
      }

      case STATE_GHOST:
      case STATE_COMPLETED:
        break;
    }
  }
}

// FIX (#2): central "is the room free *right now*" predicate.
// Returns false if any slot is currently ACTIVE or PENDING (a session is in
// progress or about to begin and waiting on occupancy confirmation).
bool fsmIsRoomFreeNow() {
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    if (!slots[i].active) continue;
    if (slots[i].state == STATE_ACTIVE || slots[i].state == STATE_PENDING) {
      return false;
    }
  }
  return true;
}

// FIX (#2): walk-up booking now refuses to clobber an existing ACTIVE/PENDING
// session AND refuses if its [now, end] window would overlap any future
// SCHEDULED booking. Returns false if rejected so the caller can show feedback.
bool fsmCreateWalkUpBooking(const char* occupantName, uint16_t durationMins, const char* title) {
  // Refuse if NTP hasn't synced yet — otherwise time(nullptr) is near-zero
  // and the booking lands at 1970-01-01, making it invisible on the
  // dashboard (end_time < now filters it out immediately).
  time_t rawNow = time(nullptr);
  if (rawNow < 1000000000L) {
    Serial.println(F("[FSM] Walk-up rejected: clock not synced yet."));
    return false;
  }
  if (!fsmIsRoomFreeNow()) {
    Serial.println(F("[FSM] Walk-up rejected: room not free."));
    return false;
  }
  time_t now      = rawNow + 7200; // 2 hour offset for Kigali
  time_t proposedStart = now;
  time_t proposedEnd   = now + (time_t)(durationMins * 60);

  // Refuse if the proposed window overlaps any slot we already know about.
  // Two intervals [a, b) and [c, d) overlap iff a < d && c < b.
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    if (!slots[i].active) continue;
    // Only consider upcoming / live bookings
    if (slots[i].state == STATE_GHOST || slots[i].state == STATE_COMPLETED) continue;
    if (proposedStart < slots[i].endTime && slots[i].startTime < proposedEnd) {
      Serial.printf("[FSM] Walk-up rejected: overlaps %s (%lu..%lu)\n",
                    slots[i].bookingId,
                    (unsigned long)slots[i].startTime,
                    (unsigned long)slots[i].endTime);
      return false;
    }
  }

  BookingSlot* slot = findFreeSlot();
  if (!slot) { Serial.println(F("[FSM] No free slot for walk-up.")); return false; }
  memset(slot, 0, sizeof(*slot));
  snprintf(slot->bookingId, sizeof(slot->bookingId), "wu_%lu", (unsigned long)now);
  strlcpy(slot->occupantName, occupantName, sizeof(slot->occupantName));
  strlcpy(slot->title, title ? title : "", sizeof(slot->title));
  slot->startTime      = now;
  slot->endTime        = now + (durationMins * 60);
  slot->state          = STATE_ACTIVE;
  slot->active         = true;
  slot->pendingStartMs = 0;
  slot->buzzerFired    = false;
  Serial.printf("[FSM] Walk-up: %s for %u min (%s)\n", slot->bookingId, durationMins, slot->title);
  FsmEvent evt;
  memset(&evt, 0, sizeof(evt));
  evt.type = EVT_WALK_UP_BOOKING;
  strlcpy(evt.bookingId,    slot->bookingId,    sizeof(evt.bookingId));
  strlcpy(evt.roomId,       ROOM_ID,            sizeof(evt.roomId));
  strlcpy(evt.title,        slot->title,        sizeof(evt.title));
  strlcpy(evt.occupantName, slot->occupantName, sizeof(evt.occupantName));
  evt.timestamp = now;
  evt.startTime = slot->startTime;
  evt.endTime   = slot->endTime;
  eventQueuePush(evt);
  return true;
}

// Priority ordering for the status screen: ACTIVE > PENDING > others.
// SCHEDULED slots never drive the status label — the room stays AVAILABLE
// until a booking actually starts (fsmTick transitions SCHEDULED -> PENDING
// at startTime). Upcoming reservations are visible on the calendar view.
FSMState fsmGetCurrentState() {
  FSMState best = STATE_COMPLETED;  // lowest priority (= available)
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    if (!slots[i].active) continue;
    FSMState s = slots[i].state;
    if (s == STATE_SCHEDULED) continue;  // Upcoming reservations don't count
    auto urgency = [](FSMState st) -> int {
      switch (st) {
        case STATE_ACTIVE:    return 4;
        case STATE_PENDING:   return 3;
        case STATE_GHOST:     return 1;
        default:              return 0;
      }
    };
    if (urgency(s) > urgency(best)) best = s;
  }
  return best;
}

bool fsmRoomIsAvailable() {
  return fsmIsRoomFreeNow();
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
