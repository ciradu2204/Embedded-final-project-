#include <Arduino.h>
#include <time.h>
#include <UTFT.h>
#include "display_render.h"

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
    case STATE_SCHEDULED: return "AVAILABLE";
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
                      (strcmp(d->roomName,     _prevData.roomName)     != 0) ||
                      (strcmp(d->occupantName, _prevData.occupantName) != 0) ||
                      (strcmp(d->startTime,    _prevData.startTime)    != 0) ||
                      (strcmp(d->endTime,      _prevData.endTime)      != 0);

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

    bool available = (d->state == STATE_SCHEDULED ||
                      d->state == STATE_GHOST      ||
                      d->state == STATE_COMPLETED);
    if (available) { drawPanel(lcd, 20, 90, 360, 120, 0, 140, 0);   lcd->setBackColor(0, 140, 0); }
    else           { drawPanel(lcd, 20, 90, 360, 120, 180, 20, 20); lcd->setBackColor(180, 20, 20); }
    lcd->setColor(COL_WHITE); lcd->setFont(BigFont);
    lcdPrint(lcd, stateLabel(d->state), 30, 130);
    lcd->setBackColor(COL_BG);

    if (d->state == STATE_ACTIVE || d->state == STATE_PENDING) {
      lcd->setFont(SmallFont); lcd->setColor(COL_GRAY);
      lcdPrint(lcd, "Booked by:", 20, 225);
      lcd->setColor(COL_WHITE); lcd->setFont(BigFont);
      lcdPrint(lcd, d->occupantName, 20, 245);
      lcd->setFont(SmallFont); lcd->setColor(COL_GRAY);
      lcdPrint(lcd, "Session:", 20, 295);
      lcd->setColor(COL_WHITE);
      char timeBuf[32];
      strcpy(timeBuf, d->startTime); strcat(timeBuf, " - "); strcat(timeBuf, d->endTime);
      lcd->setFont(BigFont);
      lcdPrint(lcd, timeBuf, 20, 315);
    } else {
      // Book Now button — x:20-280, y:230-300
      drawPanel(lcd, 20, 230, 260, 70, 0, 100, 200);
      lcd->setBackColor(0, 100, 200); lcd->setColor(COL_WHITE); lcd->setFont(BigFont);
      lcdPrint(lcd, "BOOK NOW", 40, 255);
      lcd->setBackColor(COL_BG); lcd->setFont(SmallFont); lcd->setColor(COL_GRAY);
      lcdPrint(lcd, "Tap to reserve this room", 20, 320);
    }

    bool avail2 = (d->state == STATE_SCHEDULED || d->state == STATE_GHOST || d->state == STATE_COMPLETED);
    lcd->setFont(SmallFont); lcd->setColor(COL_GRAY);
    lcdPrint(lcd, "LED indicator:", 430, 100);
    if (avail2) { lcd->setColor(COL_GREEN); lcdPrint(lcd, "GREEN  (available)", 430, 118); }
    else        { lcd->setColor(COL_RED);   lcdPrint(lcd, "RED  (occupied)",    430, 118); }

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
void displayCalendarScreen(UTFT* lcd) {
  lcd->clrScr();
  lcd->setBackColor(COL_BG);
  drawPanel(lcd, 0, 0, SCR_W, 60, COL_NAVY);
  lcd->setColor(COL_WHITE); lcd->setBackColor(COL_NAVY);
  lcd->setFont(BigFont); lcdPrint(lcd, "This Week", 20, 15);
  lcd->setFont(SmallFont); lcd->setColor(COL_GRAY);
  lcdPrint(lcd, "Swipe right to go back", 530, 38);

  char days[7][4] = {"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};
  int colW = SCR_W / 7;
  for (int i = 0; i < 7; i++) {
    drawPanel(lcd, i * colW + 2, 65, colW - 4, 25, 60, 60, 80);
    lcd->setColor(COL_WHITE); lcd->setBackColor(60, 60, 80);
    lcd->setFont(SmallFont);
    lcd->print(days[i], i * colW + 8, 70);
  }
  lcd->setBackColor(COL_BG);
  for (int row = 0; row < 8; row++) {
    int y = 95 + row * 47;
    lcd->setColor(40, 40, 40);
    lcd->drawLine(0, y, SCR_W, y);
    char hbuf[8] = "";
    int hr = 8 + row;
    if (hr < 10) strcat(hbuf, "0");
    char tmp[4]; itoa(hr, tmp, 10); strcat(hbuf, tmp);
    strcat(hbuf, ":00");
    lcd->setColor(COL_GRAY);
    lcd->print(hbuf, 2, y + 4);
  }
  lcd->setColor(COL_GRAY); lcd->setFont(SmallFont);
  lcdPrint(lcd, "Loading bookings...", CENTER, 440);
}

void displayCalendarBookings(UTFT* lcd, CalendarSlot* slots, uint8_t count) {
  drawPanel(lcd, 0, 95, SCR_W, 370, COL_BG);
  lcd->setBackColor(COL_BG);
  int colW = SCR_W / 7;
  for (int row = 0; row < 8; row++) {
    int y = 95 + row * 47;
    lcd->setColor(40, 40, 40); lcd->drawLine(0, y, SCR_W, y);
    char hbuf[8] = "";
    int hr = 8 + row;
    if (hr < 10) strcat(hbuf, "0");
    char tmp[4]; itoa(hr, tmp, 10); strcat(hbuf, tmp);
    strcat(hbuf, ":00");
    lcd->setColor(COL_GRAY); lcd->print(hbuf, 2, y + 4);
  }
  if (count == 0) {
    lcd->setColor(COL_GRAY); lcd->setFont(SmallFont);
    lcdPrint(lcd, "No bookings this week", CENTER, 440);
    return;
  }
  for (uint8_t i = 0; i < count; i++) {
    if (!slots[i].active) continue;
    time_t st = (time_t)slots[i].startSecs;
    time_t et = (time_t)slots[i].endSecs;
    struct tm* tmS = localtime(&st);
    if (!tmS) continue;
    int dayOfWeek = tmS->tm_wday;
    int col = (dayOfWeek == 0) ? 6 : dayOfWeek - 1;
    int startHour = tmS->tm_hour, startMin = tmS->tm_min;
    struct tm* tmE = localtime(&et);
    int endHour = tmE ? tmE->tm_hour : startHour + 1;
    int endMin  = tmE ? tmE->tm_min  : 0;
    float yStart = 95.0f + (startHour - 8 + startMin / 60.0f) * 47.0f;
    float yEnd   = 95.0f + (endHour   - 8 + endMin   / 60.0f) * 47.0f;
    if (yEnd <= 95 || yStart >= 471) continue;
    if (yStart < 95) yStart = 95;
    if (yEnd > 471)  yEnd   = 471;
    int bx = col * colW + 3, bw = colW - 6;
    int by = (int)yStart + 1, bh = (int)(yEnd - yStart) - 2;
    if (bh < 4) bh = 4;
    if      (slots[i].state == STATE_ACTIVE)  drawPanel(lcd, bx, by, bw, bh, 180, 20,  20);
    else if (slots[i].state == STATE_PENDING) drawPanel(lcd, bx, by, bw, bh, 200, 120,  0);
    else                                       drawPanel(lcd, bx, by, bw, bh,  20,  80, 160);
    if (bh > 14) {
      lcd->setFont(SmallFont); lcd->setColor(COL_WHITE);
      uint8_t br = (slots[i].state == STATE_ACTIVE ? 180 : (slots[i].state == STATE_PENDING ? 200 : 20));
      uint8_t bg_ = (slots[i].state == STATE_ACTIVE ? 20  : (slots[i].state == STATE_PENDING ? 120 : 80));
      uint8_t bb  = (slots[i].state == STATE_ACTIVE ? 20  : (slots[i].state == STATE_PENDING ?   0 : 160));
      lcd->setBackColor(br, bg_, bb);
      char nb[12] = {0};
      strncpy(nb, slots[i].name, min((int)(bw / 8), 11));
      lcd->print(nb, bx + 2, by + 2);
    }
  }
  drawPanel(lcd, 0, 435, SCR_W, 20, COL_BG);
}

// ── Book Now ──────────────────────────────────────────────────────────────────
void displayBookNowScreen(UTFT* lcd) {
  lcd->clrScr(); lcd->setBackColor(COL_BG);
  drawPanel(lcd, 0, 0, SCR_W, 70, COL_NAVY);
  lcd->setColor(COL_WHITE); lcd->setBackColor(COL_NAVY);
  lcd->setFont(BigFont); lcdPrint(lcd, "Book this room", 20, 20);
  lcd->setBackColor(COL_BG); lcd->setFont(SmallFont); lcd->setColor(COL_GRAY);
  lcdPrint(lcd, "Select duration:", 20, 100);
  int bx[3]  = {40, 280, 520};
  char bl[3][12] = {"15 minutes", "30 minutes", "60 minutes"};
  for (int i = 0; i < 3; i++) {
    drawPanel(lcd, bx[i], 130, 200, 100, 0, 80, 160);
    lcd->setBackColor(0, 80, 160); lcd->setColor(COL_WHITE); lcd->setFont(BigFont);
    lcd->print(bl[i], bx[i] + 10, 168);
  }
  lcd->setBackColor(COL_BG); lcd->setFont(SmallFont); lcd->setColor(COL_GRAY);
  lcdPrint(lcd, "Tap duration to confirm. Syncs to cloud.", 20, 380);
  drawPanel(lcd, 320, 420, 160, 44, 80, 20, 20);
  lcd->setBackColor(80, 20, 20); lcd->setColor(COL_WHITE); lcd->setFont(SmallFont);
  lcdPrint(lcd, "Cancel", 365, 435);
  lcd->setBackColor(COL_BG);
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
