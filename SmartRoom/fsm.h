#ifndef FSM_H
#define FSM_H

#include "booking.h"

// ── FSM public interface ──────────────────────────────────────────────────────
void fsmInit();
void fsmTick(bool presenceDetected);         // Call every second from main loop
void fsmAddBooking(const BookingSlot& slot); // Called when MQTT booking arrives
void fsmCancelBooking(const char* bookingId);
bool fsmRoomIsAvailable();                   // True when no active/pending slot
bool fsmIsRoomFreeNow();                     // FIX (#2): same as above, explicit name
FSMState fsmGetCurrentState();
BookingSlot* fsmGetActiveSlot();             // Returns pointer to active slot, or nullptr
BookingSlot* fsmGetSlots();

// Walk-up booking: creates a local slot immediately, queues event for cloud sync.
// FIX (#2): Returns false if rejected because the room is not currently free.
bool fsmCreateWalkUpBooking(const char* occupantName, uint16_t durationMins);

// Get countdown in minutes to session end (0 if no active session)
uint16_t fsmCountdownMins();

#endif
