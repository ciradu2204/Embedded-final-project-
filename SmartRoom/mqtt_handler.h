#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include <Arduino.h>
#include "booking.h"

void mqttInit();
void mqttLoop();               // Call every loop iteration
bool mqttConnected();
void mqttPublishStatus(const FsmEvent& evt);
void mqttFlushQueue();         // Flush stored events after reconnect

// Callback type: called when a booking message arrives from cloud
typedef void (*BookingCallback)(const char* payload);
void mqttSetBookingCallback(BookingCallback cb);

// NOTE: mqttPublishWalkUp was removed — walk-up bookings are published
// via the standard mqttPublishStatus() path using EVT_WALK_UP_BOOKING.

#endif
