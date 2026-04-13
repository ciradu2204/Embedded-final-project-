#ifndef BUZZER_H
#define BUZZER_H

#include <Arduino.h>

/*
 * Buzzer driver — supports both active and passive buzzers.
 * ACTIVE_BUZZER in config.h: 1=active, 0=passive (current hardware).
 * Wiring: + terminal -> GPIO 27, - terminal -> GND.
 */

void buzzerInit();
void buzzerDoubleBeep();      // Two short beeps — 5-min session warning
void buzzerShortBeep();       // Single short confirmation beep
// Single quiet blip — called periodically when offline to signal no connectivity.
// Much less annoying than a full beep pattern repeated rapidly.
void buzzerOfflineBlip();
void buzzerTick();            // Call every loop iteration

#endif
