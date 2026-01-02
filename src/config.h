// src/config.h
#pragma once
#include <Arduino.h>

// ★ここがポイント：配布ビルドでは config_private.h を読まない
#if !defined(MC_DISABLE_CONFIG_PRIVATE)
  #if __has_include("config_private.h")
    #include "config_private.h"
  #endif
#endif

#include "mc_config_store.h"

#ifndef MC_DISPLAY_SLEEP_SECONDS
#define MC_DISPLAY_SLEEP_SECONDS 60
#endif

#ifndef MC_TTS_ACTIVE_THREADS_DURING_TTS
#define MC_TTS_ACTIVE_THREADS_DURING_TTS 0
#endif

#ifndef MC_ATTENTION_TEXT
#define MC_ATTENTION_TEXT "Hi"
#endif

struct AppConfig {
  const char* wifi_ssid;
  const char* wifi_pass;

  const char* duco_user;
  const char* duco_miner_key;
  const char* duco_rig_name;
  const char* duco_banner;

  const char* az_speech_region;
  const char* az_speech_key;
  const char* az_tts_voice;

  const char* app_name;
  const char* app_version;

  const char* attention_text;
};

inline const AppConfig& appConfig() {
  static AppConfig cfg{
    mcCfgWifiSsid(),
    mcCfgWifiPass(),
    mcCfgDucoUser(),
    mcCfgDucoKey(),

    "Mining-Stackchan-Core2",
    "M5StackCore2",

    mcCfgAzRegion(),
    mcCfgAzKey(),
    mcCfgAzVoice(),

    "Mining-Stackchan-Core2",
    "0.64",

    MC_ATTENTION_TEXT
  };
  // 実行中にSETされた場合にも反映されるよう、毎回上書き（ポインタ差し替えのみ）
  cfg.wifi_ssid = mcCfgWifiSsid();
  cfg.wifi_pass = mcCfgWifiPass();
  cfg.duco_user = mcCfgDucoUser();
  cfg.duco_miner_key = mcCfgDucoKey();
  cfg.az_speech_region = mcCfgAzRegion();
  cfg.az_speech_key    = mcCfgAzKey();
  cfg.az_tts_voice     = mcCfgAzVoice();
  return cfg;
}
