#pragma once
#include <Arduino.h>

struct RuntimeFeatures {
  bool wifiConfigured = false;
  bool miningEnabled  = false;
  bool ttsEnabled     = false;
};

RuntimeFeatures getRuntimeFeatures();
