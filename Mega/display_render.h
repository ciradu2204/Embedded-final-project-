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

struct CalendarSlot {
  uint32_t startSecs;   // Unix timestamp
  uint32_t endSecs;
  char     name[24];
  uint8_t  state;
  bool     active;
};

struct RoomDisplayData {
  char     roomName[32];
  char     occupantName[32];
  char     startTime[12];
  char     endTime[12];
  uint8_t  state;
  uint16_t countdownMins;
  uint32_t countdownSecs;
};

void displayStartup(UTFT* lcd);
void displayStatusScreen(UTFT* lcd, RoomDisplayData* d);
void displayCalendarScreen(UTFT* lcd);
void displayCalendarBookings(UTFT* lcd, CalendarSlot* slots, uint8_t count);
void displayBookNowScreen(UTFT* lcd);
void displayConfirmation(UTFT* lcd, bool success);
void displayOfflineWarning(UTFT* lcd, bool show);

uint16_t    stateColour(uint8_t state);
const char* stateLabel(uint8_t state);

#endif

// Called when returning from sub-screen to force full status redraw
void resetStatusScreenCache();
