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
