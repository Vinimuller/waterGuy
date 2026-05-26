#include "events.h"

namespace {
  const int CAPACITY = 8;
  IrrigationEvent buffer[CAPACITY];
  int head = 0;
  int tail = 0;
  int count = 0;
}

namespace events {

void emit(const IrrigationEvent& e) {
  buffer[head] = e;
  head = (head + 1) % CAPACITY;
  if (count < CAPACITY) {
    count++;
  } else {
    // buffer full — drop oldest
    tail = (tail + 1) % CAPACITY;
  }
}

bool pop(IrrigationEvent& out) {
  if (count == 0) return false;
  out = buffer[tail];
  tail = (tail + 1) % CAPACITY;
  count--;
  return true;
}

}
