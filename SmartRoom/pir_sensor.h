#ifndef PIR_SENSOR_H
#define PIR_SENSOR_H

#include <Arduino.h>
#include "config.h"

/*
 * HC-SR501 PIR presence driver
 *
 * Hardware setup reminder:
 *   - Jumper set to H (repeat trigger mode) — output stays HIGH as long
 *     as presence continues, does not drop after the delay timer expires.
 *   - Sensitivity pot (closer to dome): mid position (~3m detection range).
 *   - Time-delay pot (closer to pins): minimum (~3 second hold time).
 *     The firmware grace period (10 min) handles timing, not the hardware.
 *   - VCC: 5V from ESP32 Vin pin (ESP32 devkit Vin provides 5V from USB).
 *   - GND: common GND with ESP32.
 *   - OUT: GPIO 34 on ESP32 (input-only pin, perfect for sensors).
 *
 * Software:
 *   - pirInit()    — call once in setup()
 *   - pirPresent() — call every loop, returns true if presence is confirmed
 *
 * A software debounce window (PIR_DEBOUNCE_MS in config.h, default 3s)
 * filters brief dropouts that can occur when a person moves across the
 * sensor's detection zones. Presence is considered lost only after the
 * output has been LOW continuously for the full debounce period.
 */

void pirInit();
bool pirPresent();          // True if presence is currently confirmed
bool pirJustDetected();     // True only on the rising edge (first detection)
bool pirJustLost();         // True only on the falling edge (presence lost)

#endif
