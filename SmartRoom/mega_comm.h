#ifndef MEGA_COMM_H
#define MEGA_COMM_H

#include <Arduino.h>
#include "booking.h"

struct MegaTouchEvent {
  bool     valid;
  char     gesture[16];
  uint16_t x;
  uint16_t y;
  uint16_t bookDuration;
};

void megaCommInit();
void megaCommTick();

void megaSendStatus(const char* roomName, uint8_t state,
                    const char* occupantName,
                    const char* startTime, const char* endTime,
                    uint16_t countdownMins, uint32_t countdownSecs);

void megaSendCalendarData(BookingSlot* slots, uint8_t count);
void megaSendCalendar();
void megaSendBookNow();
void megaSendConfirm(bool success);
void megaSendOfflineWarning(bool show);
void megaSendStartup();

// FIX (#6): Send a free-form text message to the Mega for transient feedback
// (e.g. walk-up booking rejection).
void megaSendMessage(const char* text);

void    megaSetRemoteScreen(uint8_t screen);
uint8_t megaGetRemoteScreen();

MegaTouchEvent megaGetTouchEvent();

#endif
