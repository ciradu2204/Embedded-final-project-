#include "pir_sensor.h"

// ── Internal state ────────────────────────────────────────────────────────────
static bool          _confirmed    = false;
static bool          _justDetected = false;
static bool          _justLost     = false;
static bool          _lastRaw      = false;  // previous raw reading
static unsigned long _lowSince     = 0;      // millis() when pin first went LOW

void pirInit() {
  pinMode(PIN_PIR, INPUT);
  _confirmed    = false;
  _justDetected = false;
  _justLost     = false;
  _lastRaw      = false;
  _lowSince     = 0;
  Serial.println(F("[PIR] Initialised on GPIO 34."));
  Serial.println(F("[PIR] HC-SR501 needs ~30s warm-up after power-on."));
}

bool pirPresent() {
  _justDetected = false;
  _justLost     = false;

  bool raw = (bool)digitalRead(PIN_PIR);

  if (raw) {
    // ── Pin is HIGH: presence signal active ──────────────────────────────────
    // BUG FIX: only reset _lowSince on the LOW→HIGH edge, not every HIGH tick.
    // Previously _lowSince was set to millis() on every HIGH reading, which
    // meant the debounce timer could never expire after a brief dropout.
    if (!_lastRaw) {
      // Transition LOW→HIGH: cancel any pending dropout timer
      _lowSince = 0;
    }

    if (!_confirmed) {
      _confirmed    = true;
      _justDetected = true;
      Serial.println(F("[PIR] Presence detected."));
    }

  } else {
    // ── Pin is LOW: no motion signal ─────────────────────────────────────────
    if (_confirmed) {
      if (_lastRaw) {
        // Transition HIGH→LOW: start the dropout timer now (first LOW tick)
        _lowSince = millis();
      }
      // _lowSince is guaranteed non-zero here because we set it on first LOW tick
      if (_lowSince > 0 && (millis() - _lowSince >= PIR_DEBOUNCE_MS)) {
        _confirmed = false;
        _justLost  = true;
        _lowSince  = 0;
        Serial.println(F("[PIR] Presence lost (debounce expired)."));
      }
      // If debounce has not expired, still report _confirmed = true
    }
  }

  _lastRaw = raw;
  return _confirmed;
}

bool pirJustDetected() { return _justDetected; }
bool pirJustLost()     { return _justLost; }
