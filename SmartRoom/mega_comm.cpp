#include "mega_comm.h"
#include "config.h"
#include <time.h>

#define MegaSerial Serial2

static MegaTouchEvent _lastEvent     = {false, "", 0, 0, 0, ""};
static char           _rxBuf[256];
static uint8_t        _rxPos         = 0;
static uint8_t        _remoteScreen  = 0;
// Set by megaCommTick when the Mega requests a calendar resend (CALRETRY).
// Cleared by megaTakeCalendarRetry() once the main loop has acted on it.
static bool           _calRetryPending = false;

static bool extractStr(const char* json, const char* key, char* out, uint8_t outLen) {
  char search[32];
  snprintf(search, sizeof(search), "\"%s\":\"", key);
  const char* p = strstr(json, search);
  if (!p) return false;
  p += strlen(search);
  uint8_t i = 0;
  while (*p && *p != '"' && i < outLen - 1) out[i++] = *p++;
  out[i] = '\0';
  return true;
}

static int extractInt(const char* json, const char* key) {
  char search[32];
  snprintf(search, sizeof(search), "\"%s\":", key);
  const char* p = strstr(json, search);
  if (!p) return -1;
  return atoi(p + strlen(search));
}

void megaCommInit() {
  MegaSerial.begin(UART_MEGA_BAUD, SERIAL_8N1, UART_MEGA_RX, UART_MEGA_TX);
  Serial.println(F("[MegaComm] UART2 open. RX=GPIO16, TX=GPIO17."));
}

void megaCommTick() {
  while (MegaSerial.available()) {
    char c = MegaSerial.read();
    if (c == '\n') {
      _rxBuf[_rxPos] = '\0';
      _rxPos = 0;
      char evt[16] = {0};
      extractStr(_rxBuf, "evt", evt, sizeof(evt));

      if (strcmp(evt, "SCREEN") == 0) {
        int s = extractInt(_rxBuf, "s");
        if (s >= 0) _remoteScreen = (uint8_t)s;
      } else if (strcmp(evt, "TOUCH") == 0) {
        _lastEvent.valid = true;
        _lastEvent.bookDuration = 0;
        extractStr(_rxBuf, "gesture", _lastEvent.gesture, sizeof(_lastEvent.gesture));
        int x = extractInt(_rxBuf, "x");
        int y = extractInt(_rxBuf, "y");
        _lastEvent.x = (x >= 0) ? (uint16_t)x : 0;
        _lastEvent.y = (y >= 0) ? (uint16_t)y : 0;
      } else if (strcmp(evt, "CALRETRY") == 0) {
        _calRetryPending = true;
      } else if (strcmp(evt, "BOOK") == 0) {
        int dur = extractInt(_rxBuf, "dur");
        if (dur > 0) {
          _lastEvent.valid = true;
          strlcpy(_lastEvent.gesture, "BOOK", sizeof(_lastEvent.gesture));
          _lastEvent.bookDuration = (uint16_t)dur;
          _lastEvent.bookPurpose[0] = '\0';
          extractStr(_rxBuf, "purpose", _lastEvent.bookPurpose, sizeof(_lastEvent.bookPurpose));
        }
      }
    } else {
      if (_rxPos < sizeof(_rxBuf) - 1) _rxBuf[_rxPos++] = c;
    }
  }
}

MegaTouchEvent megaGetTouchEvent() {
  MegaTouchEvent copy = _lastEvent;
  _lastEvent.valid = false;
  return copy;
}

void megaSetRemoteScreen(uint8_t screen) { _remoteScreen = screen; }
uint8_t megaGetRemoteScreen()            { return _remoteScreen; }

bool megaTakeCalendarRetry() {
  bool p = _calRetryPending;
  _calRetryPending = false;
  return p;
}

void megaSendStatus(const char* roomName, uint8_t state,
                    const char* occupantName, const char* title,
                    const char* startTime, const char* endTime,
                    uint16_t countdownMins, uint32_t countdownSecs,
                    const char* upcomingOccupant,
                    const char* upcomingTitle,
                    const char* upcomingStart,
                    const char* upcomingEnd,
                    const char* upcomingDate) {
  char buf[560];
  snprintf(buf, sizeof(buf),
           "{\"cmd\":\"STATUS\",\"room\":\"%s\",\"state\":%u,"
           "\"occ\":\"%s\",\"title\":\"%s\","
           "\"start\":\"%s\",\"end\":\"%s\","
           "\"mins\":%u,\"secs\":%lu,"
           "\"uOcc\":\"%s\",\"uTitle\":\"%s\","
           "\"uStart\":\"%s\",\"uEnd\":\"%s\",\"uDate\":\"%s\"}\n",
           roomName, state, occupantName, title ? title : "",
           startTime, endTime, countdownMins, (unsigned long)countdownSecs,
           upcomingOccupant ? upcomingOccupant : "",
           upcomingTitle    ? upcomingTitle    : "",
           upcomingStart    ? upcomingStart    : "",
           upcomingEnd      ? upcomingEnd      : "",
           upcomingDate     ? upcomingDate     : "");
  MegaSerial.print(buf);
}

// Send calendar booking data to Mega for display on calendar screen.
// Sends up to 'count' slots. Each slot has start/end Unix timestamps and name.
void megaSendCalendarData(BookingSlot* slots, uint8_t count) {
  // Mega's default HardwareSerial RX ring is 64 bytes and a CALSLOT line
  // is ~90-130 bytes, so we split each CALSLOT at byte 48 with a pause in
  // between. No flush() — ESP32 HardwareSerial.flush() can block for a
  // long time under contention, and the delays here are already long
  // enough that any reasonable hardware will finish the transmit.
  uint8_t sent = 0;
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    if (!slots[i].active) continue;
    if (slots[i].state == STATE_COMPLETED || slots[i].state == STATE_GHOST) continue;
    sent++;
    char buf[200];
    // slots[i].startTime / endTime are already Kigali wall-clock epoch
    // (the backend shifted by KIGALI_OFFSET_SECONDS before publishing).
    // Send them as-is — localtime() on the Mega will extract the correct
    // local hour directly. CALDONE's `now` is derived from time(nullptr),
    // which is real UTC, so IT needs the +7200 shift to match this
    // convention. See mega_protocol.cpp:CALDONE handler for the filter
    // logic that depends on both values being in the same epoch.
    int len = snprintf(buf, sizeof(buf),
             "{\"cmd\":\"CALSLOT\",\"s\":%lu,\"e\":%lu,\"n\":\"%s\",\"t\":\"%s\",\"st\":%u}\n",
             (unsigned long)slots[i].startTime,
             (unsigned long)slots[i].endTime,
             slots[i].occupantName,
             slots[i].title,
             (uint8_t)slots[i].state);
    if (len <= 0) continue;
    // Split long CALSLOTs at 40 bytes with a 70 ms gap, giving the Mega's
    // 64-byte RX ring a comfortable margin to drain between halves. A few
    // extra ms buys reliability for the slot that was getting silently
    // dropped when the stream happened to collide with any LCD work.
    if (len > 40) {
      MegaSerial.write((const uint8_t*)buf, 40);
      delay(70);
      MegaSerial.write((const uint8_t*)(buf + 40), len - 40);
    } else {
      MegaSerial.write((const uint8_t*)buf, len);
    }
    delay(160);   // was 120 — widen so back-to-back slots don't overlap
  }
  // Extra settle before CALDONE so the final slot has time to fully
  // parse before the "draw now" trigger arrives.
  delay(100);
  // Include the expected slot count so the Mega can detect drops on the
  // UART and request a resend via a CALRETRY event. See the CALDONE handler
  // in mega_protocol.cpp and the CALRETRY path in onMegaTouchEvent.
  char doneBuf[80];
  snprintf(doneBuf, sizeof(doneBuf),
           "{\"cmd\":\"CALDONE\",\"now\":%lu,\"n\":%u}\n",
           (unsigned long)(time(nullptr) + 7200), (unsigned)sent);
  MegaSerial.print(doneBuf);
}

void megaSendCalendar()   { MegaSerial.print("{\"cmd\":\"CALENDAR\"}\n"); }
void megaSendCalReset()   { MegaSerial.print("{\"cmd\":\"CALRESET\"}\n"); }
void megaSendBookNow()    { MegaSerial.print("{\"cmd\":\"BOOKNOW\"}\n"); }
void megaSendStartup()    { MegaSerial.print("{\"cmd\":\"STARTUP\"}\n"); }

void megaSendConfirm(bool success) {
  char buf[40];
  snprintf(buf, sizeof(buf), "{\"cmd\":\"CONFIRM\",\"ok\":%d}\n", success ? 1 : 0);
  MegaSerial.print(buf);
}

void megaSendOfflineWarning(bool show) {
  char buf[40];
  snprintf(buf, sizeof(buf), "{\"cmd\":\"OFFLINE\",\"show\":%d}\n", show ? 1 : 0);
  MegaSerial.print(buf);
}

// FIX (#6): Push a short text message to the Mega. Used for walk-up booking
// rejection feedback so the user understands why nothing happened.
void megaSendMessage(const char* text) {
  char escaped[96];
  uint8_t j = 0;
  for (uint8_t i = 0; text[i] && j < sizeof(escaped) - 2; i++) {
    if (text[i] == '"' || text[i] == '\\') escaped[j++] = '\\';
    escaped[j++] = text[i];
  }
  escaped[j] = '\0';
  char buf[160];
  snprintf(buf, sizeof(buf), "{\"cmd\":\"MSG\",\"text\":\"%s\"}\n", escaped);
  MegaSerial.print(buf);
}
