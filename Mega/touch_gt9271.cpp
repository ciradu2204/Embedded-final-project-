#include "touch_gt9271.h"
#include <Wire.h>

#define GT9271_REG_STATUS   0x814E   
#define GT9271_REG_POINT1   0x8150   
#define GT9271_REG_CONFIG   0x8047   

// ── Helpers ───────────────────────────────────────────────────────────
static void gt9271Write(uint16_t reg, uint8_t val) {
  Wire.beginTransmission(GT9271_ADDR);
  Wire.write(reg >> 8);      
  Wire.write(reg & 0xFF);    
  Wire.write(val);
  Wire.endTransmission();
}

static bool gt9271Read(uint16_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(GT9271_ADDR);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  if (Wire.endTransmission(false) != 0) return false;

  Wire.requestFrom((uint8_t)GT9271_ADDR, len);
  for (uint8_t i = 0; i < len && Wire.available(); i++) {
    buf[i] = Wire.read();
  }
  return true;
}

// ── Main Functions ────────────────────────────────────────────────────
void touchInit() {
  pinMode(GT9271_RST, OUTPUT);
  pinMode(GT9271_INT, OUTPUT);

  digitalWrite(GT9271_RST, LOW);
  digitalWrite(GT9271_INT, LOW);
  delay(20); 

  digitalWrite(GT9271_RST, HIGH);
  delay(50); 
  
  pinMode(GT9271_INT, INPUT);      
  Wire.begin();
  delay(50); 

  uint8_t cfg = 0;
  if (gt9271Read(GT9271_REG_CONFIG, &cfg, 1)) {
    Serial.print(F("[GT9271] Found. Config: "));
    Serial.println(cfg, HEX);
  } else {
    Serial.println(F("[GT9271] Not responding on I2C."));
  }
}

TouchPoint touchRead() {
  static TouchPoint lastTp = {false, 0, 0};
  TouchPoint tp = {false, 0, 0};
  uint8_t status = 0;

  // 1. If I2C read fails, DO NOT assume a lift. Return the last known good state.
  if (!gt9271Read(GT9271_REG_STATUS, &status, 1)) {
    return lastTp;
  }

  // 2. If buffer is NOT ready, DO NOT assume the finger was lifted!
  // Return the last known state so dragging/swiping doesn't drop out.
  if (!(status & 0x80)) {
    return lastTp;
  }

  uint8_t numPoints = status & 0x0F;
  if (numPoints > 0) {
    uint8_t data[4];
    if (gt9271Read(GT9271_REG_POINT1, data, 4)) {
      tp.x = data[0] | ((uint16_t)data[1] << 8);
      tp.y = data[2] | ((uint16_t)data[3] << 8);
      tp.touched = true;

      // GT9271 sometimes sends (0,0) as a garbage coordinate. Ignore it.
      if (tp.x == 0 && tp.y == 0) {
         tp = lastTp;
      }
    }
  } else {
    // 3. numPoints == 0 means the finger ACTUALLY lifted.
    tp.touched = false;
    tp.x = lastTp.x; // Keep the last valid coordinates for the swipe calculation
    tp.y = lastTp.y;
  }

  // Clear the ready flag so the chip can capture the next point
  gt9271Write(GT9271_REG_STATUS, 0);

  lastTp = tp;
  return tp;
}

char detectGesture(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
  int dx = (int)x2 - (int)x1;
  int dy = (int)y2 - (int)y1;

  // 1. Check if the horizontal movement is significant (reduced to 60px)
  // 2. Ensure it's more of a horizontal movement than a vertical one
  if (abs(dx) > 60 && abs(dx) > abs(dy)) {
    if (dx > 0) return GESTURE_SWIPE_RIGHT; // x2 > x1 means moving to the right
    else return GESTURE_SWIPE_LEFT;         // x2 < x1 means moving to the left
  }

  // Otherwise, treat as a Tap
  return GESTURE_TAP;
}
