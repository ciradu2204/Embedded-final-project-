#include "mega_comm.h"
#include "config.h"

#define MegaSerial Serial2

static MegaTouchEvent _lastEvent     = {false, "", 0, 0, 0};
static char           _rxBuf[256];
static uint8_t        _rxPos         = 0;
static uint8_t        _remoteScreen  = 0;

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
        if (s >= 0) {
          _remoteScreen = (uint8_t)s;
          Serial.printf("[MegaComm] Screen=%u\n", _remoteScreen);
        }
      } else if (strcmp(evt, "TOUCH") == 0) {
        _lastEvent.valid = true;
        _lastEvent.bookDuration = 0;
        extractStr(_rxBuf, "gesture", _lastEvent.gesture, sizeof(_lastEvent.gesture));
        int x = extractInt(_rxBuf, "x");
        int y = extractInt(_rxBuf, "y");
        _lastEvent.x = (x >= 0) ? (uint16_t)x : 0;
        _lastEvent.y = (y >= 0) ? (uint16_t)y : 0;
        Serial.printf("[MegaComm] Touch: %s (%u,%u)\n",
                      _lastEvent.gesture, _lastEvent.x, _lastEvent.y);
      } else if (strcmp(evt, "BOOK") == 0) {
        int dur = extractInt(_rxBuf, "dur");
        if (dur > 0) {
          _lastEvent.valid = true;
          strlcpy(_lastEvent.gesture, "BOOK", sizeof(_lastEvent.gesture));
          _lastEvent.bookDuration = (uint16_t)dur;
          Serial.printf("[MegaComm] Walk-up: %u min\n", dur);
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

void megaSendStatus(const char* roomName, uint8_t state,
                    const char* occupantName,
                    const char* startTime, const char* endTime,
                    uint16_t countdownMins, uint32_t countdownSecs) {
  char buf[280];
  snprintf(buf, sizeof(buf),
           "{\"cmd\":\"STATUS\",\"room\":\"%s\",\"state\":%u,"
           "\"occ\":\"%s\",\"start\":\"%s\",\"end\":\"%s\","
           "\"mins\":%u,\"secs\":%lu}\n",
           roomName, state, occupantName,
           startTime, endTime, countdownMins, (unsigned long)countdownSecs);
  MegaSerial.print(buf);
}

// Send calendar booking data to Mega for display on calendar screen.
// Sends up to 'count' slots. Each slot has start/end Unix timestamps and name.
void megaSendCalendarData(BookingSlot* slots, uint8_t count) {
  // Send each active slot as a separate CALSLOT command to keep packet size small
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    if (!slots[i].active) continue;
    if (slots[i].state == STATE_COMPLETED || slots[i].state == STATE_GHOST) continue;
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"cmd\":\"CALSLOT\",\"s\":%lu,\"e\":%lu,\"n\":\"%s\",\"st\":%u}\n",
             (unsigned long)slots[i].startTime,
             (unsigned long)slots[i].endTime,
             slots[i].occupantName,
             (uint8_t)slots[i].state);
    MegaSerial.print(buf);
    delay(30);  // Small gap between slots to avoid overwhelming Mega's serial buffer
  }
  // Send CALDONE to signal end of data
  MegaSerial.print("{\"cmd\":\"CALDONE\"}\n");
}

void megaSendCalendar()   { MegaSerial.print("{\"cmd\":\"CALENDAR\"}\n"); }
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
