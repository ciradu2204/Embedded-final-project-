#include "nvs_manager.h"
#include "config.h"
#include <Preferences.h>

static Preferences _prefs;

void nvsInit() {
  _prefs.begin(NVS_NAMESPACE, false);
  Serial.println(F("[NVS] Initialised."));
}

// Serialise active slots to compact JSON and write to flash.
// Example: [{"id":"aaba834a-...","occ":"Alice","s":1714000000,"e":1714003600,"st":2}]
void nvsSaveBookings(BookingSlot* slots, uint8_t count) {
  // Must fit up to MAX_SLOTS * ~150 bytes per booking + brackets.
  // Static (BSS) to keep it off the loopTask stack — combined with other
  // large stack frames (nvsLoadBookings' internal String, various display
  // JSON buffers), a 4KB stack allocation here was tripping the canary.
  static char buf[4096];
  uint16_t pos = 0;

  buf[pos++] = '[';
  bool first = true;

  for (uint8_t i = 0; i < count; i++) {
    if (!slots[i].active) continue;
    if (!first) buf[pos++] = ',';
    first = false;

    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "{\"id\":\"%s\",\"occ\":\"%s\",\"tt\":\"%s\","
                    "\"s\":%lu,\"e\":%lu,\"st\":%u}",
                    slots[i].bookingId,
                    slots[i].occupantName,
                    slots[i].title,
                    (unsigned long)slots[i].startTime,
                    (unsigned long)slots[i].endTime,
                    (uint8_t)slots[i].state);

    if (pos >= sizeof(buf) - 2) {
      Serial.println(F("[NVS] Buffer full — truncating."));
      break;
    }
  }
  buf[pos++] = ']';
  buf[pos]   = '\0';

  _prefs.putString(NVS_KEY_BOOKINGS, buf);
}

// Parse JSON back into slots array. Returns number of slots loaded.
uint8_t nvsLoadBookings(BookingSlot* slots, uint8_t maxCount) {
  String stored = _prefs.getString(NVS_KEY_BOOKINGS, "[]");
  if (stored.length() < 3) {
    Serial.println(F("[NVS] No saved bookings."));
    return 0;
  }

  const char* p  = stored.c_str();
  uint8_t loaded = 0;

  while (*p && loaded < maxCount) {
    while (*p && *p != '{') p++;
    if (!*p) break;
    p++;

    BookingSlot s;
    memset(&s, 0, sizeof(s));
    s.active = true;

    while (*p && *p != '}') {
      while (*p && *p != '"' && *p != '}') p++;
      if (*p != '"') break;
      p++;

      char key[8] = {0};
      uint8_t ki = 0;
      while (*p && *p != '"' && ki < 7) key[ki++] = *p++;
      key[ki] = '\0';
      if (*p == '"') p++;
      if (*p == ':') p++;

      if (*p == '"') {
        p++;
        // FIX: val buffer is 40 chars to handle full UUIDs (36 chars + null)
        char val[40] = {0};
        uint8_t vi = 0;
        while (*p && *p != '"' && vi < 39) val[vi++] = *p++;
        val[vi] = '\0';
        if (*p == '"') p++;

        if (strcmp(key, "id")  == 0) strlcpy(s.bookingId,    val, sizeof(s.bookingId));
        if (strcmp(key, "occ") == 0) strlcpy(s.occupantName, val, sizeof(s.occupantName));
        if (strcmp(key, "tt")  == 0) strlcpy(s.title,        val, sizeof(s.title));
      } else {
        long num = atol(p);
        while (*p && *p != ',' && *p != '}') p++;
        if (strcmp(key, "s")  == 0) s.startTime = (time_t)num;
        if (strcmp(key, "e")  == 0) s.endTime   = (time_t)num;
        if (strcmp(key, "st") == 0) s.state     = (FSMState)num;
      }
      while (*p == ' ' || *p == ',') p++;
    }
    if (*p == '}') p++;

    // Compare against Kigali wall-clock — stored endTime is backend-shifted
    // by +7200, so the reference time must match. Refuse to restore anything
    // until the clock is synced: a past slot resurrected pre-NTP will show
    // as RESERVED on the LCD until the next snapshot prunes it.
    time_t nowKigali = time(nullptr) + 7200;
    if (nowKigali >= 1000000000L && s.endTime > nowKigali) {
      slots[loaded++] = s;
    }
    // Silently skip: clock not synced, or booking already expired.
  }
  Serial.printf("[NVS] Loaded %u slot(s).\n", loaded);
  return loaded;
}
