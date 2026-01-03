// src/mc_config_store.h
#pragma once
#include <Arduino.h>

// 起動時に一度だけLittleFSから設定をロード
void mcConfigBegin();

// WebSerial等から key/value をセット（未保存）
bool mcConfigSetKV(const String& key, const String& val, String& err);

// LittleFSへ保存
bool mcConfigSave(String& err);

// 現在の設定を“マスク付き”JSONで返す（秘密は *_set の bool で表現）
String mcConfigGetMaskedJson();

// ---- getters ----
// runtime(=LittleFS)が空なら、config_private.h のマクロへフォールバック（distでは無効化される）
const char* mcCfgWifiSsid();
const char* mcCfgWifiPass();

const char* mcCfgDucoUser();
const char* mcCfgDucoKey();

const char* mcCfgAzRegion();
const char* mcCfgAzKey();
const char* mcCfgAzVoice();

// 任意：カスタムサブドメイン/エンドポイント（未設定なら空）
const char* mcCfgAzEndpoint();

const char* mcCfgAttentionText();

uint8_t mcCfgSpkVolume();

const char* mcCfgShareAcceptedText();
const char* mcCfgHelloText();

uint32_t mcCfgCpuMhz();
