// src/config.h
#pragma once
#include <Arduino.h>
#include "config_private.h"   // ← 秘密情報はここから
// 無操作スリープ秒数（config_private で未定義なら 60 秒）
#ifndef MC_DISPLAY_SLEEP_SECONDS
#define MC_DISPLAY_SLEEP_SECONDS 60
#endif

struct AppConfig {
  const char* wifi_ssid;
  const char* wifi_pass;
  const char* duco_user;
  const char* duco_miner_key;
  const char* duco_rig_name;
  const char* duco_banner;
  const char* app_name;
  const char* app_version;
};

inline const AppConfig& appConfig() {
  static const AppConfig cfg{
    MC_WIFI_SSID,
    MC_WIFI_PASS,
    MC_DUCO_USER,
    MC_DUCO_MINER_KEY,
    "Mining-Stackchan-Core2",   // DUCO_RIG_NAME
    "M5StackCore2",             // DUCO_BANNER
    "Mining-Stackchan Core2",   // APP_NAME
    "0.33"                      // APP_VERSION
  };
  return cfg;
}
