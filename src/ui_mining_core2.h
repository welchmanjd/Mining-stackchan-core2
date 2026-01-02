// src/ui_mining_core2.h
#pragma once
// ===== Mining stackchan UI for Core2 (Spec-aligned + anti-flicker) =====
// Layout:
//   - Left  : Avatar 144x216
//   - Right : Info   176x216
//   - Bottom: Ticker 320x24
//
// Right panel spec (from 表情設計):
//   Font0 x2 (approx 12x16 cell), 14 cols x 4 rows + header
//   Label 4 chars + space + value up to 9 chars
//   Header title centered + 3-dot indicator
//   Pages: 0 MINING, 1 DEVICE, 2 NETWORK
//   Tap inside right panel => next page
//
// Anti-flicker:
//   - Draw right panel into sprite then push once
//   - Draw ticker into sprite then push once

#include <M5Unified.h>
#include <Arduino.h>
#include <Avatar.h>
#include <WiFi.h>
#include <M5GFX.h>
#include <math.h>

class UIMining {
public:
  struct PanelData {
    float    hr_kh      = 0.0f;
    uint32_t accepted   = 0;
    uint32_t rejected   = 0;

    float    ping_ms    = -1.0f;

    float    rej_pct    = 0.0f;   // 互換用（使わなくてもOK）
    float    bestshare  = -1.0f;

    bool     poolAlive  = false;
    bool     miningEnabled = false;
    float    diff       = 0.0f;

    uint32_t elapsed_s  = 0;

    String   sw;
    String   fw;
    String   poolName;
    String   worker;

    // ★追加: 起動スプラッシュ用の診断メッセージ
    String   wifiDiag;
    String   poolDiag;
  };


  // 画面レイアウト定数（既存どおり）
  enum : int {
    W = 320, H = 240,
    LOG_H = 24,
    AV_W = 144, AV_H = 216,
    INF_W = 176, INF_H = 216,
    X_INF = 144, Y_LOG = 216
  };

  // シングルトンインスタンス取得
  static UIMining& instance();

  // 初期化
  void begin(const char* appName, const char* appVer);

  // ファーム短縮表記（今は固定文字列）
  String shortFwString() const;

  // uptime 秒
  uint32_t uptimeSeconds() const;

  // ハッシュレート基準値（色判定用）
  void setHashrateReference(float kh);
  void setAutoPageMs(uint32_t ms);

  // 1フレーム分の描画
  void drawAll(const PanelData& p, const String& tickerText, bool suppressTouchBeep = false);

  // Touch snapshot: main loop should read touch (I2C) once and pass it to UI.
  // This avoids occasional I2C hangs/freezes caused by multiple touch reads per frame.
  struct TouchSnapshot {
    bool enabled = false;
    bool pressed = false;
    bool down    = false;  // rising edge (pressed && !prev)
    int  x       = 0;
    int  y       = 0;
  };

  void setTouchSnapshot(const TouchSnapshot& s);


  // 起動時のスプラッシュ画面（WiFi / Pool の2行 + 診断）
  void drawSplash(const String& wifiText,  uint16_t wifiCol,
                  const String& poolText,  uint16_t poolCol,
                  const String& wifiHint,  const String& poolHint);


  // 自動スリープ直前に表示するメッセージ
  void drawSleepMessage();

  // Aボタンで切り替える「スタックチャン画面」（暫定・文字だけ版）
  void drawStackchanScreen(const PanelData& p);

  // 画面モードの切り替え通知（main から呼ぶ）
  void onEnterStackchanMode();
  void onLeaveStackchanMode();

  // "何？" attention mode: short-lived focus state triggered by tap.
  // durationMs==0 -> clear.
  void triggerAttention(uint32_t durationMs, const char* text = nullptr);
  bool isAttentionActive() const;
  void setAttentionDefaultText(const char* text);

  // ★ 追加: 目線・まばたき・ゆらぎをまとめて更新
  void updateAvatarLiveliness();

  // しゃべる/黙る時間（ms）を外から調整できるようにする
  void setStackchanSpeechTiming(uint32_t talkMinMs, uint32_t talkVarMs,
                                uint32_t silentMinMs, uint32_t silentVarMs);

  // --- 外部(TTS)向け：スタックチャン吹き出しの「新規発話」イベント ---
  uint32_t stackchanSpeechSeq() const { return stackchan_speech_seq_; }
  const String& stackchanSpeechText() const { return stackchan_speech_text_; }

  // Behavior など外部から吹き出しと表情を直接指定する
  void setStackchanSpeech(const String& text);
  void setStackchanExpression(m5avatar::Expression exp);

private:


private:
  // コンストラクタは外から触らせない（instance() からのみ）
  UIMining();

  // ---------- Spec constants (relative to right panel origin) ----------
  static constexpr int PAD_LR = 4;
  static constexpr int PAD_T  = 4;

  static constexpr int CHAR_W = 12; // Font0 * 2
  static constexpr int CHAR_H = 16;

  static constexpr int X_LABEL = PAD_LR;             // abs148 -> rel4
  static constexpr int X_VALUE = PAD_LR + CHAR_W*5;  // abs208 -> rel64

  static constexpr int Y_HEADER = 4;
  static constexpr int Y1 = 24;
  static constexpr int Y2 = 42;
  static constexpr int Y3 = 60;
  static constexpr int Y4 = 78;

  static constexpr int IND_R = 2;
  static constexpr int IND_Y = 12;

  // abs 302/308/314 -> rel 158/164/170
  static constexpr int IND_X1 = 158;
  static constexpr int IND_X2 = 164;
  static constexpr int IND_X3 = 170;

  static constexpr uint16_t COL_LABEL = 0xC618; // light grey
  static constexpr uint16_t COL_DARK  = 0x4208;

  struct TextLayoutY {
    int header, y1, y2, y3, y4;
    int ind_y;
  };

  TextLayoutY computeTextLayoutY() const;

  TouchSnapshot touch_;


  // ---------- State ----------
  String app_name_;
  String app_ver_;

  m5avatar::Avatar avatar_;

  M5Canvas info_;
  M5Canvas tick_;

  int      info_page_      = 0;
  uint32_t last_page_ms_   = 0;
  uint32_t auto_page_ms_   = 0;

  uint32_t last_total_shares_ = 0;
  uint32_t last_share_ms_     = 0;

  // ticker
  String   ticker_last_;      // 最後に受け取った1文
  String   ticker_log_;       // 流し続けるログ全体
  int32_t  ticker_offset_ = 0;
  uint32_t last_tick_ms_  = 0;

  float hr_ref_kh_ = 0.0f;
  
  // スプラッシュ画面の状態
  bool     splash_active_   = false;
  uint32_t splash_start_ms_ = 0;
  uint32_t splash_ready_ms_ = 0;   // WiFi & Pool が揃って OK になった時刻

  // スプラッシュの前回描画内容（チラつき防止用）
  String   splash_wifi_text_;
  String   splash_pool_text_;
  String   splash_wifi_hint_;
  String   splash_pool_hint_;
  uint16_t splash_wifi_col_  = WHITE;
  uint16_t splash_pool_col_  = WHITE;


  // 画面モード状態
  bool   in_stackchan_mode_   = false;  // 今スタックチャン画面か
  bool   stackchan_needs_clear_ = false; // 入った直後に1回だけ画面クリアするため

  // スタックチャン「しゃべる/黙る」制御
  bool     stackchan_talking_        = false;
  uint32_t stackchan_phase_start_ms_ = 0;
  uint32_t stackchan_phase_dur_ms_   = 0;
  String   stackchan_bubble_text_;

  // ---- Stackchan avatar updates (deferred) ----
  // M5Stack-Avatar sometimes freezes when setExpression/setSpeechText is called
  // from outside the draw loop. So we only *request* updates here and apply them
  // inside drawStackchanScreen().
  bool                  stackchan_expr_pending_   = false;
  m5avatar::Expression  stackchan_expr_desired_   = m5avatar::Expression::Neutral;
  bool                  stackchan_speech_pending_ = false;
  String                stackchan_speech_desired_;

  // 外部(TTS)向け：新しい吹き出しが生成されたら seq++ して text を更新
  uint32_t stackchan_speech_seq_ = 0;
  String   stackchan_speech_text_;

  // "attention" state ("WHAT?" mode)
  bool     attention_active_    = false;
  uint32_t attention_until_ms_  = 0;
  String   attention_default_text_ = "WHAT?";
  String   attention_text_         = "WHAT?";

  // しゃべる/黙る時間（ms=ミリ秒）
  uint32_t stackchan_talk_min_ms_   = 2500;
  uint32_t stackchan_talk_var_ms_   = 1500; // 0..var を加算
  uint32_t stackchan_silent_min_ms_ = 10000;
  uint32_t stackchan_silent_var_ms_ = 0; // 0..var を加算

  // 機嫌度（-2..+2）: 採掘状況から計算して、表情/動きの強さに使う
  int8_t   mood_level_        = 0;
  uint32_t mood_last_calc_ms_ = 0;

  // mood ログ用
  uint32_t mood_last_report_ms_ = 0;   // 定期ログのタイマー

  // ---------- Static frame ----------
  void drawStaticFrame();

  // ---------- Page input ----------
  void handlePageInput(bool suppressTouchBeep);

  // ---------- Last share age ----------
  void updateLastShareClock(const PanelData& p);
  uint32_t lastShareAgeSec() const;

  // ---------- Font prep ----------
  void prepInfoFont();
  void prepBodyFont();
  void prepHeaderFont();

  // ---------- Header + dots ----------
  void drawDots(const TextLayoutY& ly);
  void drawHeader(const char* title, const TextLayoutY& ly);

  // ---------- Line primitive ----------
  void drawLine(int y, const char* label4, const String& value,
                uint16_t colLabel, uint16_t colValue);

  // ---------- Value formatters ----------
  String vHash(float kh) const;
  String vShare(uint32_t acc, uint32_t rej, uint8_t& successOut);
  String vDiff(float diff) const;
  String vLast(uint32_t age) const;
  String vUp(uint32_t s) const;
  String vTemp(float c) const;
  String vHeap() const;
  String vNet(const PanelData& p) const;
  String vRssi() const;
  String vPool(const String& name) const;

  // ---------- Colors ----------
  uint16_t cHash(const PanelData& p) const;
  uint16_t cShare(uint8_t s) const;
  uint16_t cSharePct(float s) const;
  uint16_t cLast(uint32_t age) const;
  uint16_t cTemp(float c) const;
  uint16_t cHeap(uint32_t kb) const;
  uint16_t cNet(const String& v) const;
  uint16_t cRssi(int rssi) const;
  uint16_t cBatt(int pct) const;

  // ---------- Temperature / Power ----------
  float   readTempC();
  int     batteryPct();
  bool    isExternalPower();
  String  vBatt();

  // ---------- Pages ----------
  void drawPage0(const PanelData& p);
  void drawPage1(const PanelData& p);
  void drawPage2(const PanelData& p);
  void drawPoolNameSmall(const TextLayoutY& ly, const String& name);

  // ---------- Right panel draw ----------
  void drawInfo(const PanelData& p);


  // ---------- Ticker ----------
  void drawTicker(const String& text);

  // ---------- Avatar ----------
  void updateAvatarMood(const PanelData& p);
  String buildStackchanBubble(const PanelData& p);
};
