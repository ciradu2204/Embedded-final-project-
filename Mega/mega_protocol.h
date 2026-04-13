#ifndef MEGA_PROTOCOL_H
#define MEGA_PROTOCOL_H

#include <Arduino.h>
#include <UTFT.h>
#include "touch_gt9271.h"

void handleIncomingCommand(UTFT* lcd);
void sendTouchEvent(TouchPoint tp);

// Allows local touch handlers (e.g. calendar scrolling) to redraw without
// a round trip through the ESP32. Call once from setup() with the global UTFT.
void megaProtocolSetLcd(UTFT* lcd);

#endif
