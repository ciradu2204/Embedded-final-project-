#include "mega_protocol.h"
#include "display_render.h"
#include "touch_gt9271.h"
#include <Arduino.h>

// Admin PIN for walk-up bookings. Must match SmartRoom/config.h::ADMIN_PIN.
// Keep the two values in sync manually — the Mega has no way to fetch it
// from the ESP32 at runtime.
#define MEGA_ADMIN_PIN "1234"

static uint8_t         currentScreen = 0;
static RoomDisplayData currentData;

// Calendar slot storage — populated by CALSLOT commands
static CalendarSlot calSlots[MAX_CAL_SLOTS];
static uint8_t      calSlotCount = 0;

// FIX: Visible top hour in the calendar viewport. Persists across re-renders
// so that swipe/arrow scrolling sticks while CALSLOT data trickles in.
static uint8_t      calTopHour    = CAL_DAY_START_HOUR;

// Helper: redraw calendar with current slot buffer at current scroll offset
static void redrawCalendar(UTFT* lcd) {
  displayCalendarScreen(lcd, calTopHour);
  displayCalendarBookings(lcd, calSlots, calSlotCount, calTopHour);
}

static void scrollCalendar(UTFT* lcd, int delta) {
  int next = (int)calTopHour + delta;
  int maxTop = CAL_DAY_END_HOUR - CAL_VISIBLE_ROWS + 1;
  if (next < CAL_DAY_START_HOUR) next = CAL_DAY_START_HOUR;
  if (next > maxTop)             next = maxTop;
  if ((uint8_t)next == calTopHour) return;
  calTopHour = (uint8_t)next;
  redrawCalendar(lcd);
}

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
    currentData.title[0] = '\0';
    extractStr(buf, "title", currentData.title,        sizeof(currentData.title));
    extractStr(buf, "start", currentData.startTime,    sizeof(currentData.startTime));
    extractStr(buf, "end",   currentData.endTime,      sizeof(currentData.endTime));
    currentData.state         = (uint8_t)extractInt(buf, "state");
    currentData.countdownMins = (uint16_t)extractInt(buf, "mins");
    currentData.countdownSecs = (uint32_t)extractLong(buf, "secs");
    displayStatusScreen(lcd, &currentData);

  } else if (strcmp(cmd, "CALENDAR") == 0) {
    // Reset calendar slot buffer + reset scroll to top of day
    memset(calSlots, 0, sizeof(calSlots));
    currentScreen = 1;
    reportScreen(1);
    calSlotCount = 0;
    calTopHour   = CAL_DAY_START_HOUR;
    displayCalendarScreen(lcd, calTopHour);

  } else if (strcmp(cmd, "CALSLOT") == 0) {
    // Receive one booking slot for the calendar
    if (calSlotCount < MAX_CAL_SLOTS) {
      CalendarSlot& cs = calSlots[calSlotCount];
      cs.startSecs = (uint32_t)extractLong(buf, "s");
      cs.endSecs   = (uint32_t)extractLong(buf, "e");
      extractStr(buf, "n", cs.name,  sizeof(cs.name));
      cs.title[0] = '\0';
      extractStr(buf, "t", cs.title, sizeof(cs.title));
      cs.state  = (uint8_t)extractInt(buf, "st");
      cs.active = (cs.startSecs > 0);
      if (cs.active) calSlotCount++;
    }

  } else if (strcmp(cmd, "CALDONE") == 0) {
    // All CALSLOT packets received — draw the bookings on the calendar
    if (currentScreen == 1) {
      displayCalendarBookings(lcd, calSlots, calSlotCount, calTopHour);
    }

  } else if (strcmp(cmd, "MSG") == 0) {
    // FIX (#6): toast-style transient text message
    char text[64] = {0};
    extractStr(buf, "text", text, sizeof(text));
    displayMessage(lcd, text);

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
// NOTE: This uses the static `lcd` pointer captured by handleIncomingCommand —
// we rely on the calendar arrow/swipe handlers running locally on the Mega so
// scrolling stays snappy without an ESP32 round-trip. The lcd pointer is
// passed in via setRenderTarget() below.
static UTFT* _lcdPtr = nullptr;
void megaProtocolSetLcd(UTFT* lcd) { _lcdPtr = lcd; }

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
        // FIX: BOOK NOW button is only drawn when the room is available
        // (SCHEDULED / GHOST / COMPLETED).
        bool roomAvailable = (currentData.state == STATE_SCHEDULED ||
                              currentData.state == STATE_GHOST     ||
                              currentData.state == STATE_COMPLETED);
        if (roomAvailable &&
            downX >= 20 && downX <= 280 &&
            downY >= 230 && downY <= 300) {
          // Walk-up is admin-only: open the local PIN screen first.
          if (_lcdPtr) {
            currentScreen = 3;
            reportScreen(3);
            displayPinScreen(_lcdPtr);
          }
        }
      }
    }
    else if (currentScreen == 1) {
      // Calendar screen: vertical scrolling stays local for snappiness.
      if (_lcdPtr) {
        if (gesture == GESTURE_SWIPE_UP) {
          // Finger swiped up -> show later hours
          scrollCalendar(_lcdPtr, +CAL_VISIBLE_ROWS / 2);
        } else if (gesture == GESTURE_SWIPE_DOWN) {
          scrollCalendar(_lcdPtr, -CAL_VISIBLE_ROWS / 2);
        } else if (gesture == GESTURE_TAP) {
          // Up arrow tap target
          if (downX >= 740 && downX <= 790 &&
              downY >= 65  && downY <= 90) {
            scrollCalendar(_lcdPtr, -1);
          }
          // Down arrow tap target
          else if (downX >= 740 && downX <= 790 &&
                   downY >= 442 && downY <= 475) {
            scrollCalendar(_lcdPtr, +1);
          }
        }
      }
    }
    else if (currentScreen == 2) {
      // Combined duration + purpose picker. Layout must match
      // display_render.cpp::displayBookNowScreen().
      if (gesture == GESTURE_TAP && _lcdPtr) {
        // Duration row: y=110..170
        if (downY >= 110 && downY <= 170) {
          for (int i = 0; i < 4; i++) {
            int x = 20 + i * 195;
            if (downX >= x && downX <= x + 180) {
              bookNowSelectDuration(_lcdPtr, (int8_t)i);
              break;
            }
          }
        }
        // Purpose row: y=220..280
        else if (downY >= 220 && downY <= 280) {
          for (int i = 0; i < 5; i++) {
            int x = 20 + i * 156;
            if (downX >= x && downX <= x + 150) {
              bookNowSelectPurpose(_lcdPtr, (int8_t)i);
              break;
            }
          }
        }
        // Confirm button: x=260..540, y=400..460
        else if (downX >= 260 && downX <= 540 &&
                 downY >= 400 && downY <= 460) {
          if (bookNowIsReady()) {
            char out[96];
            const char* purpose = bookNowGetPurpose();
            snprintf(out, sizeof(out),
                     "{\"evt\":\"BOOK\",\"dur\":%d,\"purpose\":\"%s\"}\n",
                     bookNowGetDurationMins(), purpose);
            Serial2.print(out);
          }
        }
        // Cancel button: x=20..180, y=400..460
        else if (downX >= 20 && downX <= 180 &&
                 downY >= 400 && downY <= 460) {
          currentScreen = 0;
          reportScreen(0);
          resetStatusScreenCache();
          Serial2.print("{\"evt\":\"TOUCH\",\"gesture\":\"CANCEL\"}\n");
        }
      }
    }
    else if (currentScreen == 3) {
      // PIN entry keypad. Layout must match display_render.cpp::displayPinScreen().
      if (gesture == GESTURE_TAP && _lcdPtr) {
        // Cancel button: x=20..180, y=100..170
        if (downX >= 20 && downX <= 180 &&
            downY >= 100 && downY <= 170) {
          currentScreen = 0;
          reportScreen(0);
          resetStatusScreenCache();
          Serial2.print("{\"evt\":\"TOUCH\",\"gesture\":\"CANCEL\"}\n");
          return;
        }
        // 3x4 keypad starting at x=230, y=100, cells 140x70 with 10px gaps
        if (downX >= 230 && downX <= 680 &&
            downY >= 100 && downY <= 420) {
          int col = (downX - 230) / 150;
          int row = (downY - 100) / 80;
          if (col < 0) col = 0; if (col > 2) col = 2;
          if (row < 0) row = 0; if (row > 3) row = 3;
          int idx = row * 3 + col;
          const char* keys[12] = {"1","2","3","4","5","6","7","8","9","C","0","OK"};
          const char* k = keys[idx];
          if (strcmp(k, "C") == 0) {
            pinClear(_lcdPtr);
          } else if (strcmp(k, "OK") == 0) {
            if (strcmp(pinGetBuffer(), MEGA_ADMIN_PIN) == 0) {
              currentScreen = 2;
              reportScreen(2);
              displayBookNowScreen(_lcdPtr);
            } else {
              displayMessage(_lcdPtr, "Wrong PIN");
              pinClear(_lcdPtr);
            }
          } else {
            pinAppendDigit(_lcdPtr, k[0]);
          }
        }
      }
    }
  }
}
