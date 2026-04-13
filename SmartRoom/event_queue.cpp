#include "event_queue.h"
#include <Arduino.h>

static FsmEvent _buf[EVENT_BUFFER_SIZE];
static uint8_t  _head = 0;
static uint8_t  _tail = 0;
static uint8_t  _count = 0;

void eventQueueInit() {
  _head = _tail = _count = 0;
}

bool eventQueuePush(const FsmEvent& evt) {
  if (_count >= EVENT_BUFFER_SIZE) {
    Serial.println(F("[Queue] Buffer full, dropping oldest event."));
    _tail = (_tail + 1) % EVENT_BUFFER_SIZE;
    _count--;
  }
  _buf[_head] = evt;
  _head = (_head + 1) % EVENT_BUFFER_SIZE;
  _count++;
  return true;
}

bool eventQueuePop(FsmEvent& out) {
  if (_count == 0) return false;
  out = _buf[_tail];
  _tail = (_tail + 1) % EVENT_BUFFER_SIZE;
  _count--;
  return true;
}

bool eventQueueEmpty() { return _count == 0; }
uint8_t eventQueueSize() { return _count; }
