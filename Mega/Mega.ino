#include <Arduino.h>
#include <UTFT.h>
#include <Wire.h>
#include "display_render.h"
#include "touch_gt9271.h"
#include "mega_protocol.h"

// ── Backlight ────────────────────────────────────────────────────────────────
#define BL_PIN 8

// ── UTFT display object ───────────────────────────────────────────────────────
UTFT myGLCD(SSD1963_800, 38, 39, 40, 41);

extern uint8_t BigFont[];
extern uint8_t SmallFont[];

void setup() {
  Serial.begin(115200);       // Debug output to PC
  Serial2.begin(115200);      // Using Serial2 (Pins 16/17) to ESP32

  pinMode(BL_PIN, OUTPUT);
  digitalWrite(BL_PIN, HIGH);

  // 1. TOUCH INIT MUST GO FIRST! (Shares Pin 41 Reset with LCD)
  touchInit();

  // 2. LCD INIT GOES SECOND!
  myGLCD.InitLCD(LANDSCAPE);
  myGLCD.clrScr();

  displayStartup(&myGLCD);

  Serial.println(F("Mega ready. Listening on Serial2 (Pins 16/17)."));
}

void loop() {
  // 1. Check for commands arriving from ESP32 on Serial2
  if (Serial2.available()) {
    handleIncomingCommand(&myGLCD);
  }

  // 2. Poll touch panel and forward any touch events to ESP32
  TouchPoint tp = touchRead();
  
  // VITAL FIX: No "if (tp.touched)" here! 
  // It must run every loop so the protocol knows when you LIFT your finger.
  sendTouchEvent(tp);
}
