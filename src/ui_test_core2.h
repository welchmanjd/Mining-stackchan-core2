#pragma once
#include <Arduino.h>
#include "app_test_mode.h"

class UITestCore2 {
public:
  void begin();
  void markDirty();
  void draw(uint32_t nowMs, const TestState& st);

private:
  bool dirty_ = true;
  uint32_t lastDrawMs_ = 0;
};
