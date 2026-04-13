#include "led.h"
#include "config.h"

/*
 * RGB module wiring (4-pin common cathode module):
 *   R pin  -> 220Ω -> GPIO 25
 *   G pin  -> 220Ω -> GPIO 26
 *   B pin  -> leave disconnected (not used)
 *   GND/- pin -> GND
 *
 * ESP32 Core 3.x API change:
 *   OLD (2.x): ledcSetup(channel, freq, res) + ledcAttachPin(pin, channel)
 *   NEW (3.x): ledcAttach(pin, freq, res)  -- single call, no channel numbers
 *   ledcWrite(pin, duty) also takes pin directly, not channel number.
 */

static LEDState      _current     = LED_OFF;
static bool          _blinkState  = false;
static unsigned long _lastBlinkMs = 0;

#define BLINK_INTERVAL_MS 500

void ledInit() {
  // Core 3.x: one call per pin, no separate channel management
  ledcAttach(PIN_GREEN_LED, PWM_FREQ_LED, PWM_RESOLUTION);
  ledcAttach(PIN_RED_LED,   PWM_FREQ_LED, PWM_RESOLUTION);

  ledcWrite(PIN_GREEN_LED, 0);
  ledcWrite(PIN_RED_LED,   0);

  Serial.println(F("[LED] Green GPIO 25, Red GPIO 26. Core 3.x PWM ready."));
}

void ledSet(LEDState state) {
  if (state == _current) return;
  _current     = state;
  _blinkState  = false;
  _lastBlinkMs = millis();

  switch (state) {
    case LED_OFF:
      ledcWrite(PIN_GREEN_LED, 0);
      ledcWrite(PIN_RED_LED,   0);
      break;
    case LED_GREEN:
      ledcWrite(PIN_GREEN_LED, 255);
      ledcWrite(PIN_RED_LED,   0);
      break;
    case LED_RED:
      ledcWrite(PIN_GREEN_LED, 0);
      ledcWrite(PIN_RED_LED,   255);
      break;
    case LED_BLINK_RED:
      ledcWrite(PIN_GREEN_LED, 0);
      ledcWrite(PIN_RED_LED,   255);
      _blinkState = true;
      break;
  }
}

void ledTick() {
  if (_current != LED_BLINK_RED) return;
  unsigned long now = millis();
  if (now - _lastBlinkMs >= BLINK_INTERVAL_MS) {
    _lastBlinkMs = now;
    _blinkState  = !_blinkState;
    ledcWrite(PIN_RED_LED, _blinkState ? 255 : 20);
  }
}
