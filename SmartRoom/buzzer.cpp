#include "buzzer.h"
#include "config.h"

#define MAX_PATTERN_STEPS 8

static uint16_t      _pattern[MAX_PATTERN_STEPS];
static uint8_t       _patternLen     = 0;
static uint8_t       _patternStep    = 0;
static bool          _patternRunning = false;
static unsigned long _stepStartMs    = 0;

static void buzzerOn() {
#if ACTIVE_BUZZER
  digitalWrite(PIN_BUZZER, HIGH);
#else
  ledcWriteTone(PIN_BUZZER, BUZZER_FREQ);
#endif
}

static void buzzerOff() {
#if ACTIVE_BUZZER
  digitalWrite(PIN_BUZZER, LOW);
#else
  ledcWriteTone(PIN_BUZZER, 0);
#endif
}

static void startPattern(const uint16_t* durations, uint8_t len) {
  // If a pattern is already running, let it finish rather than cutting it off.
  // This prevents overlapping patterns from leaving the buzzer stuck on.
  if (_patternRunning) return;
  memcpy(_pattern, durations, len * sizeof(uint16_t));
  _patternLen     = len;
  _patternStep    = 0;
  _patternRunning = true;
  _stepStartMs    = millis();
  buzzerOn();
}

void buzzerInit() {
#if ACTIVE_BUZZER
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  Serial.println(F("[Buzzer] Active buzzer on GPIO 27."));
#else
  ledcAttach(PIN_BUZZER, BUZZER_FREQ, PWM_RESOLUTION);
  ledcWriteTone(PIN_BUZZER, 0);
  Serial.printf("[Buzzer] Passive buzzer on GPIO 27 at %d Hz.\n", BUZZER_FREQ);
#endif
}

void buzzerDoubleBeep() {
  // 200ms on, 150ms off, 200ms on
  static const uint16_t pattern[] = {200, 150, 200};
  startPattern(pattern, 3);
}

void buzzerShortBeep() {
  // 80ms on — subtle confirmation tap
  static const uint16_t pattern[] = {80};
  startPattern(pattern, 1);
}

void buzzerOfflineBlip() {
  // Very short 40ms blip — barely noticeable, just enough to indicate a state.
  // Called every 2 seconds from SmartRoom.ino when offline, so this runs at most
  // once per 2 seconds and only if no other pattern is currently playing.
  static const uint16_t pattern[] = {40};
  startPattern(pattern, 1);
}

void buzzerTick() {
  if (!_patternRunning) return;
  if (millis() - _stepStartMs >= _pattern[_patternStep]) {
    _patternStep++;
    _stepStartMs = millis();
    if (_patternStep >= _patternLen) {
      buzzerOff();
      _patternRunning = false;
      return;
    }
    if (_patternStep % 2 == 0) buzzerOn();
    else                        buzzerOff();
  }
}
