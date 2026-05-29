#ifndef EVENTS_H
#define EVENTS_H

#include <Arduino.h>

enum class IrrigationEventType {
  PinChanged,
  ConfigSaved,
  ConfigError,
  ClockSynced,
  ProgramStarted,
  ProgramEnded,
  StartSkipped,
};

struct IrrigationEvent {
  IrrigationEventType type;
  int    pin;
  int    val;
  long   minute;
  String detail;
};

namespace events {
  void emit(const IrrigationEvent& e);
  bool pop(IrrigationEvent& out);
}

#endif
