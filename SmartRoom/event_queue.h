#ifndef EVENT_QUEUE_H
#define EVENT_QUEUE_H

#include "booking.h"
#include "config.h"

// ── Store-and-forward circular buffer ────────────────────────────────────────
// Events generated during network outages are held here and flushed on reconnect.

void  eventQueueInit();
bool  eventQueuePush(const FsmEvent& evt);   // Returns false if buffer is full
bool  eventQueuePop(FsmEvent& out);          // Returns false if buffer is empty
bool  eventQueueEmpty();
uint8_t eventQueueSize();

#endif
