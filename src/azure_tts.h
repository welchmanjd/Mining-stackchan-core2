// src/azure_tts.h
#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// Azure TTS を「取得(HTTPS)→WAV再生(M5Unified)」までやる最小モジュール。
// ・speakAsync() でHTTP取得は別タスク
// ・loop() から poll() を呼ぶと、準備完了したWAVを再生し、終了後にfreeする
//
// 追加(2025-12):
// ・HTTPS接続をできるだけ使い回す(keep-alive)ことでラグ短縮を狙う
// ・Wi-Fi切断を検知したら、次回に備えてHTTP/TLSセッションを捨てる

class AzureTts {
public:
  // config_private.h の MC_AZ_* から設定する（begin() だけでOK）
  void begin(uint8_t volume = 180);

  // 1回しゃべらせる（非同期で取得→poll()で再生開始）
  // 返り値 false は「WiFi未接続」「設定不足」「すでに処理中」など
  bool speakAsync(const String& text, const char* voice = nullptr);

  // main loop から毎フレーム呼ぶ（再生開始・終了処理・メモリ解放）
  void poll();

  bool isBusy() const;

  // Wi-Fi切断などでセッションが壊れていそうな時に呼ぶ。
  // ※実際の破棄は安全なタイミング(Idle)で行う
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

  LastResult lastResult() const;


private:
  enum State : uint8_t { Idle, Fetching, Ready, Playing, Error };

  static void taskEntry(void* pv);
  void taskBody();

  static String xmlEscape_(const String& s);
  String buildSsml_(const String& text, const String& voice) const;

  bool fetchWav_(const String& ssml, uint8_t** outBuf, size_t* outLen);

  // ---- new (token + dns warmup + rate limit) ----
  void warmupDnsOnce_();

  bool ensureToken_();
  bool fetchTokenOld_(String* outTok);
  bool fetchTokenNew_(String* outTok);

  bool dnsWarmed_ = false;

  String   token_;
  uint32_t tokenExpireMs_ = 0;
  bool     preferOldSts_ = true;

  uint32_t lastRequestMs_ = 0;  // リクエスト間隔制御用

  void resetSession_();  // internal (do not call from other threads)

private:
  volatile State state_ = Idle;
  TaskHandle_t   task_  = nullptr;

  // request (Idle→Fetching の間だけ保持)
  String reqText_;
  String reqVoice_;

  // Azure config (begin() で設定)
  String endpoint_;
  String key_;
  String defaultVoice_;

  // wav buffer (Ready/Playing の間保持)
  uint8_t* wav_    = nullptr;
  size_t   wavLen_ = 0;

  // keep-alive session owned by this object
  WiFiClientSecure client_;
  HTTPClient       https_;
  bool             keepaliveEnabled_ = true;

  // Wi-Fi切断等を検知したら true (Idleになったタイミングで resetSession_ する)
  volatile bool sessionResetPending_ = false;

  uint32_t last_ok_ms_ = 0;                 // 最後に正常に音声を取れた時刻
  uint32_t disable_keepalive_until_ms_ = 0; // 失敗後しばらく keep-alive 禁止

  RuntimeConfig cfg_;
  bool playbackEnabled_ = true;

  uint32_t seq_ = 0;
  LastResult last_;

};

