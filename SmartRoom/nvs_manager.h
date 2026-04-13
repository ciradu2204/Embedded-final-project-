#ifndef NVS_MANAGER_H
#define NVS_MANAGER_H

#include "booking.h"

/*
 * NVS (Non-Volatile Storage) manager
 *
 * Persists the booking cache to ESP32 flash so the FSM survives power cuts.
 * On boot the device reads the cache and reconstructs FSM state before
 * WiFi is even established.
 *
 * Storage format: the BookingSlot array is serialised to a compact JSON
 * string and stored as a single NVS blob under key "bookings".
 *
 * Call nvsInit() once in setup().
 * Call nvsSaveBookings() whenever the slot array changes.
 * Call nvsLoadBookings() at boot to restore state.
 */

void nvsInit();
void nvsSaveBookings(BookingSlot* slots, uint8_t count);
uint8_t nvsLoadBookings(BookingSlot* slots, uint8_t maxCount);

#endif
