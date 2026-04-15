#ifndef DISPLAY_RENDER_H
#define DISPLAY_RENDER_H

#include <Arduino.h>
#include <UTFT.h>

#define STATE_SCHEDULED   0
#define STATE_PENDING     1
#define STATE_ACTIVE      2
#define STATE_GHOST       3
#define STATE_COMPLETED   4

// Max calendar slots to display
#define MAX_CAL_SLOTS  10

// ── Calendar viewport ──────────────────────────────────────────────────────────
// Day spans 07:00 – 22:00 (15 hours). 8 hour rows are visible at a time and the
// view scrolls vertically with up/down swipes (or the on-screen arrows).
#define CAL_DAY_START_HOUR  7
#define CAL_DAY_END_HOUR    22
#define CAL_VISIBLE_ROWS    8

struct CalendarSlot {
  uint32_t startSecs;   // Unix timestamp
  uint32_t endSecs;
  char     name[24];
  char     title[24];   // booking purpose
  uint8_t  state;
  bool     active;
};

struct RoomDisplayData {
  char     roomName[32];
  char     occupantName[32];
  char     title[32];
  char     startTime[12];
  char     endTime[12];
  uint8_t  state;
  uint16_t countdownMins;
  uint32_t countdownSecs;
};

void displayStartup(UTFT* lcd);
void displayStatusScreen(UTFT* lcd, RoomDisplayData* d);

// Calendar grid: drawn once when the user opens the calendar screen.
// `topHour` is the first hour shown at the top of the visible window.
void displayCalendarScreen(UTFT* lcd, uint8_t topHour);
void displayCalendarBookings(UTFT* lcd, CalendarSlot* slots, uint8_t count, uint8_t topHour);

void displayBookNowScreen(UTFT* lcd);
void displayConfirmation(UTFT* lcd, bool success);
void displayOfflineWarning(UTFT* lcd, bool show);

// FIX (#6): toast-style transient message overlay (used for walk-up rejection).
void displayMessage(UTFT* lcd, const char* text);

// Book Now combined-picker state machine. The touch router calls the select
// helpers when the user taps a chip; displayBookNowScreen() resets state.
void bookNowSelectDuration(UTFT* lcd, int8_t idx);
void bookNowSelectPurpose(UTFT* lcd, int8_t idx);
void bookNowRefreshConfirm(UTFT* lcd);
int         bookNowGetDurationMins();
const char* bookNowGetPurpose();
bool        bookNowIsReady();

// Admin PIN screen. pinAppendDigit adds one char; pinClear resets;
// pinGetBuffer returns the current entry for comparison.
void        displayPinScreen(UTFT* lcd);
void        pinAppendDigit(UTFT* lcd, char d);
void        pinClear(UTFT* lcd);
const char* pinGetBuffer();

uint16_t    stateColour(uint8_t state);
const char* stateLabel(uint8_t state);

#endif

// Called when returning from sub-screen to force full status redraw
void resetStatusScreenCache();
