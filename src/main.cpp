// src/main.cpp
// ===== Mining-chan Core2 — main entry (UI + orchestrator) =====
// Board   : M5Stack Core2
// Libs    : M5Unified, ArduinoJson, WiFi, WiFiClientSecure, HTTPClient, m5stack-avatar
// Notes   : マイニング処理は mining_task.* に分離。
//           画面描画は ui_mining_core2.h に集約。

#include <M5Unified.h>
#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <stdarg.h>
#include <esp32-hal-cpu.h>

#include "ui_mining_core2.h"
#include "app_presenter.h"
#include "config.h"
#include "mining_task.h"
#include "logging.h"   // ← 他の #include と一緒に、ファイル先頭の方へ移動推奨
#include "azure_tts.h"


// UI 更新用の前回時刻 [ms]
static unsigned long lastUiMs = 0;

// Aボタンで切り替える画面モード
static bool g_stackchanMode = false;

// "Attention" ("WHAT?") mode: short-lived focus state triggered by tap in Stackchan screen.
static bool     g_attentionActive = false;
static uint32_t g_attentionUntilMs = 0;
static MiningYieldProfile g_savedYield = MiningYieldNormal();
static bool     g_savedYieldValid = false;

// Azure TTS
static AzureTts g_tts;

// ===== 自動スリープ関連 =====

// 最後に「ユーザー入力」があった時刻 [ms]
static unsigned long lastInputMs = 0;

// 画面がスリープ（消灯）中かどうか
static bool displaySleeping = false;

// NTP が一度設定されたかどうか
static bool g_timeNtpDone = false;

// 画面関連の定数
static const uint8_t  DISPLAY_ACTIVE_BRIGHTNESS = 128;     // 通常時の明るさ
static const uint32_t DISPLAY_SLEEP_TIMEOUT_MS  =
    (uint32_t)MC_DISPLAY_SLEEP_SECONDS * 1000UL;           // 設定値[秒]→[ms]で画面OFF

// スリープ前の「Zzz…」表示時間 [ms]
static const uint32_t DISPLAY_SLEEP_MESSAGE_MS  = 5000UL;  // ここを変えれば好きな秒数に


// ---- TTS中のマイニング制御（安全側） ----
static int s_baseThreads = -1;     // 通常時のthreadsを記憶
static int s_appliedThreads = -999;
static uint32_t s_zeroSince = 0;

static void applyMiningPolicyForTts(bool ttsBusy) {
  if (s_baseThreads < 0) s_baseThreads = (int)getMiningActiveThreads(); // 通常は2のはず

  const bool speaking = M5.Speaker.isPlaying();

  // 方針：
  //  - 再生中だけ 0（最強に安定）
  //  - 取得中は 1（任意。通信が詰まりやすいなら効く）
  //  - それ以外は元に戻す
  int target = s_baseThreads;
  if (speaking) target = 0;
  else if (ttsBusy) target = 1;   // ←重い/詰まるなら有効。嫌ならコメントアウトして target=s_baseThreads

  // 安全装置：しゃべってないのに 0 が続いたら強制復帰（失敗時の“止まりっぱなし”防止）
  if (!speaking && getMiningActiveThreads() == 0) {
    if (s_zeroSince == 0) s_zeroSince = millis();
    if (millis() - s_zeroSince > 5000) {
      mc_logf("[TTS] safety restore mining -> %d", s_baseThreads);
      target = s_baseThreads;
      s_zeroSince = 0;
    }
  } else {
    s_zeroSince = 0;
  }

  if (target != s_appliedThreads) {
    mc_logf("[TTS] mining threads: %d -> %d (busy=%d speaking=%d)",
            (int)getMiningActiveThreads(), target, (int)ttsBusy, (int)speaking);
    setMiningActiveThreads((uint8_t)target);
    s_appliedThreads = target;
  }
}





// ---------------- WiFi / Time ----------------

// WiFi 接続を「状態マシン化」したノンブロッキング版。
// 毎フレーム呼び出される前提で、
//   - 初回呼び出し時に WiFi.begin() をキック
//   - 接続が完了するかタイムアウトしたら true を返す
//   - それまでは false を返す
// ※接続に成功したかどうかは WiFi.status() == WL_CONNECTED で判定する。
static bool wifi_connect() {
  const auto& cfg = appConfig();

  // 状態を static で保持
  enum WifiState {
    WIFI_NOT_STARTED,
    WIFI_CONNECTING,
    WIFI_DONE
  };
  static WifiState   state   = WIFI_NOT_STARTED;
  static uint32_t    t_start = 0;
  static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000UL;

  switch (state) {
    case WIFI_NOT_STARTED: {
      WiFi.mode(WIFI_STA);
      WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);
      t_start = millis();
      mc_logf("[WIFI] begin connect (ssid=%s)", cfg.wifi_ssid);
      state = WIFI_CONNECTING;
      return false;
    }

    case WIFI_CONNECTING: {
      wl_status_t st = WiFi.status();
      if (st == WL_CONNECTED) {
        mc_logf("[WIFI] connected: %s", WiFi.localIP().toString().c_str());
        state = WIFI_DONE;
        return true;
      }
      if (millis() - t_start > WIFI_CONNECT_TIMEOUT_MS) {
        mc_logf("[WIFI] connect timeout (status=%d)", (int)st);
        state = WIFI_DONE;
        // 「接続試行」としては終わったので true を返す（成功/失敗は WiFi.status() で見る）
        return true;
      }
      // まだ接続試行中
      return false;
    }

    case WIFI_DONE:
    default:
      // 2回目以降は何もしない
      return true;
  }
}


static void setupTimeNTP() {
  setenv("TZ", "JST-9", 1);
  tzset();
  configTime(9 * 3600, 0,
             "ntp.nict.jp",
             "time.google.com",
             "pool.ntp.org");
}





// ---------------- Arduino entry points ----------------
void setup() {
  // --- シリアルとログ（最初に開く） ---
  Serial.begin(115200);
  delay(50);
  mc_logf("[MAIN] setup() start");

  // --- CPUクロックを最大に ---
  setCpuFrequencyMhz(240);

  // --- M5Unified の設定 ---
  auto cfg_m5 = M5.config();
  cfg_m5.output_power  = true;   // 外部5VはON
  cfg_m5.clear_display = true;   // 起動時に画面クリア

  // 使っていない内蔵デバイスはOFFにしておくと安定度アップが期待できる
  cfg_m5.internal_imu = false;   // 今はIMU使っていないのでOFF
  cfg_m5.internal_mic = false;   // マイクも使っていないのでOFF
  cfg_m5.internal_spk = true;    // スピーカーはビープで使うのでON
  cfg_m5.internal_rtc = true;    // RTCはNTPと併用したいのでONのまま

  mc_logf("[MAIN] call M5.begin()");
  M5.begin(cfg_m5);
  mc_logf("[MAIN] M5.begin() done");

  // config（cfg を使う処理がこの後にあるので、ここで取っておく）
  const auto& cfg = appConfig();

  // Azure TTS 初期化
  g_tts.begin();

  // --- 画面の初期状態 ---
  M5.Display.setBrightness(DISPLAY_ACTIVE_BRIGHTNESS);
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextColor(WHITE, BLACK);


  // ★ UI起動 & スプラッシュ表示
  UIMining::instance().begin(cfg.app_name, cfg.app_version);

  // スタックチャン「喋る/黙る」時間設定（単位: ms）
  // 喋る時間 = talkMin + 0〜talkVar の乱数加算
  // 黙る時間 = silentMin + 0〜silentVar の乱数加算
  UIMining::instance().setStackchanSpeechTiming(
    2200, 1200,   // 喋る: 最短2200ms + (0〜1200ms) → 2.2〜3.4秒
    900,  1400    // 黙る: 最短 900ms + (0〜1400ms) → 0.9〜2.3秒
  );


  // タイマー類の初期化
  lastUiMs        = 0;
  lastInputMs     = millis();
  displaySleeping = false;

  // 起動ログ
  mc_logf("%s %s booting...", cfg.app_name, cfg.app_version);

  // ★ WiFi/NTPは loop() 側でノンブロッキングに進めるのでここでは呼ばない
  // wifi_connect();
  // setupTimeNTP();

  // FreeRTOS タスクでマイニング開始
  startMiner();
}




void loop() {
  M5.update();

  unsigned long now = millis();

  // TTSの再生開始/終了処理（スリープ中でも呼びたいので早めに）
  g_tts.poll();
  applyMiningPolicyForTts(g_tts.isBusy());

  // Wi-Fi切断検知：keep-alive中のTLSセッションを次回に備えて破棄予約
  static wl_status_t s_prevWifi = WL_IDLE_STATUS;
  wl_status_t wifiNow = WiFi.status();
  if (s_prevWifi == WL_CONNECTED && wifiNow != WL_CONNECTED) {
    mc_logf("[WIFI] disconnected (status=%d) -> reset TTS session", (int)wifiNow);
    g_tts.requestSessionReset();
  }
  s_prevWifi = wifiNow;

  // --- 入力検出（ボタン + タッチ） ---
  bool anyInput = false;

  // 物理ボタン
  if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed()) {
    anyInput = true;
  }
  
  // タッチ入力（短タップも拾えるように「押された瞬間」を検出）
  static bool prevTouchPressed = false;
  bool touchPressed = false;
  bool touchDown    = false;

  auto& tp = M5.Touch;
  if (tp.isEnabled()) {
    auto det = tp.getDetail();
    touchPressed = det.isPressed();
    touchDown    = touchPressed && !prevTouchPressed;
    prevTouchPressed = touchPressed;
    if (touchPressed) {
      anyInput = true;
    }
  }

  // --- スリープ中の復帰処理 ---
  if (displaySleeping) {
    if (anyInput) {
      // 何か入力があったら画面ONにして、このフレームは「起きるだけ」
      mc_logf("[MAIN] display wake (sleep off)");
      M5.Display.setBrightness(DISPLAY_ACTIVE_BRIGHTNESS);
      displaySleeping = false;
      lastInputMs     = now;
    }

    delay(2);
    return;
  }


  // ここから「画面がON」の時の処理


  // Bボタン：固定文を喋る（まずは動作確認用）
  if (M5.BtnB.wasPressed()) {
    const char* text = "こんにちはマイニングスタックチャンです。";
    if (!g_tts.speakAsync(text)) {
      mc_logf("[TTS] speakAsync failed (busy / wifi / config?)");
    }
  }

  UIMining& ui = UIMining::instance();

  if (anyInput) {
    lastInputMs = now;
  }

  // --- Aボタン：画面モード切り替え + ビープ ---
  // （スタックチャン画面に入る時も必ずピッと鳴る）
  if (M5.BtnA.wasPressed()) {
    M5.Speaker.tone(1500, 50);  // 他の場所と同じ 1500Hz / 50ms

    g_stackchanMode = !g_stackchanMode;
    mc_logf("[MAIN] BtnA pressed, stackchanMode=%d", (int)g_stackchanMode);

    // (ui is already referenced above)
    if (g_stackchanMode) {
      ui.onEnterStackchanMode();
    } else {
      ui.onLeaveStackchanMode();

      // Leaving stackchan mode -> clear attention + restore mining yield
      if (g_attentionActive) {
        g_attentionActive = false;
        if (g_savedYieldValid) setMiningYieldProfile(g_savedYield);
        ui.triggerAttention(0);
      }
    }
  }

  // --- Attention mode: tap in Stackchan screen to go "WHAT?" and throttle mining ---
  // NOTE: Right now we only apply "strong yield" (so mining continues but UI becomes very responsive).
  // Future hooks:
  //   - STOP: setMiningActiveThreads(0)
  //   - HALF: setMiningActiveThreads(1)
  if (g_stackchanMode && touchDown && !displaySleeping) {
    const uint32_t dur = 3000; // ms
    mc_logf("[ATTN] enter");

    // save current yield once
    if (!g_attentionActive) {
      g_savedYield = getMiningYieldProfile();
      g_savedYieldValid = true;
    }

    g_attentionActive = true;
    g_attentionUntilMs = now + dur;

    setMiningYieldProfile(MiningYieldStrong());
    ui.triggerAttention(dur, "WHAT?");
    M5.Speaker.tone(1800, 30);
  }

  // Attention timeout -> restore mining yield and clear bubble
  if (g_attentionActive && (int32_t)(g_attentionUntilMs - now) <= 0) {
    g_attentionActive = false;
    mc_logf("[ATTN] exit");

    if (g_savedYieldValid) {
      setMiningYieldProfile(g_savedYield);
    } else {
      setMiningYieldProfile(MiningYieldNormal());
    }
    ui.triggerAttention(0);
  }


  // --- 起動時の WiFi 接続 & NTP 同期（ノンブロッキング） ---
  bool wifiDone = wifi_connect();
  if (wifiDone && !g_timeNtpDone && WiFi.status() == WL_CONNECTED) {
    setupTimeNTP();
    g_timeNtpDone = true;
  }


  // --- UI 更新（100ms ごとに1回） ---
  if (now - lastUiMs >= 100) {
    lastUiMs = now;

    MiningSummary summary;
    updateMiningSummary(summary);

    // (ui is already referenced above)
    UIMining::PanelData data;
    buildPanelData(summary, ui, data);

    // ticker は Step1 の buildTicker(summary) のまま
    String ticker = buildTicker(summary);


    // ★ ここで画面を切り替え
    if (g_stackchanMode) {
      ui.drawStackchanScreen(data);   // スタックチャン画面
    } else {
      ui.drawAll(data, ticker);       // ダッシュボード画面
    }
  }

  // --- 一定時間無操作なら画面OFF（マイニングは継続） ---
  if (!displaySleeping && (now - lastInputMs >= DISPLAY_SLEEP_TIMEOUT_MS)) {
    mc_logf("[MAIN] display sleep (screen off)");

    // 右パネルだけ「Zzz…」メッセージを1秒だけ表示
    UIMining::instance().drawSleepMessage();
    delay(DISPLAY_SLEEP_MESSAGE_MS);  // ★ ここを定数に

    M5.Display.setBrightness(0);
    displaySleeping = true;
  }



  static bool    s_ttsThrottling = false;
  static uint8_t s_savedThreads  = 0;  // 初期値は何でもOK（開始時に保存するので）


  if (g_tts.isBusy()) {
    if (!s_ttsThrottling) {
      s_savedThreads = getMiningActiveThreads();
      setMiningActiveThreads(MC_TTS_ACTIVE_THREADS_DURING_TTS);
      s_ttsThrottling = true;
      mc_logf("[TTS] mining throttle: threads %u -> %u",
              (unsigned)s_savedThreads, (unsigned)MC_TTS_ACTIVE_THREADS_DURING_TTS);
    }
  } else {
    if (s_ttsThrottling) {
      setMiningActiveThreads(s_savedThreads);
      s_ttsThrottling = false;
      mc_logf("[TTS] mining restore: threads -> %u", (unsigned)s_savedThreads);
    }
  }



  delay(2);
}
