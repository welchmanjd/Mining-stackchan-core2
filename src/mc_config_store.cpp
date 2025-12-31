// src/mc_config_store.cpp
#include "mc_config_store.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>

#include "config.h"   // config_private.h の読み込み条件(MC_DISABLE_CONFIG_PRIVATE)を尊重
#include "logging.h"

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

  uint32_t display_sleep_s = MC_DISPLAY_SLEEP_SECONDS;
  String attention_text;
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

  g_rt.display_sleep_s = (uint32_t)MC_DISPLAY_SLEEP_SECONDS;
  g_rt.attention_text  = MC_ATTENTION_TEXT;
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

  DynamicJsonDocument doc(4096);
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

  setStr("wifi_ssid", g_rt.wifi_ssid);
  setStr("wifi_pass", g_rt.wifi_pass);

  setStr("duco_user", g_rt.duco_user);
  setStr("duco_key",  g_rt.duco_key);

  setStr("az_region", g_rt.az_region);
  setStr("az_key",    g_rt.az_key);
  setStr("az_voice",  g_rt.az_voice);

  setStr("az_endpoint", g_rt.az_endpoint);

  setU32("display_sleep_s", g_rt.display_sleep_s);
  setStr("attention_text",  g_rt.attention_text);

  mc_logf("[CFG] loaded %s\n", kCfgPath);
}

static bool isKeyKnown_(const String& key) {
  return key == "wifi_ssid" || key == "wifi_pass" ||
         key == "duco_user" || key == "duco_key" || key == "duco_miner_key" ||
         key == "az_region" || key == "az_speech_region" ||
         key == "az_key"    || key == "az_speech_key" ||
         key == "az_voice"  || key == "az_tts_voice" ||
         key == "az_endpoint" || key == "az_custom_subdomain" ||
         key == "display_sleep_s" ||
         key == "attention_text";
}

} // namespace

void mcConfigBegin() {
  loadOnce_();
}

bool mcConfigSetKV(const String& key, const String& value, String& err) {
  loadOnce_();
  err = "";

  if (!isKeyKnown_(key)) {
    err = "unknown_key";
    return false;
  }

  auto setDirty = [&] { g_dirty = true; };

  if (key == "wifi_ssid") { g_rt.wifi_ssid = value; setDirty(); return true; }
  if (key == "wifi_pass") { g_rt.wifi_pass = value; setDirty(); return true; }

  if (key == "duco_user") { g_rt.duco_user = value; setDirty(); return true; }
  if (key == "duco_key" || key == "duco_miner_key") { g_rt.duco_key = value; setDirty(); return true; }

  if (key == "az_region" || key == "az_speech_region") { g_rt.az_region = value; setDirty(); return true; }
  if (key == "az_key"    || key == "az_speech_key")    { g_rt.az_key    = value; setDirty(); return true; }
  if (key == "az_voice"  || key == "az_tts_voice")     { g_rt.az_voice  = value; setDirty(); return true; }

  if (key == "az_endpoint" || key == "az_custom_subdomain") {
    g_rt.az_endpoint = value;
    setDirty();
    return true;
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

  DynamicJsonDocument doc(4096);

  doc["wifi_ssid"] = g_rt.wifi_ssid;
  doc["wifi_pass"] = g_rt.wifi_pass;

  doc["duco_user"] = g_rt.duco_user;
  doc["duco_key"]  = g_rt.duco_key;

  doc["az_region"] = g_rt.az_region;
  doc["az_key"]    = g_rt.az_key;
  doc["az_voice"]  = g_rt.az_voice;

  doc["az_endpoint"] = g_rt.az_endpoint;

  doc["display_sleep_s"] = g_rt.display_sleep_s;
  doc["attention_text"]  = g_rt.attention_text;

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

  DynamicJsonDocument doc(2048);
  doc["wifi_ssid"] = g_rt.wifi_ssid;
  doc["wifi_pass"] = "***";

  doc["duco_user"] = g_rt.duco_user;
  doc["duco_key"]  = "***";

  doc["az_region"] = g_rt.az_region;
  doc["az_key"]    = "***";
  doc["az_voice"]  = g_rt.az_voice;

  doc["az_endpoint"] = g_rt.az_endpoint;

  doc["display_sleep_s"] = g_rt.display_sleep_s;
  doc["attention_text"]  = g_rt.attention_text;

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

uint32_t mcCfgDisplaySleepSeconds() { loadOnce_(); return g_rt.display_sleep_s; }
const char* mcCfgAttentionText()    { loadOnce_(); return g_rt.attention_text.c_str(); }
