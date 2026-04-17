#include <Arduino.h>
#include <time.h>
#include <UTFT.h>
#include "display_render.h"

// UNIX_OFFSET now lives in display_render.h so both the render code and
// the touch/protocol layer can share it.

static struct tm* calendarLocaltime(const time_t* t) {
  time_t adjusted = (*t > UNIX_OFFSET) ? (*t - UNIX_OFFSET) : 0;
  return localtime(&adjusted);
}

extern uint8_t BigFont[];
extern uint8_t SmallFont[];

#define COL_BG      0,   0,   0
#define COL_WHITE 255, 255, 255
#define COL_GREEN   0, 200,   0
#define COL_RED   220,  30,  30
#define COL_AMBER 255, 160,   0
#define COL_GRAY  120, 120, 120
#define COL_NAVY   20,  20,  50
#define SCR_W 800
#define SCR_H 480

static void drawPanel(UTFT* lcd, int x, int y, int w, int h,
                      uint8_t r, uint8_t g, uint8_t b) {
  lcd->setColor(r, g, b);
  lcd->fillRect(x, y, x + w, y + h);
}

static void lcdPrint(UTFT* lcd, const char* s, int x, int y) {
  lcd->print(const_cast<char*>(s), x, y);
}

uint16_t stateColour(uint8_t state) {
  switch (state) {
    case STATE_SCHEDULED: return 0x07E0;
    case STATE_PENDING:   return 0xFD20;
    case STATE_ACTIVE:    return 0xF800;
    case STATE_GHOST:     return 0x7BEF;
    default:              return 0x07E0;
  }
}

const char* stateLabel(uint8_t state) {
  switch (state) {
    case STATE_SCHEDULED: return "RESERVED";
    case STATE_PENDING:   return "PENDING";
    case STATE_ACTIVE:    return "IN USE";
    case STATE_GHOST:
    case STATE_COMPLETED: return "AVAILABLE";
    default:              return "UNKNOWN";
  }
}

// ── Startup ───────────────────────────────────────────────────────────────────
void displayStartup(UTFT* lcd) {
  lcd->clrScr();
  drawPanel(lcd, 0, 0, SCR_W, 80, 20, 20, 80);
  lcd->setColor(COL_WHITE); lcd->setBackColor(20, 20, 80);
  lcd->setFont(BigFont);
  lcdPrint(lcd, "SmartRoom", CENTER, 22);
  lcd->setFont(SmallFont); lcd->setColor(180, 180, 180);
  lcdPrint(lcd, "CMU Africa  -  Embedded Systems Development", CENTER, 60);
  lcd->setBackColor(COL_BG); lcd->setColor(COL_GREEN);
  lcd->setFont(SmallFont);
  lcdPrint(lcd, "Initialising... connecting to network", CENTER, 240);
}

// ── Status screen ─────────────────────────────────────────────────────────────
// Three layers of change-gating to eliminate all unnecessary pixel writes:
//
//   Layer 1 (_firstDraw / _prevData): full screen clear + redraw only when
//   the booking state, room name, occupant, or session times change.
//
//   Layer 2 (_prevSecs): the countdown MM:SS area (y=360-390) is only touched
//   when the seconds value changes. During an active session this fires once
//   per second (correct). After a session ends _prevSecs goes from nonzero to 0
//   triggering exactly one erase, then nothing more.
//
//   Layer 3 (_prevOffline): the offline banner (y=455-480) is only drawn when
//   the connected/disconnected status actually changes.
//
// Together these eliminate the per-second SSD1963 scan-line flash that was
// visible after a booking ended and the room returned to AVAILABLE state.

static RoomDisplayData _prevData    = {0};
static bool            _firstDraw   = true;
static uint32_t        _prevSecs    = 0xFFFFFFFF; // sentinel: force first draw
static bool            _prevOffline = false;

void displayStatusScreen(UTFT* lcd, RoomDisplayData* d) {
  bool stateChanged = _firstDraw ||
                      (d->state != _prevData.state) ||
                      (strcmp(d->roomName,         _prevData.roomName)         != 0) ||
                      (strcmp(d->occupantName,     _prevData.occupantName)     != 0) ||
                      (strcmp(d->title,            _prevData.title)            != 0) ||
                      (strcmp(d->startTime,        _prevData.startTime)        != 0) ||
                      (strcmp(d->endTime,          _prevData.endTime)          != 0) ||
                      (strcmp(d->upcomingOccupant, _prevData.upcomingOccupant) != 0) ||
                      (strcmp(d->upcomingTitle,    _prevData.upcomingTitle)    != 0) ||
                      (strcmp(d->upcomingStart,    _prevData.upcomingStart)    != 0);

  if (stateChanged) {
    _firstDraw   = false;
    // Reset secondary trackers so the fresh screen gets its first countdown
    // and offline-banner draw on the very next STATUS packet.
    _prevSecs    = 0xFFFFFFFF;
    _prevOffline = !_prevOffline; // force offline banner redraw

    lcd->clrScr();
    lcd->setBackColor(COL_BG);

    drawPanel(lcd, 0, 0, SCR_W, 70, COL_NAVY);
    lcd->setColor(COL_WHITE); lcd->setBackColor(COL_NAVY);
    lcd->setFont(BigFont);
    lcdPrint(lcd, d->roomName, 20, 18);
    lcd->setFont(SmallFont); lcd->setColor(COL_GRAY);
    lcdPrint(lcd, "Swipe left: calendar", 530, 50);

    // SCHEDULED, PENDING and ACTIVE all mean "a booking exists" — show the
    // occupied/reserved panel and the session details, not BOOK NOW.
    bool available = (d->state == STATE_GHOST || d->state == STATE_COMPLETED);
    if (available) { drawPanel(lcd, 20, 90, 360, 120, 0, 140, 0);   lcd->setBackColor(0, 140, 0); }
    else           { drawPanel(lcd, 20, 90, 360, 120, 180, 20, 20); lcd->setBackColor(180, 20, 20); }
    lcd->setColor(COL_WHITE); lcd->setFont(BigFont);
    lcdPrint(lcd, stateLabel(d->state), 30, 130);
    lcd->setBackColor(COL_BG);

    if (d->state == STATE_ACTIVE || d->state == STATE_PENDING || d->state == STATE_SCHEDULED) {
      lcd->setFont(SmallFont); lcd->setColor(COL_GRAY);
      lcdPrint(lcd, "Booked by:", 20, 220);
      lcd->setColor(COL_WHITE); lcd->setFont(BigFont);
      lcdPrint(lcd, d->occupantName, 20, 238);

      if (d->title[0]) {
        lcd->setFont(SmallFont); lcd->setColor(COL_GRAY);
        lcdPrint(lcd, "Purpose:", 20, 270);
        lcd->setColor(COL_WHITE); lcd->setFont(BigFont);
        lcdPrint(lcd, d->title, 20, 288);
      }

      lcd->setFont(SmallFont); lcd->setColor(COL_GRAY);
      lcdPrint(lcd, "Session:", 20, 320);
      lcd->setColor(COL_WHITE);
      char timeBuf[32];
      strcpy(timeBuf, d->startTime); strcat(timeBuf, " - "); strcat(timeBuf, d->endTime);
      lcd->setFont(BigFont);
      lcdPrint(lcd, timeBuf, 20, 338);
    } else {
      // Book Now button — x:20-280, y:230-300
      drawPanel(lcd, 20, 230, 260, 70, 0, 100, 200);
      lcd->setBackColor(0, 100, 200); lcd->setColor(COL_WHITE); lcd->setFont(BigFont);
      lcdPrint(lcd, "BOOK NOW", 40, 255);
      lcd->setBackColor(COL_BG); lcd->setFont(SmallFont); lcd->setColor(COL_GRAY);
      lcdPrint(lcd, "Tap to reserve this room", 20, 320);
    }

    // "Up next" panel — right column, replaces the old LED legend.
    // Draw a soft navy card so the block feels like a unit, and only
    // when there's an actual upcoming reservation.
    if (d->upcomingStart[0]) {
      drawPanel(lcd, 430, 90, 350, 210, COL_NAVY);
      lcd->setBackColor(COL_NAVY); lcd->setColor(COL_GRAY);
      lcd->setFont(SmallFont);
      lcdPrint(lcd, "UP NEXT", 450, 100);

      lcd->setColor(COL_WHITE); lcd->setFont(BigFont);
      lcdPrint(lcd, d->upcomingStart, 450, 125);

      lcd->setColor(COL_GRAY); lcd->setFont(SmallFont);
      lcdPrint(lcd, "Booked by:", 450, 170);
      lcd->setColor(COL_WHITE); lcd->setFont(BigFont);
      lcdPrint(lcd, d->upcomingOccupant, 450, 188);

      if (d->upcomingTitle[0]) {
        lcd->setFont(SmallFont); lcd->setColor(COL_GRAY);
        lcdPrint(lcd, "Purpose:", 450, 225);
        lcd->setColor(COL_WHITE); lcd->setFont(BigFont);
        lcdPrint(lcd, d->upcomingTitle, 450, 243);
      }
      lcd->setBackColor(COL_BG);
    }

    memcpy(&_prevData, d, sizeof(RoomDisplayData));
  }

  // Layer 2: countdown — only update when the second value actually changes
  if (d->state == STATE_ACTIVE || d->state == STATE_PENDING) {
    if (d->countdownSecs != _prevSecs) {
      _prevSecs = d->countdownSecs;
      drawPanel(lcd, 20, 360, 400, 30, COL_BG);
      if (d->countdownSecs > 0) {
        uint16_t mm = (uint16_t)(d->countdownSecs / 60);
        uint8_t  ss = (uint8_t)(d->countdownSecs % 60);
        lcd->setFont(SmallFont);
        lcd->setColor(mm < 5 ? 255 : 180, mm < 5 ? 80 : 180, 80);
        char buf[32] = "";
        char tmp[8];
        if (mm < 10) strcat(buf, "0");
        itoa(mm, tmp, 10); strcat(buf, tmp);
        strcat(buf, ":");
        if (ss < 10) strcat(buf, "0");
        itoa(ss, tmp, 10); strcat(buf, tmp);
        strcat(buf, " remaining");
        lcd->setBackColor(COL_BG);
        lcdPrint(lcd, buf, 20, 370);
      }
    }
  } else {
    // Not active/pending: clear countdown area exactly once after state change
    if (_prevSecs != 0) {
      _prevSecs = 0;
      drawPanel(lcd, 20, 360, 400, 30, COL_BG);
    }
  }
}

// Layer 3: offline banner — only draw when connected status changes
void displayOfflineWarning(UTFT* lcd, bool show) {
  if (show == _prevOffline) return;  // No change — do nothing at all
  _prevOffline = show;
  if (show) {
    drawPanel(lcd, 0, 455, SCR_W, 25, 140, 80, 0);
    lcd->setBackColor(140, 80, 0); lcd->setColor(COL_WHITE); lcd->setFont(SmallFont);
    lcdPrint(lcd, "OFFLINE  Running on local cache  Events sync on reconnect", 10, 462);
    lcd->setBackColor(COL_BG);
  } else {
    drawPanel(lcd, 0, 455, SCR_W, 25, COL_BG);
  }
}

void resetStatusScreenCache() {
  _firstDraw   = true;
  _prevSecs    = 0xFFFFFFFF;
  _prevOffline = false;
  memset(&_prevData, 0, sizeof(_prevData));
}

// ── Calendar ──────────────────────────────────────────────────────────────────
//
// Day spans CAL_DAY_START_HOUR .. CAL_DAY_END_HOUR. Only CAL_VISIBLE_ROWS
// hours are visible at a time; the rest are reached by swiping up/down or
// tapping the on-screen arrows.
//
// Layout:
//   y= 0..60   header
//   y=65..90   day-of-week strip
//   y=95..471  hour grid (CAL_VISIBLE_ROWS rows of 47px each)
//   y=435..480 footer / scroll arrows
//
// The visible "top hour" is provided by the protocol layer so it survives
// re-renders when calendar slot data arrives later.

#define CAL_GRID_TOP_Y     95
#define CAL_ROW_HEIGHT     47
#define CAL_GRID_BOTTOM_Y  (CAL_GRID_TOP_Y + CAL_VISIBLE_ROWS * CAL_ROW_HEIGHT)

// Up/down arrow tap targets (right edge of header strip, easy to reach)
#define CAL_ARROW_X       740
#define CAL_ARROW_W        50
#define CAL_ARROW_UP_Y     65
#define CAL_ARROW_UP_H     25
#define CAL_ARROW_DN_Y    440
#define CAL_ARROW_DN_H     35

static void drawCalendarHourGrid(UTFT* lcd, uint8_t topHour) {
  lcd->setBackColor(COL_BG);
  int colW = SCR_W / 7;
  for (int row = 0; row < CAL_VISIBLE_ROWS; row++) {
    int y = CAL_GRID_TOP_Y + row * CAL_ROW_HEIGHT;
    lcd->setColor(40, 40, 40);
    lcd->drawLine(0, y, SCR_W, y);
    int hr = topHour + row;
    if (hr > CAL_DAY_END_HOUR) break;
    char hbuf[8] = "";
    if (hr < 10) strcat(hbuf, "0");
    char tmp[4]; itoa(hr, tmp, 10); strcat(hbuf, tmp);
    strcat(hbuf, ":00");
    lcd->setColor(COL_GRAY);
    lcd->print(hbuf, 2, y + 4);
  }
  // Faint vertical separators between weekday columns
  for (int i = 1; i < 7; i++) {
    lcd->setColor(30, 30, 30);
    lcd->drawLine(i * colW, CAL_GRID_TOP_Y, i * colW, CAL_GRID_BOTTOM_Y);
  }
}

static void drawCalendarArrows(UTFT* lcd, uint8_t topHour) {
  bool canUp   = (topHour > CAL_DAY_START_HOUR);
  bool canDown = (topHour + CAL_VISIBLE_ROWS - 1 < CAL_DAY_END_HOUR);

  // Up arrow button
  if (canUp) drawPanel(lcd, CAL_ARROW_X, CAL_ARROW_UP_Y, CAL_ARROW_W, CAL_ARROW_UP_H, 0, 100, 200);
  else       drawPanel(lcd, CAL_ARROW_X, CAL_ARROW_UP_Y, CAL_ARROW_W, CAL_ARROW_UP_H, 50, 50, 60);
  lcd->setBackColor(canUp ? 0 : 50, canUp ? 100 : 50, canUp ? 200 : 60);
  lcd->setColor(COL_WHITE); lcd->setFont(BigFont);
  lcd->print("^", CAL_ARROW_X + 18, CAL_ARROW_UP_Y + 4);

  // Down arrow button — sits in the footer band
  drawPanel(lcd, 0, CAL_ARROW_DN_Y, SCR_W, CAL_ARROW_DN_H, COL_BG);
  if (canDown) drawPanel(lcd, CAL_ARROW_X, CAL_ARROW_DN_Y + 2, CAL_ARROW_W, CAL_ARROW_DN_H - 4, 0, 100, 200);
  else         drawPanel(lcd, CAL_ARROW_X, CAL_ARROW_DN_Y + 2, CAL_ARROW_W, CAL_ARROW_DN_H - 4, 50, 50, 60);
  lcd->setBackColor(canDown ? 0 : 50, canDown ? 100 : 50, canDown ? 200 : 60);
  lcd->setColor(COL_WHITE); lcd->setFont(BigFont);
  lcd->print("v", CAL_ARROW_X + 18, CAL_ARROW_DN_Y + 8);

  // Range hint at bottom-left
  lcd->setBackColor(COL_BG); lcd->setColor(COL_GRAY); lcd->setFont(SmallFont);
  char range[24];
  uint8_t bottomHour = topHour + CAL_VISIBLE_ROWS - 1;
  if (bottomHour > CAL_DAY_END_HOUR) bottomHour = CAL_DAY_END_HOUR;
  snprintf(range, sizeof(range), "%02u:00 - %02u:00  (swipe)", topHour, bottomHour + 1);
  lcd->print(range, 10, CAL_ARROW_DN_Y + 12);
}

void displayCalendarScreen(UTFT* lcd, uint8_t topHour, uint32_t weekStart, uint32_t weekEnd) {
  lcd->clrScr();
  lcd->setBackColor(COL_BG);
  drawPanel(lcd, 0, 0, SCR_W, 60, COL_NAVY);
  lcd->setColor(COL_WHITE); lcd->setBackColor(COL_NAVY);
  lcd->setFont(SmallFont); lcd->setColor(COL_GRAY);
  lcdPrint(lcd, "Swipe right to go back", 500, 38);

  (void)weekStart;
  (void)weekEnd;

  char days[7][4] = {"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};
  int colW = SCR_W / 7;
  for (int i = 0; i < 7; i++) {
    drawPanel(lcd, i * colW + 2, 65, colW - 4, 25, 60, 60, 80);
    lcd->setColor(COL_WHITE); lcd->setBackColor(60, 60, 80);
    lcd->setFont(SmallFont);
    lcd->print(days[i], i * colW + 8, 70);
  }
  drawCalendarHourGrid(lcd, topHour);
  drawCalendarArrows(lcd, topHour);

  lcd->setBackColor(COL_BG); lcd->setColor(COL_GRAY); lcd->setFont(SmallFont);
  lcdPrint(lcd, "Loading bookings...", CENTER, CAL_ARROW_DN_Y + 12);
}

void displayCalendarBookings(UTFT* lcd, CalendarSlot* slots, uint8_t count,
                             uint8_t topHour, uint32_t weekStart, uint32_t weekEnd) {
  // Clear grid area + footer hint, then redraw grid lines
  drawPanel(lcd, 0, CAL_GRID_TOP_Y, SCR_W, CAL_GRID_BOTTOM_Y - CAL_GRID_TOP_Y, COL_BG);
  drawCalendarHourGrid(lcd, topHour);

  int colW = SCR_W / 7;

  if (count == 0) {
    lcd->setBackColor(COL_BG); lcd->setColor(COL_GRAY); lcd->setFont(SmallFont);
    lcdPrint(lcd, "No bookings this week", CENTER, CAL_ARROW_DN_Y + 12);
    drawCalendarArrows(lcd, topHour);
    return;
  }

  float topPx    = (float)CAL_GRID_TOP_Y;
  float bottomPx = (float)CAL_GRID_BOTTOM_Y;

  for (uint8_t i = 0; i < count; i++) {
    if (!slots[i].active) continue;
    // Skip bookings outside the current week when we have a valid week window.
    // weekStart == 0 means the ESP32 clock hasn't synced yet — show everything.
    if (weekStart > 0 && weekEnd > weekStart) {
      if (slots[i].startSecs < weekStart || slots[i].startSecs > weekEnd) continue;
    }
    time_t st = (time_t)slots[i].startSecs;
    time_t et = (time_t)slots[i].endSecs;
    struct tm* tmS = calendarLocaltime(&st);
    if (!tmS) continue;
    int dayOfWeek = tmS->tm_wday;
    int col = (dayOfWeek == 0) ? 6 : dayOfWeek - 1;
    int startHour = tmS->tm_hour, startMin = tmS->tm_min;
    struct tm* tmE = calendarLocaltime(&et);
    int endHour = tmE ? tmE->tm_hour : startHour + 1;
    int endMin  = tmE ? tmE->tm_min  : 0;

    float yStart = topPx + ((startHour - topHour) + startMin / 60.0f) * CAL_ROW_HEIGHT;
    float yEnd   = topPx + ((endHour   - topHour) + endMin   / 60.0f) * CAL_ROW_HEIGHT;

    Serial.print(F("[Draw] i=")); Serial.print(i);
    Serial.print(F(" col="));     Serial.print(col);
    Serial.print(F(" yS="));      Serial.print((int)yStart);
    Serial.print(F(" yE="));      Serial.println((int)yEnd);

    // Clip to visible window
    if (yEnd <= topPx || yStart >= bottomPx) {
      Serial.println(F("[Draw] clipped"));
      continue;
    }
    if (yStart < topPx)    yStart = topPx;
    if (yEnd   > bottomPx) yEnd   = bottomPx;

    int bx = col * colW + 3, bw = colW - 6;
    int by = (int)yStart + 1, bh = (int)(yEnd - yStart) - 2;
    if (bh < 4) bh = 4;
    if      (slots[i].state == STATE_ACTIVE)  drawPanel(lcd, bx, by, bw, bh, 180, 20,  20);
    else if (slots[i].state == STATE_PENDING) drawPanel(lcd, bx, by, bw, bh, 200, 120,  0);
    else                                       drawPanel(lcd, bx, by, bw, bh,  20,  80, 160);
    if (bh > 14) {
      lcd->setFont(SmallFont); lcd->setColor(COL_WHITE);
      uint8_t br  = (slots[i].state == STATE_ACTIVE ? 180 : (slots[i].state == STATE_PENDING ? 200 : 20));
      uint8_t bg_ = (slots[i].state == STATE_ACTIVE ? 20  : (slots[i].state == STATE_PENDING ? 120 : 80));
      uint8_t bb  = (slots[i].state == STATE_ACTIVE ? 20  : (slots[i].state == STATE_PENDING ?   0 : 160));
      lcd->setBackColor(br, bg_, bb);
      char nb[12] = {0};
      strncpy(nb, slots[i].name, min((int)(bw / 8), 11));
      lcd->print(nb, bx + 2, by + 2);
    }
  }
  drawCalendarArrows(lcd, topHour);
}

// ── Toast message overlay ────────────────────────────────────────────────────
// FIX (#6): Drawn over whatever screen is active. Used by the ESP32 to surface
// transient feedback like "Room unavailable" after a refused walk-up booking.
void displayMessage(UTFT* lcd, const char* text) {
  drawPanel(lcd, 80, 380, 640, 60, 80, 20, 20);
  lcd->setBackColor(80, 20, 20); lcd->setColor(COL_WHITE); lcd->setFont(BigFont);
  // Truncate to fit visually (~36 chars at this width)
  char trimmed[40];
  strncpy(trimmed, text, sizeof(trimmed) - 1);
  trimmed[sizeof(trimmed) - 1] = '\0';
  lcdPrint(lcd, trimmed, CENTER, 398);
  lcd->setBackColor(COL_BG);
}

// ── Book Now (combined picker) ────────────────────────────────────────────────
// Layout (used by the touch router in mega_protocol.cpp — keep in sync):
//
//   Header  y=0..70
//   "Duration" label       y=85
//   Duration chips row     y=110..170   (4 chips, 180px wide, 15px gap)
//     chip 0: x= 20..200   = 1 min
//     chip 1: x=215..395   = 15 min
//     chip 2: x=410..590   = 30 min
//     chip 3: x=605..785   = 60 min
//   "Purpose" label        y=195
//   Purpose chips row      y=220..280   (5 chips, 150px wide, 6px gap)
//     chip 0: x= 20..170   Meeting
//     chip 1: x=176..326   Class
//     chip 2: x=332..482   Study
//     chip 3: x=488..638   Event
//     chip 4: x=644..794   Other
//   Selection summary      y=300
//   Confirm button         x=260..540, y=400..460
//   Cancel button          x=20..180,  y=400..460

static void drawChip(UTFT* lcd, int x, int y, int w, int h,
                     const char* label, bool selected) {
  if (selected) drawPanel(lcd, x, y, w, h, 0, 160, 220);
  else          drawPanel(lcd, x, y, w, h, 0,  80, 160);
  lcd->setBackColor(selected ? 0 : 0, selected ? 160 : 80, selected ? 220 : 160);
  lcd->setColor(COL_WHITE); lcd->setFont(BigFont);
  int textW = strlen(label) * 16;
  int tx = x + (w - textW) / 2;
  if (tx < x + 4) tx = x + 4;
  lcd->print(const_cast<char*>(label), tx, y + (h - 16) / 2);
  lcd->setBackColor(COL_BG);
}

static const char* const BOOK_DURATIONS[4] = {"1 min", "15 min", "30 min", "60 min"};
static const int BOOK_DURATION_MINS[4]     = {1, 15, 30, 60};
static const char* const BOOK_PURPOSES[5]  = {"Meeting", "Class", "Study", "Event", "Other"};

static int8_t _selectedDurIdx     = -1;
static int8_t _selectedPurposeIdx = -1;

void displayBookNowScreen(UTFT* lcd) {
  _selectedDurIdx     = -1;
  _selectedPurposeIdx = -1;

  lcd->clrScr(); lcd->setBackColor(COL_BG);
  drawPanel(lcd, 0, 0, SCR_W, 70, COL_NAVY);
  lcd->setColor(COL_WHITE); lcd->setBackColor(COL_NAVY);
  lcd->setFont(BigFont); lcdPrint(lcd, "Book this room", 20, 20);
  lcd->setBackColor(COL_BG);

  // Duration row
  lcd->setFont(SmallFont); lcd->setColor(COL_GRAY);
  lcdPrint(lcd, "Duration:", 20, 85);
  for (int i = 0; i < 4; i++) {
    int x = 20 + i * 195;
    drawChip(lcd, x, 110, 180, 60, BOOK_DURATIONS[i], false);
  }

  // Purpose row
  lcd->setFont(SmallFont); lcd->setColor(COL_GRAY);
  lcdPrint(lcd, "Purpose:", 20, 195);
  for (int i = 0; i < 5; i++) {
    int x = 20 + i * 156;
    drawChip(lcd, x, 220, 150, 60, BOOK_PURPOSES[i], false);
  }

  // Selection summary placeholder
  lcd->setFont(SmallFont); lcd->setColor(COL_GRAY);
  lcdPrint(lcd, "Pick a duration and a purpose, then Confirm.", 20, 310);

  // Confirm + Cancel buttons
  drawPanel(lcd, 260, 400, 280, 60, 60, 60, 60);  // disabled look until both picked
  lcd->setBackColor(60, 60, 60); lcd->setColor(COL_WHITE); lcd->setFont(BigFont);
  lcdPrint(lcd, "Confirm", 340, 418);

  drawPanel(lcd, 20, 400, 160, 60, 120, 20, 20);
  lcd->setBackColor(120, 20, 20); lcd->setColor(COL_WHITE); lcd->setFont(BigFont);
  lcdPrint(lcd, "Cancel", 50, 418);
  lcd->setBackColor(COL_BG);
}

// Called by the touch router after a chip tap. Updates the picker state and
// repaints only the affected chip / confirm button (no full-screen redraw).
void bookNowSelectDuration(UTFT* lcd, int8_t idx) {
  if (idx < 0 || idx >= 4) return;
  if (_selectedDurIdx == idx) return;
  // Redraw the previously selected chip as unselected
  if (_selectedDurIdx >= 0) {
    int x = 20 + _selectedDurIdx * 195;
    drawChip(lcd, x, 110, 180, 60, BOOK_DURATIONS[_selectedDurIdx], false);
  }
  _selectedDurIdx = idx;
  int x = 20 + idx * 195;
  drawChip(lcd, x, 110, 180, 60, BOOK_DURATIONS[idx], true);
  bookNowRefreshConfirm(lcd);
}

void bookNowSelectPurpose(UTFT* lcd, int8_t idx) {
  if (idx < 0 || idx >= 5) return;
  if (_selectedPurposeIdx == idx) return;
  if (_selectedPurposeIdx >= 0) {
    int x = 20 + _selectedPurposeIdx * 156;
    drawChip(lcd, x, 220, 150, 60, BOOK_PURPOSES[_selectedPurposeIdx], false);
  }
  _selectedPurposeIdx = idx;
  int x = 20 + idx * 156;
  drawChip(lcd, x, 220, 150, 60, BOOK_PURPOSES[idx], true);
  bookNowRefreshConfirm(lcd);
}

void bookNowRefreshConfirm(UTFT* lcd) {
  bool ready = (_selectedDurIdx >= 0 && _selectedPurposeIdx >= 0);
  if (ready) drawPanel(lcd, 260, 400, 280, 60, 0, 140, 0);
  else       drawPanel(lcd, 260, 400, 280, 60, 60, 60, 60);
  lcd->setBackColor(ready ? 0 : 60, ready ? 140 : 60, ready ? 0 : 60);
  lcd->setColor(COL_WHITE); lcd->setFont(BigFont);
  lcdPrint(lcd, "Confirm", 340, 418);
  lcd->setBackColor(COL_BG);
}

int bookNowGetDurationMins() {
  if (_selectedDurIdx < 0) return 0;
  return BOOK_DURATION_MINS[_selectedDurIdx];
}

const char* bookNowGetPurpose() {
  if (_selectedPurposeIdx < 0) return "";
  return BOOK_PURPOSES[_selectedPurposeIdx];
}

bool bookNowIsReady() {
  return (_selectedDurIdx >= 0 && _selectedPurposeIdx >= 0);
}

// ── PIN entry screen ──────────────────────────────────────────────────────────
// 3x4 keypad:  1 2 3
//              4 5 6
//              7 8 9
//              C 0 OK
// Keys are 140x70 cells starting at x=230, y=100, with 10px gaps.
// Entry display row: y=60..90 centered.

static char _pinBuf[8] = {0};
static uint8_t _pinLen = 0;
#define PIN_MAX_LEN 6

static void drawPinEntry(UTFT* lcd) {
  drawPanel(lcd, 230, 60, 340, 30, COL_BG);
  lcd->setBackColor(COL_BG); lcd->setColor(COL_WHITE); lcd->setFont(BigFont);
  char masked[PIN_MAX_LEN + 1] = {0};
  for (uint8_t i = 0; i < _pinLen; i++) masked[i] = '*';
  masked[_pinLen] = '\0';
  int w = _pinLen * 16;
  int x = 400 - w / 2;
  lcd->print(masked[0] ? masked : "____", x, 65);
}

void displayPinScreen(UTFT* lcd) {
  _pinLen = 0;
  _pinBuf[0] = '\0';

  lcd->clrScr(); lcd->setBackColor(COL_BG);
  drawPanel(lcd, 0, 0, SCR_W, 50, COL_NAVY);
  lcd->setColor(COL_WHITE); lcd->setBackColor(COL_NAVY);
  lcd->setFont(BigFont); lcdPrint(lcd, "Admin PIN", 20, 12);
  lcd->setBackColor(COL_BG);

  drawPinEntry(lcd);

  const char* keys[12] = {"1","2","3","4","5","6","7","8","9","C","0","OK"};
  for (int row = 0; row < 4; row++) {
    for (int col = 0; col < 3; col++) {
      int idx = row * 3 + col;
      int x = 230 + col * 150;
      int y = 100 + row * 80;
      uint8_t r = 0, g = 80, b = 160;
      if (strcmp(keys[idx], "OK") == 0) { r = 0; g = 140; b = 0; }
      else if (strcmp(keys[idx], "C") == 0) { r = 120; g = 20; b = 20; }
      drawPanel(lcd, x, y, 140, 70, r, g, b);
      lcd->setBackColor(r, g, b); lcd->setColor(COL_WHITE); lcd->setFont(BigFont);
      int labelW = strlen(keys[idx]) * 16;
      int tx = x + (140 - labelW) / 2;
      lcd->print(const_cast<char*>(keys[idx]), tx, y + 27);
    }
  }

  drawPanel(lcd, 20, 100, 160, 70, 120, 20, 20);
  lcd->setBackColor(120, 20, 20); lcd->setColor(COL_WHITE); lcd->setFont(BigFont);
  lcdPrint(lcd, "Cancel", 45, 127);
  lcd->setBackColor(COL_BG);
}

void pinAppendDigit(UTFT* lcd, char d) {
  if (_pinLen >= PIN_MAX_LEN) return;
  _pinBuf[_pinLen++] = d;
  _pinBuf[_pinLen]   = '\0';
  drawPinEntry(lcd);
}

void pinClear(UTFT* lcd) {
  _pinLen = 0;
  _pinBuf[0] = '\0';
  drawPinEntry(lcd);
}

const char* pinGetBuffer() {
  return _pinBuf;
}

// ── Confirmation ──────────────────────────────────────────────────────────────
void displayConfirmation(UTFT* lcd, bool success) {
  if (success) {
    drawPanel(lcd, 150, 170, 500, 140, 0, 120, 0);
    lcd->setBackColor(0, 120, 0); lcd->setColor(COL_WHITE); lcd->setFont(BigFont);
    lcdPrint(lcd, "Booking confirmed!", 170, 220);
  } else {
    drawPanel(lcd, 150, 170, 500, 140, 160, 0, 0);
    lcd->setBackColor(160, 0, 0); lcd->setColor(COL_WHITE); lcd->setFont(BigFont);
    lcdPrint(lcd, "Booking failed.", 170, 210);
    lcd->setFont(SmallFont); lcdPrint(lcd, "Check network or try again.", 170, 255);
  }
  lcd->setBackColor(COL_BG);
}
