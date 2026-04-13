#ifndef LED_H
#define LED_H

#include <Arduino.h>

/*
 * Separate Red and Green LED driver
 *
 * Wiring:
 *   Green LED: Anode → 220Ω resistor → GPIO 25 → LED → Cathode → GND
 *   Red LED:   Anode → 220Ω resistor → GPIO 26 → LED → Cathode → GND
 *
 * Both LEDs use PWM via ESP32 LEDC so we can do smooth fade transitions
 * instead of abrupt on/off switching.
 *
 * LED states:
 *   LED_OFF       — both off (completed / initialising)
 *   LED_GREEN     — room available
 *   LED_RED       — room occupied or pending
 *   LED_BLINK_RED — ghost detection grace period countdown warning
 *                   (slow blink to signal "no-show detected")
 */

typedef enum {
  LED_OFF = 0,
  LED_GREEN,
  LED_RED,
  LED_BLINK_RED    // 1Hz blink during PENDING state
} LEDState;

void ledInit();
void ledSet(LEDState state);
void ledTick();        // Call every loop to handle blinking

#endif
