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
#define MC_DISPLAY_SLEEP_SECONDS 600
#endif

#ifndef MC_TTS_ACTIVE_THREADS_DURING_TTS
#define MC_TTS_ACTIVE_THREADS_DURING_TTS 0
#endif

#ifndef MC_ATTENTION_TEXT
#define MC_ATTENTION_TEXT "Hi"
#endif

// ---- CPU frequency (MHz) ----
// setCpuFrequencyMhz() が受け付ける代表値: 80 / 160 / 240
// config_private.h で #define MC_CPU_FREQ_MHZ 240 などにすると上書き可能
#ifndef MC_CPU_FREQ_MHZ
  #define MC_CPU_FREQ_MHZ 240
#endif

// ★命名を Web/JSON（index.html / mc_config_store）に合わせる
//   duco_miner_key / az_speech_region / az_speech_key / az_tts_voice など
struct AppConfig {
  const char* wifi_ssid;
  const char* wifi_pass;

  // duco (Web/JSON: duco_user / duco_miner_key)
  const char* duco_user;
  const char* duco_miner_key;

  // duco (固定値)
  const char* duco_rig_name;
  const char* duco_banner;

  // azure (Web/JSON: az_speech_region / az_speech_key / az_tts_voice)
  const char* az_speech_region;
  const char* az_speech_key;
  const char* az_tts_voice;

  // app
  const char* app_name;
  const char* app_version;

  // attention
  const char* attention_text;

  // speech lines (Web/JSON: share_accepted_text / hello_text)
  const char* share_accepted_text;
  const char* hello_text;
};

inline const AppConfig& appConfig() {
  static AppConfig cfg{
    // wifi
    mcCfgWifiSsid(),
    mcCfgWifiPass(),

    // duco
    mcCfgDucoUser(),
    mcCfgDucoKey(),
    "Mining-Stackchan-Core2", // duco_rig_name
    "M5StackCore2",           // duco_banner

    // azure tts
    mcCfgAzRegion(),
    mcCfgAzKey(),
    mcCfgAzVoice(),

    // app
    "Mining-Stackchan-Core2", // app_name
    "0.681",                   // app_version

    // attention
    mcCfgAttentionText(),

    // speech lines
    mcCfgShareAcceptedText(),
    mcCfgHelloText()
  };

  // 実行中にSETされた場合にも反映されるよう、毎回上書き（ポインタ差し替えのみ）
  cfg.wifi_ssid      = mcCfgWifiSsid();
  cfg.wifi_pass      = mcCfgWifiPass();
  cfg.duco_user      = mcCfgDucoUser();
  cfg.duco_miner_key = mcCfgDucoKey();
  cfg.az_speech_region  = mcCfgAzRegion();
  cfg.az_speech_key     = mcCfgAzKey();
  cfg.az_tts_voice      = mcCfgAzVoice();
  cfg.attention_text = mcCfgAttentionText();

  cfg.share_accepted_text = mcCfgShareAcceptedText();
  cfg.hello_text          = mcCfgHelloText();

  return cfg;
}
