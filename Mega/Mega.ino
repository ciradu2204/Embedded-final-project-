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

  // Hand the LCD pointer to the protocol layer so local touch handlers
  // (calendar scrolling) can redraw without an ESP32 round-trip.
  megaProtocolSetLcd(&myGLCD);

  displayStartup(&myGLCD);

  Serial.println(F("Mega ready. Listening on Serial2 (Pins 16/17)."));
}

void loop() {
  // Drain every complete line currently in the Serial2 RX ring before
  // touching touch. Mega's default HardwareSerial ring is only 64 bytes —
  // a STATUS (~170B) or CALSLOT (~90B) burst overflows it if the loop
  // takes more than ~5.5 ms between drains. At 115200 baud the ring fills
  // in roughly 5 ms, so touch/I2C work must not go longer than that.
  while (Serial2.available()) {
    handleIncomingCommand(&myGLCD);
  }

  // Poll touch panel and forward any touch events to ESP32. Wrapped with
  // a pre-check on Serial2 so an inbound packet in flight gets drained
  // before we spend I2C time reading the touch controller.
  if (Serial2.available() > 0) {
    while (Serial2.available()) handleIncomingCommand(&myGLCD);
  }
  TouchPoint tp = touchRead();

  // Drain again after touch read — I2C read takes ~1-3 ms and more bytes
  // may have arrived while we were talking to the touch IC.
  while (Serial2.available()) handleIncomingCommand(&myGLCD);

  // Run every loop (not gated on tp.touched) so finger-lift is always
  // detected even if we missed the "down" poll due to a Serial drain.
  sendTouchEvent(tp);
}
