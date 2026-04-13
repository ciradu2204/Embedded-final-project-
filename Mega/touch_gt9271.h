#ifndef TOUCH_GT9271_H
#define TOUCH_GT9271_H

#include <Arduino.h>

// ── GT9271 I2C address ──────────────────────────────────────────────────
#define GT9271_ADDR    0x5D

// ── Shield pin assignments ──────────────────────────────────────────────
#define GT9271_INT     48   
#define GT9271_RST     41   // Renamed WAKE to RST to match manufacturer
// SDA = 20, SCL = 21

struct TouchPoint {
  bool touched;
  uint16_t x;
  uint16_t y;
};

#define GESTURE_TAP           'T'
#define GESTURE_SWIPE_LEFT    'L'
#define GESTURE_SWIPE_RIGHT   'R'

void        touchInit();
TouchPoint  touchRead();
char        detectGesture(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);

#endif
