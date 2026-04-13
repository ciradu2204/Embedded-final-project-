#include "mega_protocol.h"
#include "display_render.h"
#include "touch_gt9271.h"
#include <Arduino.h>

static uint8_t     currentScreen = 0;
static RoomDisplayData currentData;

// Calendar slot storage — populated by CALSLOT commands
static CalendarSlot calSlots[MAX_CAL_SLOTS];
static uint8_t      calSlotCount = 0;

// ── Non-blocking character-by-character UART reader ──────────────────────────
// FIX: Replaces readStringUntil() which blocks for 1 second on incomplete packets.
static char    _rxBuf[300];
static uint8_t _rxPos = 0;

static bool readLine(char* outBuf, uint16_t maxLen) {
  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n') {
      _rxBuf[_rxPos] = '\0';
      if (_rxPos > 0) {
        strncpy(outBuf, _rxBuf, maxLen - 1);
        outBuf[maxLen - 1] = '\0';
        _rxPos = 0;
        return true;
      }
      _rxPos = 0;
    } else if (c != '\r') {
      if (_rxPos < sizeof(_rxBuf) - 1) _rxBuf[_rxPos++] = c;
    }
  }
  return false;
}

// ── JSON helpers ──────────────────────────────────────────────────────────────
static bool extractStr(const char* json, const char* key,
                       char* outBuf, uint8_t bufLen) {
  char search[32];
  snprintf(search, sizeof(search), "\"%s\":\"", key);
  const char* p = strstr(json, search);
  if (!p) return false;
  p += strlen(search);
  uint8_t i = 0;
  while (*p && *p != '"' && i < bufLen - 1) outBuf[i++] = *p++;
  outBuf[i] = '\0';
  return true;
}

static int extractInt(const char* json, const char* key) {
  char search[32];
  snprintf(search, sizeof(search), "\"%s\":", key);
  const char* p = strstr(json, search);
  if (!p) return -1;
  return atoi(p + strlen(search));
}

static long extractLong(const char* json, const char* key) {
  char search[32];
  snprintf(search, sizeof(search), "\"%s\":", key);
  const char* p = strstr(json, search);
  if (!p) return -1;
  return atol(p + strlen(search));
}

static void reportScreen(uint8_t screen) {
  char buf[40];
  snprintf(buf, sizeof(buf), "{\"evt\":\"SCREEN\",\"s\":%u}\n", screen);
  Serial2.print(buf);
}

// ── Command handler ───────────────────────────────────────────────────────────
void handleIncomingCommand(UTFT* lcd) {
  char buf[300];
  // FIX: Non-blocking — returns false immediately if no complete line available
  if (!readLine(buf, sizeof(buf))) return;

  char cmd[16] = {0};
  if (!extractStr(buf, "cmd", cmd, sizeof(cmd))) return;

  if (strcmp(cmd, "STATUS") == 0) {
    if (currentScreen != 0) return;   // Ignore on sub-screens
    extractStr(buf, "room",  currentData.roomName,     sizeof(currentData.roomName));
    extractStr(buf, "occ",   currentData.occupantName, sizeof(currentData.occupantName));
    extractStr(buf, "start", currentData.startTime,    sizeof(currentData.startTime));
    extractStr(buf, "end",   currentData.endTime,      sizeof(currentData.endTime));
    currentData.state         = (uint8_t)extractInt(buf, "state");
    currentData.countdownMins = (uint16_t)extractInt(buf, "mins");
    currentData.countdownSecs = (uint32_t)extractLong(buf, "secs");
    displayStatusScreen(lcd, &currentData);

  } else if (strcmp(cmd, "CALENDAR") == 0) {
    // Reset calendar slot buffer
    memset(calSlots, 0, sizeof(calSlots));
    currentScreen = 1;
    reportScreen(1);
    calSlotCount = 0; // <--- ADDED: Clears old slots before drawing new ones
    displayCalendarScreen(lcd);

  } else if (strcmp(cmd, "CALSLOT") == 0) {
    // Receive one booking slot for the calendar
    if (calSlotCount < MAX_CAL_SLOTS) {
      CalendarSlot& cs = calSlots[calSlotCount];
      cs.startSecs = (uint32_t)extractLong(buf, "s");
      cs.endSecs   = (uint32_t)extractLong(buf, "e");
      extractStr(buf, "n", cs.name, sizeof(cs.name));
      cs.state  = (uint8_t)extractInt(buf, "st");
      cs.active = (cs.startSecs > 0);
      if (cs.active) calSlotCount++;
    }

  } else if (strcmp(cmd, "CALDONE") == 0) {
    // All CALSLOT packets received — draw the bookings on the calendar
    if (currentScreen == 1) {
      displayCalendarBookings(lcd, calSlots, calSlotCount);
    }

  } else if (strcmp(cmd, "BOOKNOW") == 0) {
    currentScreen = 2;
    reportScreen(2);
    displayBookNowScreen(lcd);

  } else if (strcmp(cmd, "CONFIRM") == 0) {
    displayConfirmation(lcd, extractInt(buf, "ok") == 1);
    currentScreen = 0;
    reportScreen(0);
    // FIX: Reset the status screen cache so next STATUS triggers full redraw.
    resetStatusScreenCache();

  } else if (strcmp(cmd, "OFFLINE") == 0) {
    if (currentScreen == 0)
      displayOfflineWarning(lcd, extractInt(buf, "show") == 1);

  } else if (strcmp(cmd, "STARTUP") == 0) {
    currentScreen = 0;
    reportScreen(0);
    resetStatusScreenCache();
    displayStartup(lcd);
  }
}

// ── Touch event sender ────────────────────────────────────────────────────────
void sendTouchEvent(TouchPoint tp) {
  static bool wasDown = false;
  static uint16_t downX = 0, downY = 0;
  static uint16_t lastX = 0, lastY = 0; // <--- Tracks actual finger position

  if (tp.touched) {
    lastX = tp.x;
    lastY = tp.y;
    if (!wasDown) {
      wasDown = true;
      downX = tp.x;
      downY = tp.y;
    }
  } else if (!tp.touched && wasDown) {
    wasDown = false;
    
    // FIX: Use lastX and lastY to calculate the gesture, not tp.x (which is 0)
    char gesture = detectGesture(downX, downY, lastX, lastY);

    if (gesture == GESTURE_SWIPE_RIGHT) {
      if (currentScreen != 0) {
        currentScreen = 0;
        reportScreen(0);
        resetStatusScreenCache();
      }
      Serial2.print("{\"evt\":\"TOUCH\",\"gesture\":\"CANCEL\"}\n");
      return;
    }

    if (currentScreen == 0) {
      if (gesture == GESTURE_SWIPE_LEFT) {
        Serial2.print("{\"evt\":\"TOUCH\",\"gesture\":\"L\"}\n");
      } else if (gesture == GESTURE_TAP) {
        // FIX: Use downX and downY to check where the tap occurred
        if (downX >= 20 && downX <= 280 && downY >= 230 && downY <= 300) {
          Serial.println(F("[Touch] BOOK NOW"));
          Serial2.print("{\"evt\":\"TOUCH\",\"gesture\":\"BOOKNOW\"}\n");
        }
      }
    } 
    else if (currentScreen == 2) {
      if (gesture == GESTURE_TAP) {
        int dur = 0;
        if (downY >= 130 && downY <= 230) {
          if      (downX >= 40  && downX <= 240) dur = 15;
          else if (downX >= 280 && downX <= 480) dur = 30;
          else if (downX >= 520 && downX <= 720) dur = 60;
        }

        if (dur > 0) {
          char out[64];
          snprintf(out, sizeof(out), "{\"evt\":\"BOOK\",\"dur\":%d}\n", dur);
          Serial2.print(out);
        } else if (downY > 400) {
          // Added back the local state reset so the UI jumps back snappily
          currentScreen = 0;
          reportScreen(0);
          resetStatusScreenCache();
          Serial2.print("{\"evt\":\"TOUCH\",\"gesture\":\"CANCEL\"}\n");
        }
      }
    }
  }
}
