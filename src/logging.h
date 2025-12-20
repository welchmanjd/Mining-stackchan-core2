// src/logging.h
#pragma once

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

// シンプルな printf 互換ログ出力（ヘッダ内インライン実装）
inline void mc_logf(const char* fmt, ...) {
  char buf[256];

  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  Serial.println(buf);
}

// ===== Event logging helpers =====
// Toggle DEBUG output by defining EVT_DEBUG_ENABLED (1/0) before including this header.
#ifndef EVT_DEBUG_ENABLED
#define EVT_DEBUG_ENABLED 1
#endif

#define LOG_EVT_INFO(tag, fmt, ...) \
  mc_logf("[EVT] " tag " " fmt, ##__VA_ARGS__)

#define LOG_EVT_DEBUG(tag, fmt, ...) \
  do { if (EVT_DEBUG_ENABLED) mc_logf("[EVT] " tag " " fmt, ##__VA_ARGS__); } while (0)
