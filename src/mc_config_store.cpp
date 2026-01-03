// src/mc_config_store.cpp
#include "mc_config_store.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>

#include "config.h"   // config_private.h の読み込み条件(MC_DISABLE_CONFIG_PRIVATE)を尊重
#include "logging.h"

// ---- defaults (config_private.h で上書き可能) ----

#ifndef MC_WIFI_SSID
  #define MC_WIFI_SSID ""
#endif
#ifndef MC_WIFI_PASS
  #define MC_WIFI_PASS ""
#endif
#ifndef MC_DUCO_USER
  #define MC_DUCO_USER ""
#endif
#ifndef MC_DUCO_MINER_KEY
  #define MC_DUCO_MINER_KEY "None"
#endif

#ifndef MC_AZ_SPEECH_REGION
  #define MC_AZ_SPEECH_REGION ""
#endif
#ifndef MC_AZ_SPEECH_KEY
  #define MC_AZ_SPEECH_KEY ""
#endif
#ifndef MC_AZ_TTS_VOICE
  #define MC_AZ_TTS_VOICE ""
#endif

#ifndef MC_AZ_CUSTOM_SUBDOMAIN
  #define MC_AZ_CUSTOM_SUBDOMAIN ""
#endif

#ifndef MC_DISPLAY_SLEEP_SECONDS
  #define MC_DISPLAY_SLEEP_SECONDS 600
#endif
#ifndef MC_ATTENTION_TEXT
  #define MC_ATTENTION_TEXT "Hi there!"
#endif

#ifndef MC_SPK_VOLUME
  #define MC_SPK_VOLUME 160
#endif

// ★追加：CPU動作周波数（MHz）
#ifndef MC_CPU_FREQ_MHZ
  #define MC_CPU_FREQ_MHZ 240
#endif

// ★追加：セリフのデフォルト（config_private.h で差し替え可能）
#ifndef MC_SPEECH_SHARE_ACCEPTED
  #define MC_SPEECH_SHARE_ACCEPTED "シェア獲得したよ！"
#endif
#ifndef MC_SPEECH_HELLO
  #define MC_SPEECH_HELLO "こんにちはマイニングスタックチャンです"
#endif


namespace {

static const char* kCfgPath = "/mc_config.json";

struct RuntimeCfg {
  String wifi_ssid;
  String wifi_pass;

  String duco_user;
  String duco_key;

  String az_region;
  String az_key;
  String az_voice;

  // 任意：Speech リソースのカスタムサブドメイン（空なら未使用）
  String az_endpoint;

  // ★追加：CPU動作周波数（MHz）
  uint16_t cpu_mhz = (uint16_t)MC_CPU_FREQ_MHZ; // 80/160/240

  uint32_t display_sleep_s = MC_DISPLAY_SLEEP_SECONDS;
  String attention_text;
  uint8_t spk_volume = (uint8_t)MC_SPK_VOLUME; // 0-255

  // ★追加：カスタムセリフ
  String speech_share_accepted; // 「シェア獲得したよ」
  String speech_hello;          // 「こんにちはマイニングスタックチャンです」
};


static RuntimeCfg g_rt;
static bool g_loaded = false;
static bool g_dirty  = false;

static void applyDefaults_() {
  g_rt.wifi_ssid = MC_WIFI_SSID;
  g_rt.wifi_pass = MC_WIFI_PASS;

  g_rt.duco_user = MC_DUCO_USER;
  g_rt.duco_key  = MC_DUCO_MINER_KEY;

  g_rt.az_region = MC_AZ_SPEECH_REGION;
  g_rt.az_key    = MC_AZ_SPEECH_KEY;
  g_rt.az_voice  = MC_AZ_TTS_VOICE;

  g_rt.az_endpoint = MC_AZ_CUSTOM_SUBDOMAIN;

  // ★追加：CPU
  g_rt.cpu_mhz     = (uint16_t)MC_CPU_FREQ_MHZ;

  g_rt.display_sleep_s = (uint32_t)MC_DISPLAY_SLEEP_SECONDS;
  g_rt.attention_text  = MC_ATTENTION_TEXT;
  g_rt.spk_volume      = (uint8_t)MC_SPK_VOLUME;

  // ★追加：セリフのデフォルト投入
  g_rt.speech_share_accepted = MC_SPEECH_SHARE_ACCEPTED;
  g_rt.speech_hello          = MC_SPEECH_HELLO;
}


static void loadOnce_() {
  if (g_loaded) return;
  g_loaded = true;

  applyDefaults_();

  if (!LittleFS.begin(true)) {
    mc_logf("[CFG] LittleFS.begin failed (format attempted)\n");
    return;
  }

  if (!LittleFS.exists(kCfgPath)) {
    mc_logf("[CFG] %s not found -> defaults\n", kCfgPath);
    return;
  }

  File f = LittleFS.open(kCfgPath, "r");
  if (!f) {
    mc_logf("[CFG] open failed: %s\n", kCfgPath);
    return;
  }

  // 文字列が増えるので少し余裕を持たせる
  DynamicJsonDocument doc(5120);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    mc_logf("[CFG] JSON parse failed: %s\n", err.c_str());
    return;
  }

  auto setStr = [&](const char* key, String& dst) {
    JsonVariant v = doc[key];
    if (!v.isNull()) dst = v.as<String>();
  };
  auto setU32 = [&](const char* key, uint32_t& dst) {
    JsonVariant v = doc[key];
    if (!v.isNull()) dst = v.as<uint32_t>();
  };

  auto setU8 = [&](const char* key, uint8_t& dst) {
    JsonVariant v = doc[key];
    if (v.isNull()) return;
    int n = v.as<int>();
    if (n < 0) n = 0;
    if (n > 255) n = 255;
    dst = (uint8_t)n;
  };

  auto setCpuMhz = [&](const char* key, uint16_t& dst) {
    JsonVariant v = doc[key];
    if (v.isNull()) return;
    int mhz = v.as<int>();
    // 安全寄りに 80/160/240 のみ許可
    if (!(mhz == 80 || mhz == 160 || mhz == 240)) return;
    dst = (uint16_t)mhz;
  };

  setStr("wifi_ssid", g_rt.wifi_ssid);
  setStr("wifi_pass", g_rt.wifi_pass);

  setStr("duco_user", g_rt.duco_user);

  // 保存JSONは duco_key が正だが、念のため旧名も読む（短いキー優先）
  if (!doc["duco_key"].isNull()) {
    setStr("duco_key", g_rt.duco_key);
  } else {
    setStr("duco_miner_key", g_rt.duco_key);
  }

  // Azureも同様（短いキー優先、無ければ旧名）
  if (!doc["az_region"].isNull()) setStr("az_region", g_rt.az_region);
  else                            setStr("az_speech_region", g_rt.az_region);

  if (!doc["az_key"].isNull()) setStr("az_key", g_rt.az_key);
  else                         setStr("az_speech_key", g_rt.az_key);

  if (!doc["az_voice"].isNull()) setStr("az_voice", g_rt.az_voice);
  else                           setStr("az_tts_voice", g_rt.az_voice);

  if (!doc["az_endpoint"].isNull()) setStr("az_endpoint", g_rt.az_endpoint);
  else                              setStr("az_custom_subdomain", g_rt.az_endpoint);

  // ★CPU動作周波数（MHz）: cpu_mhz のみ（cpu_freq_mhz は完全廃止）
  if (!doc["cpu_mhz"].isNull()) {
    setCpuMhz("cpu_mhz", g_rt.cpu_mhz);
  } else {
    // 旧キー cpu_freq_mhz は読まない。存在しても無視して defaults を維持する。
    if (!doc["cpu_freq_mhz"].isNull()) {
      mc_logf("[CFG] deprecated key ignored: cpu_freq_mhz\n");
    }
  }


  setU32("display_sleep_s", g_rt.display_sleep_s);
  setStr("attention_text",  g_rt.attention_text);
  setU8("spk_volume",       g_rt.spk_volume);

  // ★追加：セリフ
  setStr("share_accepted_text", g_rt.speech_share_accepted);
  setStr("hello_text",          g_rt.speech_hello);

  mc_logf("[CFG] loaded %s\n", kCfgPath);
}



} // namespace

void mcConfigBegin() {
  loadOnce_();
}

bool mcConfigSetKV(const String& key, const String& value, String& err) {
  loadOnce_();
  err = "";

  auto setDirty = [&] { g_dirty = true; };

  if (key == "wifi_ssid") { g_rt.wifi_ssid = value; setDirty(); return true; }
  if (key == "wifi_pass") { g_rt.wifi_pass = value; setDirty(); return true; }

  if (key == "duco_user") { g_rt.duco_user = value; setDirty(); return true; }
  if (key == "duco_miner_key") { g_rt.duco_key = value; setDirty(); return true; }

  if (key == "az_speech_region") { g_rt.az_region = value; setDirty(); return true; }
  if (key == "az_speech_key")    { g_rt.az_key    = value; setDirty(); return true; }
  if (key == "az_tts_voice")     { g_rt.az_voice  = value; setDirty(); return true; }

  if (key == "az_custom_subdomain") {
    g_rt.az_endpoint = value;
    setDirty();
    return true;
  }

  // ★追加：CPU動作周波数（MHz）: cpu_mhz のみ受付（cpu_freq_mhz は完全廃止）
  if (key == "cpu_mhz") {
    char* endp = nullptr;
    long v = strtol(value.c_str(), &endp, 10);
    if (endp == value.c_str() || !(v == 80 || v == 160 || v == 240)) {
      err = "range(80|160|240)";
      return false;
    }
    g_rt.cpu_mhz = (uint16_t)v;
    setDirty();
    return true;
  }
  if (key == "cpu_freq_mhz") {
    err = "deprecated_key";
    return false;
  }


  if (key == "display_sleep_s") {
    char* endp = nullptr;
    long v = strtol(value.c_str(), &endp, 10);
    if (endp == value.c_str() || v < 0) {
      err = "invalid_number";
      return false;
    }
    g_rt.display_sleep_s = (uint32_t)v;
    setDirty();
    return true;
  }

  if (key == "attention_text") {
    g_rt.attention_text = value;
    setDirty();
    return true;
  }

  if (key == "spk_volume") {
    char* endp = nullptr;
    long v = strtol(value.c_str(), &endp, 10);
    if (endp == value.c_str() || v < 0 || v > 255) {
      err = "range(0-255)";
      return false;
    }
    g_rt.spk_volume = (uint8_t)v;
    setDirty();
    return true;
  }

  // ★追加：セリフ
  if (key == "share_accepted_text") {
    g_rt.speech_share_accepted = value;
    setDirty();
    return true;
  }
  if (key == "hello_text") {
    g_rt.speech_hello = value;
    setDirty();
    return true;
  }

  err = "unknown_key";
  return false;
}


bool mcConfigSave(String& err) {
  loadOnce_();
  err = "";

  if (!LittleFS.begin(true)) {
    err = "fs_begin_failed";
    return false;
  }

  DynamicJsonDocument doc(5120);

  doc["wifi_ssid"] = g_rt.wifi_ssid;
  doc["wifi_pass"] = g_rt.wifi_pass;

  doc["duco_user"] = g_rt.duco_user;
  doc["duco_key"]  = g_rt.duco_key;

  doc["az_region"] = g_rt.az_region;
  doc["az_key"]    = g_rt.az_key;
  doc["az_voice"]  = g_rt.az_voice;

  doc["az_endpoint"] = g_rt.az_endpoint;

  // ★追加：CPU
  doc["cpu_mhz"]      = g_rt.cpu_mhz;

  doc["display_sleep_s"] = g_rt.display_sleep_s;
  doc["attention_text"]  = g_rt.attention_text;
  doc["spk_volume"]      = g_rt.spk_volume;

  // ★追加：セリフ
  doc["share_accepted_text"] = g_rt.speech_share_accepted;
  doc["hello_text"]          = g_rt.speech_hello;

  File f = LittleFS.open(kCfgPath, "w");
  if (!f) {
    err = "open_failed";
    return false;
  }
  if (serializeJson(doc, f) == 0) {
    f.close();
    err = "serialize_failed";
    return false;
  }
  f.close();

  g_dirty = false;
  mc_logf("[CFG] saved %s\n", kCfgPath);
  return true;
}


String mcConfigGetMaskedJson() {
  loadOnce_();

  DynamicJsonDocument doc(3072);
  const bool wifi_pass_set = g_rt.wifi_pass.length() > 0;
  const bool duco_key_set  = (g_rt.duco_key.length() > 0) && (g_rt.duco_key != "None");
  const bool az_key_set    = g_rt.az_key.length() > 0;

  doc["wifi_ssid"] = g_rt.wifi_ssid;
  doc["wifi_pass"] = "***";
  doc["wifi_pass_set"] = wifi_pass_set;

  doc["duco_user"] = g_rt.duco_user;

  // short keys (既存互換)
  doc["duco_key"] = "***";
  doc["duco_key_set"] = duco_key_set;

  // legacy keys (統一先) - ★併記
  doc["duco_miner_key"] = "***";
  doc["duco_miner_key_set"] = duco_key_set;

  // short keys (既存互換)
  doc["az_region"] = g_rt.az_region;
  doc["az_key"]    = "***";
  doc["az_key_set"] = az_key_set;
  doc["az_voice"]  = g_rt.az_voice;
  doc["az_endpoint"] = g_rt.az_endpoint;

  // legacy keys (統一先) - ★併記
  doc["az_speech_region"] = g_rt.az_region;
  doc["az_speech_key"]    = "***";
  doc["az_speech_key_set"] = az_key_set;
  doc["az_tts_voice"]     = g_rt.az_voice;
  doc["az_custom_subdomain"] = g_rt.az_endpoint;

  // ★追加：CPU動作周波数（cpu_freq_mhz は完全廃止）
  doc["cpu_mhz"] = g_rt.cpu_mhz;

  doc["display_sleep_s"] = g_rt.display_sleep_s;
  doc["attention_text"]  = g_rt.attention_text;
  doc["spk_volume"]      = g_rt.spk_volume;

  // ★追加：セリフ（秘密じゃないのでそのまま返す）
  doc["share_accepted_text"] = g_rt.speech_share_accepted;
  doc["hello_text"]          = g_rt.speech_hello;

  String out;
  serializeJson(doc, out);
  return out;
}


// ---- getters ----

const char* mcCfgWifiSsid() { loadOnce_(); return g_rt.wifi_ssid.c_str(); }
const char* mcCfgWifiPass() { loadOnce_(); return g_rt.wifi_pass.c_str(); }

const char* mcCfgDucoUser() { loadOnce_(); return g_rt.duco_user.c_str(); }
const char* mcCfgDucoKey()  { loadOnce_(); return g_rt.duco_key.c_str();  }

const char* mcCfgAzRegion() { loadOnce_(); return g_rt.az_region.c_str(); }
const char* mcCfgAzKey()    { loadOnce_(); return g_rt.az_key.c_str();    }
const char* mcCfgAzVoice()  { loadOnce_(); return g_rt.az_voice.c_str();  }

// Speech リソースのカスタムサブドメイン（空なら未使用）
const char* mcCfgAzEndpoint() { loadOnce_(); return g_rt.az_endpoint.c_str(); }

// 既存
const char* mcCfgAttentionText() { loadOnce_(); return g_rt.attention_text.c_str(); }
uint8_t mcCfgSpkVolume()         { loadOnce_(); return g_rt.spk_volume; }

// ★追加：セリフ getters
const char* mcCfgShareAcceptedText() { loadOnce_(); return g_rt.speech_share_accepted.c_str(); }
const char* mcCfgHelloText()         { loadOnce_(); return g_rt.speech_hello.c_str(); }

// ★追加：CPU動作周波数 getter
uint32_t mcCfgCpuMhz() { loadOnce_(); return (uint32_t)g_rt.cpu_mhz; }
