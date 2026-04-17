#ifndef FSM_H
#define FSM_H

#include "booking.h"

// ── FSM public interface ──────────────────────────────────────────────────────
void fsmInit();
void fsmTick(bool presenceDetected);         // Call every second from main loop
void fsmAddBooking(const BookingSlot& slot); // Called when MQTT booking arrives
void fsmCancelBooking(const char* bookingId);

// Authoritative snapshot reconciliation. Call at the start of processing a
// `/bookings/snapshot` payload to mark all non-walk-up slots as "unseen",
// then call fsmAddBooking for each booking in the snapshot (which marks it
// seen). Finally call fsmPruneUnseen() to deactivate any slot that the
// backend no longer knows about (deleted, cancelled, cascaded). Walk-ups
// (id prefix "wu_") are immune to pruning because the backend may not have
// confirmed them yet.
void fsmBeginSnapshot();
void fsmPruneUnseen();
bool fsmRoomIsAvailable();                   // True when no active/pending slot
bool fsmIsRoomFreeNow();                     // FIX (#2): same as above, explicit name
FSMState fsmGetCurrentState();
BookingSlot* fsmGetActiveSlot();             // Returns pointer to active slot, or nullptr
BookingSlot* fsmGetSlots();

// Returns the next SCHEDULED booking strictly in the future (earliest
// startTime). Used by the status screen to surface "Up next:" context
// when the room is currently free. Returns nullptr if none.
BookingSlot* fsmGetUpcomingSlot();

// Walk-up booking: creates a local slot immediately, queues event for cloud sync.
// FIX (#2): Returns false if rejected because the room is not currently free.
// `title` is the booking purpose (e.g. "Meeting", "Class") and may be NULL.
bool fsmCreateWalkUpBooking(const char* occupantName, uint16_t durationMins, const char* title);

// Get countdown in minutes to session end (0 if no active session)
uint16_t fsmCountdownMins();

#endif
