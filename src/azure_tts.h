// src/azure_tts.h
#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// Azure TTS を「取得(HTTPS)→WAV再生(M5Unified)」まで行う最小モジュール。
// ・speakAsync() でHTTP取得は別タスク
// ・loop() から poll() を呼ぶと、準備完了したWAVを再生し、終わったらfreeする
//
// ★重要：設定は config_private の MC_AZ_* だけでなく、mc_config_store(LittleFS)の値も使う
class AzureTts {
public:
  void begin(uint8_t volume = 180);

  bool speakAsync(const String& text, uint32_t speakId, const char* voice = nullptr);
  bool speakAsync(const String& text, const char* voice = nullptr) { return speakAsync(text, 0, voice); }

  void poll();

  bool isBusy() const;
  bool consumeDone(uint32_t* outId);

  void requestSessionReset();

  struct RuntimeConfig {
    bool     keepAlive = true;
    uint32_t httpTimeoutMs = 20000;
    uint32_t bodyStartTimeoutMs = 900;

    // chunked decode timeouts
    uint32_t chunkTotalTimeoutMs = 15000;
    uint32_t chunkSizeLineTimeoutMs = 3000;
    uint32_t chunkDataIdleTimeoutMs = 5000;

    // Content-Length read idle timeout
    uint32_t contentReadIdleTimeoutMs = 20000;
  };

  struct LastResult {
    uint32_t seq = 0;
    bool ok = false;
    bool chunked = false;
    bool keepAlive = true;
    int  httpCode = 0;
    uint32_t bytes = 0;
    uint32_t fetchMs = 0;
    char err[24] = {0};
  };

  void setRuntimeConfig(const RuntimeConfig& cfg);
  RuntimeConfig runtimeConfig() const;

  void setPlaybackEnabled(bool en);
  bool playbackEnabled() const;

  bool testCredentials();

  LastResult lastResult() const;

private:
  enum State : uint8_t { Idle, Fetching, Ready, Playing, Error };

  static void taskEntry(void* pv);
  void taskBody();

  static String xmlEscape_(const String& s);
  String buildSsml_(const String& text, const String& voice) const;

  bool fetchWav_(const String& ssml, uint8_t** outBuf, size_t* outLen);

  void warmupDnsOnce_();

  bool ensureToken_();
  bool fetchTokenOld_(String* outTok);

  void resetSession_();

private:
  volatile State state_ = Idle;
  TaskHandle_t   task_  = nullptr;
  uint32_t currentSpeakId_ = 0;
  volatile uint32_t doneSpeakId_ = 0;

  String reqText_;
  String reqVoice_;

  // Azure config (begin() で設定)
  String endpoint_;       // 送信先 URL
  String key_;            // subscription key
  String defaultVoice_;   // default voice

  // token/ヘッダ用
  String region_;         // Speech resource region (例: japaneast)
  String customHost_;     // custom host (例: xxxx.cognitiveservices.azure.com) / 空ならregion endpoint

  bool dnsWarmed_ = false;

  String   token_;
  uint32_t tokenExpireMs_ = 0;

  uint32_t tokenFailUntilMs_ = 0;
  uint32_t lastRequestMs_ = 0;
  uint8_t  tokenFailCount_   = 0;

  uint8_t* wav_    = nullptr;
  size_t   wavLen_ = 0;

  WiFiClientSecure client_;
  HTTPClient       https_;
  bool             keepaliveEnabled_ = true;

  volatile bool sessionResetPending_ = false;

  uint32_t last_ok_ms_ = 0;
  uint32_t disable_keepalive_until_ms_ = 0;

  RuntimeConfig cfg_;
  bool playbackEnabled_ = true;

  uint32_t seq_ = 0;
  LastResult last_;
};
