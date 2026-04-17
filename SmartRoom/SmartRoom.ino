/*
 * SmartRoom – ESP32 main sketch
 */

#include "config.h"
#include "booking.h"
#include "fsm.h"
#include "event_queue.h"
#include "pir_sensor.h"
#include "led.h"
#include "buzzer.h"
#include "nvs_manager.h"
#include "mqtt_handler.h"
#include "mega_comm.h"
#include <Arduino.h>
#include <time.h>

static unsigned long _lastFsmTickMs     = 0;
static unsigned long _lastDisplaySyncMs = 0;
static unsigned long _lastNvsSaveMs     = 0;
static unsigned long _lastOfflineBlipMs = 0;
// FIX: Non-blocking confirmation timer replaces blocking delay(3200)
static unsigned long _confirmClearMs    = 0;
static bool          _confirmPending    = false;
// FIX (#3): Non-blocking startup handshake replaces blocking delay(1500).
static unsigned long _startupTimeMs     = 0;
static bool          _startupDone       = false;
static const uint32_t STARTUP_DELAY_MS  = 1500;
// Set when the L gesture switches to the calendar screen so the next
// hash-refresh tick sends CALSLOT/CALDONE unconditionally (otherwise the
// slot fingerprint may be unchanged and nothing would fire).
static bool          _calNeedFullSend   = false;
// Fingerprint of the slot table last sent to the Mega. The 1Hz hash-refresh
// only resends when this changes. The L-handler updates this after its
// initial send so the refresh tick doesn't immediately CALRESET + resend
// identical data (which caused a visible wipe on the first swipe).
static uint32_t      _lastCalHash       = 0;

static uint32_t computeCalHash() {
  uint32_t h = 2166136261u;
  BookingSlot* s = fsmGetSlots();
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    if (!s[i].active) continue;
    h ^= (uint32_t)s[i].startTime; h *= 16777619u;
    h ^= (uint32_t)s[i].endTime;   h *= 16777619u;
    h ^= (uint32_t)s[i].state;     h *= 16777619u;
  }
  return h;
}

static const uint32_t FSM_TICK_INTERVAL_MS     = 1000;
static const uint32_t DISPLAY_SYNC_INTERVAL_MS = 1000;
static const uint32_t NVS_SAVE_INTERVAL_MS     = 30000;
static const uint32_t CONFIRM_SHOW_MS          = 3000;
static const uint32_t OFFLINE_BLIP_INTERVAL_MS  = 2000;  // Blip every 2s when offline

static const char WALK_UP_NAME[] = "Walk-up booking";

static void onBookingMessage(const char* payload);
static void updateLED();
static void syncDisplay();
static void formatTime(time_t t, char* buf, uint8_t bufLen);
static uint32_t fsmCountdownSecs();

// ════════════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n=== SmartRoom ESP32 ==="));

  megaCommInit();

  // FIX (#3): No blocking delay here. The Mega needs ~1.5s to finish
  // touchInit() + InitLCD(), so we send STARTUP from loop() once that
  // window has elapsed. The rest of setup() runs in parallel.
  _startupTimeMs = millis();
  _startupDone   = false;

  eventQueueInit();
  nvsInit();
  pirInit();
  ledInit();
  buzzerInit();

  fsmInit();
  // NVS restore deferred to loop() so it runs AFTER NTP sync — otherwise
  // the past-booking guard in fsmAddBooking skips its "end < now" check
  // (nowKigali is near zero) and stale weeks-old slots from flash get
  // resurrected as SCHEDULED, making the LCD wrongly show RESERVED.

  mqttSetBookingCallback(onBookingMessage);
  mqttInit();

  updateLED();
  syncDisplay();

  buzzerShortBeep();
  Serial.println(F("[Setup] Complete."));
}

// ════════════════════════════════════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  mqttLoop();
  ledTick();
  buzzerTick();
  megaCommTick();

  // Calendar resend path — fires when the Mega's CALDONE handler spotted
  // a count mismatch (a CALSLOT was dropped on the UART). Resend the full
  // slot table and clear the cached hash so the 1Hz refresh doesn't skip
  // the next due tick thinking the data is unchanged.
  if (megaTakeCalendarRetry() && megaGetRemoteScreen() == 1) {
    Serial.println(F("[App] Calendar retry — resending slot table."));
    megaSendCalReset();
    delay(30);
    megaSendCalendarData(fsmGetSlots(), MAX_SLOTS);
    _lastCalHash = computeCalHash();
  }

  // FIX (#3): Non-blocking startup handshake — fires once, ~1.5s after boot.
  if (!_startupDone && now - _startupTimeMs >= STARTUP_DELAY_MS) {
    megaSendStartup();
    _startupDone = true;
  }

  // Deferred NVS restore: runs exactly once, after NTP has synced, so the
  // past-booking rejection in fsmAddBooking can actually evaluate end_time.
  static bool _nvsRestored = false;
  if (!_nvsRestored && time(nullptr) > 1000000000L) {
    _nvsRestored = true;
    static BookingSlot saved[MAX_SLOTS];
    uint8_t count = nvsLoadBookings(saved, MAX_SLOTS);
    for (uint8_t i = 0; i < count; i++) fsmAddBooking(saved[i]);
    updateLED();
  }

  // FIX: Non-blocking confirmation clear.
  // After booking confirmed, wait CONFIRM_SHOW_MS then return to status screen.
  // This replaces the old blocking delay(3200) that froze the ESP32.
  if (_confirmPending && now - _confirmClearMs >= CONFIRM_SHOW_MS) {
    _confirmPending = false;
    megaSetRemoteScreen(0);
    syncDisplay();
  }

  MegaTouchEvent touch = megaGetTouchEvent();
  if (touch.valid && !_confirmPending) {
    Serial.printf("[Touch] %s\n", touch.gesture);

    if (strcmp(touch.gesture, "BOOK") == 0 && touch.bookDuration > 0) {
      // FIX (#2, #6): central availability check + UX feedback when refused.
      const char* purpose = touch.bookPurpose[0] ? touch.bookPurpose : "Walk-up";
      bool ok = fsmCreateWalkUpBooking(WALK_UP_NAME, touch.bookDuration, purpose);
      if (ok) {
        updateLED();
        megaSendConfirm(true);
      } else {
        megaSendConfirm(false);
        megaSendMessage("Room unavailable. Swipe to view calendar");
      }
      // FIX: Start non-blocking timer instead of delay(3200)
      _confirmPending  = true;
      _confirmClearMs  = now;

    } else if (strcmp(touch.gesture, "L") == 0) {
      // The Mega has already switched to the calendar screen locally on
      // swipe-left (cleared calSlotCount and drew the empty grid), so the
      // ESP32 just needs to stream the current slot table. Arriving
      // CALSLOT bytes land in an idle Mega ring without competing for
      // cycles against clrScr / grid redraw.
      megaSetRemoteScreen(1);
      megaSendCalendarData(fsmGetSlots(), MAX_SLOTS);
      _lastDisplaySyncMs = now;    // suppress next 1Hz tick
      _calNeedFullSend   = false;
      _lastCalHash       = computeCalHash();  // skip redundant refresh

    } else if (strcmp(touch.gesture, "R") == 0
            || strcmp(touch.gesture, "CANCEL") == 0) {
      megaSetRemoteScreen(0);
      syncDisplay();
    }
  }

  // FSM tick every second
  if (now - _lastFsmTickMs >= FSM_TICK_INTERVAL_MS) {
    _lastFsmTickMs = now;
    fsmTick(pirPresent());
    updateLED();
    if (!eventQueueEmpty() && mqttConnected()) {
      FsmEvent evt;
      while (eventQueuePop(evt)) mqttPublishStatus(evt);
    }
  }

  // Display sync — suppressed on sub-screens and during confirmation overlay
  if (now - _lastDisplaySyncMs >= DISPLAY_SYNC_INTERVAL_MS) {
    _lastDisplaySyncMs = now;
    uint8_t remote = megaGetRemoteScreen();
    if (remote == 0 && !_confirmPending) {
      syncDisplay();
      bool offline = !mqttConnected();
      megaSendOfflineWarning(offline);
      // Throttled offline blip — one quiet 40ms blip every 2 seconds when disconnected.
      // Much less annoying than the previous continuous tone caused by the blocking
      // WiFi reconnect loop preventing buzzerTick() from running.
      if (offline && now - _lastOfflineBlipMs >= OFFLINE_BLIP_INTERVAL_MS) {
        _lastOfflineBlipMs = now;
        buzzerOfflineBlip();
      }
    } else if (remote == 1) {
      // Calendar screen open: refresh booking data so newly arrived snapshots
      // appear without the user having to leave and re-open the calendar.
      // Only resend when the slot fingerprint actually changes — avoids the
      // grid flicker that a per-second redraw would cause.
      uint32_t h = computeCalHash();
      if (_calNeedFullSend || h != _lastCalHash) {
        _calNeedFullSend = false;
        _lastCalHash = h;
        // Lightweight slot-buffer reset (no LCD redraw) followed by the
        // fresh set. The grid is already on screen, so we don't need the
        // heavy clrScr path here — that was competing with the CALSLOT
        // stream for Mega cycles and dropping bytes.
        megaSendCalReset();
        delay(30);
        megaSendCalendarData(fsmGetSlots(), MAX_SLOTS);
      }
    }
  }

  if (now - _lastNvsSaveMs >= NVS_SAVE_INTERVAL_MS) {
    _lastNvsSaveMs = now;
    nvsSaveBookings(fsmGetSlots(), MAX_SLOTS);
  }
}

// ════════════════════════════════════════════════════════════════════════════
static void onBookingMessage(const char* payload) {
  Serial.printf("[App] Booking: %s\n", payload);

  auto extractStr = [](const char* json, const char* key,
                       char* out, uint8_t len) -> bool {
    char search[48];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char* p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    uint8_t i = 0;
    while (*p && *p != '"' && i < len - 1) out[i++] = *p++;
    out[i] = '\0';
    return true;
  };

  auto extractLong = [](const char* json, const char* key) -> long {
    char search[48];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char* p = strstr(json, search);
    if (!p) return -1;
    return atol(p + strlen(search));
  };

  auto isoToUnix = [](const char* s) -> time_t {
    struct tm t = {0};
    if (sscanf(s, "%d-%d-%dT%d:%d:%d",
               &t.tm_year, &t.tm_mon, &t.tm_mday,
               &t.tm_hour, &t.tm_min, &t.tm_sec) >= 3) {
      t.tm_year -= 1900; t.tm_mon -= 1;
      return mktime(&t);
    }
    return 0;
  };

  char statusVal[16] = {0};
  extractStr(payload, "status", statusVal, sizeof(statusVal));

  // Accept both "scheduled" and "active" — the snapshot topic carries active
  // bookings too (a session already in progress when the device booted).
  if (strcmp(statusVal, "scheduled") == 0 || strcmp(statusVal, "active") == 0) {
    BookingSlot slot;
    memset(&slot, 0, sizeof(slot));
    extractStr(payload, "bookingId",  slot.bookingId,    sizeof(slot.bookingId));
    extractStr(payload, "userName",   slot.occupantName, sizeof(slot.occupantName));
    extractStr(payload, "title",      slot.title,        sizeof(slot.title));
    long rawStart = extractLong(payload, "startTime");
    if (rawStart > 1000000) {
      slot.startTime = (time_t)rawStart;
      slot.endTime   = (time_t)extractLong(payload, "endTime");
    } else {
      char startT[32] = {0}, endT[32] = {0};
      extractStr(payload, "startTime", startT, sizeof(startT));
      extractStr(payload, "endTime",   endT,   sizeof(endT));
      slot.startTime = isoToUnix(startT);
      slot.endTime   = isoToUnix(endT);
    }
    slot.active = true;
    slot.state  = STATE_SCHEDULED;
    Serial.printf("[App] Slot: id=%s user=%s\n", slot.bookingId, slot.occupantName);
    fsmAddBooking(slot);
    updateLED();
    // FIX: Do NOT call syncDisplay() here. On (re)connect the broker delivers
    // every retained/queued booking back-to-back; sync-per-message caused the
    // visible "refresh storm" on the Mega LCD. The 1 Hz loop sync below
    // already redraws on the next tick.
    nvsSaveBookings(fsmGetSlots(), MAX_SLOTS);

  } else if (strcmp(statusVal, "cancelled") == 0) {
    char bId[40] = {0};
    extractStr(payload, "bookingId", bId, sizeof(bId));
    fsmCancelBooking(bId);
    updateLED();
    // FIX: as above, defer redraw to the 1 Hz periodic syncDisplay().
  } else {
    Serial.printf("[App] Unknown status: '%s'\n", statusVal);
  }
}

static void updateLED() {
  switch (fsmGetCurrentState()) {
    case STATE_SCHEDULED:
    case STATE_GHOST:
    case STATE_COMPLETED: ledSet(LED_GREEN);     break;
    case STATE_PENDING:   ledSet(LED_BLINK_RED); break;
    case STATE_ACTIVE:    ledSet(LED_RED);       break;
  }
}

static uint32_t fsmCountdownSecs() {
  BookingSlot* s = fsmGetActiveSlot();
  if (!s) return 0;
  time_t now = time(nullptr) + 7200; // 2 hr offset for Kigali
  if (now >= s->endTime) return 0;
  return (uint32_t)(s->endTime - now);
}

static void syncDisplay() {
  FSMState     state = fsmGetCurrentState();
  BookingSlot* slot  = fsmGetActiveSlot();
  char     startStr[12] = "--:--", endStr[12] = "--:--", occupant[32] = "", title[40] = "";
  uint16_t mins = 0;
  uint32_t secs = 0;
  if (slot) {
    formatTime(slot->startTime, startStr, sizeof(startStr));
    formatTime(slot->endTime,   endStr,   sizeof(endStr));
    strlcpy(occupant, slot->occupantName, sizeof(occupant));
    strlcpy(title,    slot->title,        sizeof(title));
    secs = fsmCountdownSecs();
    mins = (uint16_t)(secs / 60);
  }
  // "Up next" panel on the status screen — populated from the earliest
  // future SCHEDULED slot regardless of whether the room is available.
  char upOcc[32] = "", upTitle[32] = "", upStart[12] = "",
       upEnd[12] = "", upDate[24] = "";
  BookingSlot* up = fsmGetUpcomingSlot();
  if (up) {
    strlcpy(upOcc,   up->occupantName, sizeof(upOcc));
    strlcpy(upTitle, up->title,        sizeof(upTitle));
    formatTime(up->startTime, upStart, sizeof(upStart));
    formatTime(up->endTime,   upEnd,   sizeof(upEnd));
    // "Mon 21 Apr" — date label for the upcoming booking.
    struct tm* tmU = localtime(&up->startTime);
    if (tmU) {
      static const char* DOW[7]    = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
      static const char* MON_ABR[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                       "Jul","Aug","Sep","Oct","Nov","Dec"};
      snprintf(upDate, sizeof(upDate), "%s %d %s",
               DOW[tmU->tm_wday], tmU->tm_mday, MON_ABR[tmU->tm_mon]);
    }
  }
  megaSendStatus(ROOM_NAME, (uint8_t)state, occupant, title, startStr, endStr,
                 mins, secs, upOcc, upTitle, upStart, upEnd, upDate);
}

static void formatTime(time_t t, char* buf, uint8_t bufLen) {
  struct tm* tmInfo = localtime(&t);
  if (!tmInfo) { strlcpy(buf, "--:--", bufLen); return; }
  snprintf(buf, bufLen, "%02d:%02d", tmInfo->tm_hour, tmInfo->tm_min);
}
