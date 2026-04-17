#include "mega_protocol.h"
#include "display_render.h"
#include "touch_gt9271.h"
#include <Arduino.h>
#include <time.h>

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

// Current-week bounds sent by the ESP32 in CALDONE. Used to filter slots in
// displayCalendarBookings so next-week bookings don't appear in this week's grid.
// Both are 0 until the first CALDONE arrives (= show all, no filter).
static uint32_t     calWeekStart  = 0;
static uint32_t     calWeekEnd    = 0;

// Helper: redraw calendar with current slot buffer at current scroll offset
static void redrawCalendar(UTFT* lcd) {
  displayCalendarScreen(lcd, calTopHour, calWeekStart, calWeekEnd);
  displayCalendarBookings(lcd, calSlots, calSlotCount, calTopHour, calWeekStart, calWeekEnd);
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

  // Echo every command we see. Helps diagnose whether CALSLOT/CALDONE
  // actually reach the Mega vs. being dropped at the UART layer.
  Serial.print(F("[Cmd] ")); Serial.println(cmd);

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
    currentData.upcomingOccupant[0] = '\0';
    currentData.upcomingTitle[0]    = '\0';
    currentData.upcomingStart[0]    = '\0';
    currentData.upcomingEnd[0]      = '\0';
    currentData.upcomingDate[0]     = '\0';
    extractStr(buf, "uOcc",   currentData.upcomingOccupant, sizeof(currentData.upcomingOccupant));
    extractStr(buf, "uTitle", currentData.upcomingTitle,    sizeof(currentData.upcomingTitle));
    extractStr(buf, "uStart", currentData.upcomingStart,    sizeof(currentData.upcomingStart));
    extractStr(buf, "uEnd",   currentData.upcomingEnd,      sizeof(currentData.upcomingEnd));
    extractStr(buf, "uDate",  currentData.upcomingDate,     sizeof(currentData.upcomingDate));
    displayStatusScreen(lcd, &currentData);

  } else if (strcmp(cmd, "CALENDAR") == 0) {
    // Reset calendar slot buffer + reset scroll to top of day
    memset(calSlots, 0, sizeof(calSlots));
    currentScreen = 1;
    reportScreen(1);
    calSlotCount  = 0;
    calTopHour    = CAL_DAY_START_HOUR;
    calWeekStart  = 0;
    calWeekEnd    = 0;
    displayCalendarScreen(lcd, calTopHour, calWeekStart, calWeekEnd);

  } else if (strcmp(cmd, "CALRESET") == 0) {
    // Lightweight "clear slot buffer" that skips the heavy clrScr redraw.
    // Used by the 1Hz hash-refresh path on the ESP32 when it needs to
    // resend fresh data but the calendar grid is already on screen.
    memset(calSlots, 0, sizeof(calSlots));
    calSlotCount = 0;

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
      Serial.print(F("[Cal] slot s=")); Serial.print(cs.startSecs);
      Serial.print(F(" e="));           Serial.print(cs.endSecs);
      Serial.print(F(" st="));          Serial.print(cs.state);
      Serial.print(F(" n='"));          Serial.print(cs.name);
      Serial.print(F("' count="));      Serial.println(calSlotCount);
    } else {
      Serial.println(F("[Cal] CALSLOT dropped: buffer full"));
    }

  } else if (strcmp(cmd, "CALDONE") == 0) {
    Serial.print(F("[Cal] CALDONE received, count="));
    Serial.print(calSlotCount);
    Serial.print(F(" screen="));
    Serial.println(currentScreen);
    if (currentScreen == 1) {
      // If the ESP32 told us how many slots it sent and we received fewer,
      // a CALSLOT was dropped on the UART. Ask for a fresh send instead of
      // rendering an incomplete grid.
      long expected = extractLong(buf, "n");
      if (expected > 0 && (uint8_t)expected != calSlotCount) {
        Serial.print(F("[Cal] CALDONE mismatch: expected="));
        Serial.print(expected);
        Serial.print(F(" got="));
        Serial.println(calSlotCount);
        Serial2.print("{\"evt\":\"CALRETRY\"}\n");
        return;
      }
      // Compute this week's Monday 00:00 and Sunday 23:59:59 from the ESP32 now.
      long nowSecs = extractLong(buf, "now");
      if (nowSecs > 1000000000L) {
        // nowSecs is Kigali local time (Unix + 7200). Compute Monday of this week.
        // tm_wday: 0=Sun, 1=Mon, ..., 6=Sat. Days since Monday = (wday+6)%7.
        time_t nowT = (time_t)nowSecs;
        time_t nowAdj = (nowT > UNIX_OFFSET) ? (nowT - UNIX_OFFSET) : 0;
        struct tm* tn = localtime(&nowAdj);
        if (tn) {
          int daysSinceMon = (tn->tm_wday + 6) % 7;
          // Strip to midnight of today (avr), then subtract extra days
          struct tm monMid = *tn;
          monMid.tm_hour = 0; monMid.tm_min = 0; monMid.tm_sec = 0;
          time_t monAdj = mktime(&monMid) - (time_t)(daysSinceMon * 86400UL);
          // Convert back to Unix (Kigali) timestamps for the filter
          calWeekStart = (uint32_t)(monAdj + UNIX_OFFSET);
          calWeekEnd   = calWeekStart + 7 * 86400UL - 1;
          Serial.print(F("[Cal] weekStart=")); Serial.print(calWeekStart);
          Serial.print(F(" weekEnd="));        Serial.println(calWeekEnd);
          // Refresh ONLY the navy header band so the date range appears
          // alongside "This Week" without wiping the grid we're about to
          // paint bookings into. Full clrScr-redraw here was causing the
          // third visible flash on calendar open.
          displayCalendarHeader(lcd, calWeekStart, calWeekEnd);
        }
      }

      uint8_t earliestHour = CAL_DAY_END_HOUR + 1;
      for (uint8_t i = 0; i < calSlotCount; i++) {
        if (!calSlots[i].active) continue;
        // Only consider slots within this week for auto-scroll target
        if (calWeekStart > 0 && calWeekEnd > calWeekStart) {
          if (calSlots[i].startSecs < calWeekStart || calSlots[i].startSecs > calWeekEnd) continue;
        }
        time_t st = (time_t)calSlots[i].startSecs;
        time_t adjusted = (st > UNIX_OFFSET) ? (st - UNIX_OFFSET) : 0;
        struct tm* tmS = localtime(&adjusted);
        if (!tmS) {
          Serial.print(F("[Cal] slot ")); Serial.print(i); Serial.println(F(": localtime NULL"));
          continue;
        }
        Serial.print(F("[Cal] slot ")); Serial.print(i);
        Serial.print(F(" tm_wday=")); Serial.print(tmS->tm_wday);
        Serial.print(F(" tm_hour=")); Serial.print(tmS->tm_hour);
        Serial.print(F(" tm_min="));  Serial.println(tmS->tm_min);
        if (tmS->tm_hour < earliestHour) earliestHour = tmS->tm_hour;
      }
      if (earliestHour <= CAL_DAY_END_HOUR) {
        int maxTop = CAL_DAY_END_HOUR - CAL_VISIBLE_ROWS + 1;
        int target = earliestHour;
        if (target < CAL_DAY_START_HOUR) target = CAL_DAY_START_HOUR;
        if (target > maxTop)             target = maxTop;
        calTopHour = (uint8_t)target;
      }
      Serial.print(F("[Cal] drawing topHour=")); Serial.println(calTopHour);
      displayCalendarBookings(lcd, calSlots, calSlotCount, calTopHour, calWeekStart, calWeekEnd);
      Serial.println(F("[Cal] draw done"));
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
        // Switch to the calendar locally so the heavy LCD redraw
        // (clrScr + grid + day strip) happens BEFORE the ESP32 starts
        // streaming CALSLOT bytes. Otherwise the Mega is blocked in LCD
        // work while the first CALSLOT lands in the 64B RX ring and gets
        // truncated. The ESP32 still gets the "L" touch event and will
        // stream fresh calendar data — but it arrives into an idle Mega.
        if (_lcdPtr) {
          memset(calSlots, 0, sizeof(calSlots));
          calSlotCount  = 0;
          calTopHour    = CAL_DAY_START_HOUR;
          calWeekStart  = 0;
          calWeekEnd    = 0;
          currentScreen = 1;
          reportScreen(1);
          displayCalendarScreen(_lcdPtr, calTopHour, calWeekStart, calWeekEnd);
        }
        Serial2.print("{\"evt\":\"TOUCH\",\"gesture\":\"L\"}\n");
      } else if (gesture == GESTURE_TAP) {
        // BOOK NOW is only drawn when the room is truly free (no scheduled,
        // pending, or active booking). SCHEDULED means a reservation is
        // already on the calendar for this slot — walk-up must be refused.
        bool roomAvailable = (currentData.state == STATE_GHOST ||
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
      // Swipe jumps by a full visible page (8h); arrow taps jump 5h.
      const int CAL_SWIPE_STEP = CAL_VISIBLE_ROWS;   // 8h, one full page
      const int CAL_ARROW_STEP = 5;                   // 5h chunks on arrow tap
      if (_lcdPtr) {
        if (gesture == GESTURE_SWIPE_UP) {
          // Finger swiped up -> show later hours
          scrollCalendar(_lcdPtr, +CAL_SWIPE_STEP);
        } else if (gesture == GESTURE_SWIPE_DOWN) {
          scrollCalendar(_lcdPtr, -CAL_SWIPE_STEP);
        } else if (gesture == GESTURE_TAP) {
          // Up arrow tap target
          if (downX >= 740 && downX <= 790 &&
              downY >= 65  && downY <= 90) {
            scrollCalendar(_lcdPtr, -CAL_ARROW_STEP);
          }
          // Down arrow tap target
          else if (downX >= 740 && downX <= 790 &&
                   downY >= 442 && downY <= 475) {
            scrollCalendar(_lcdPtr, +CAL_ARROW_STEP);
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
