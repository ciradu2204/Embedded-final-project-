#ifndef MEGA_PROTOCOL_H
#define MEGA_PROTOCOL_H

#include <Arduino.h>
#include <UTFT.h>
#include "touch_gt9271.h"

void handleIncomingCommand(UTFT* lcd);
void sendTouchEvent(TouchPoint tp);

#endif
