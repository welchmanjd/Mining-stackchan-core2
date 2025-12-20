// src/main.cpp
// ===== Mining-chan Core2  Emain entry (UI + orchestrator) =====
// Board   : M5Stack Core2
// Libs    : M5Unified, ArduinoJson, WiFi, WiFiClientSecure, HTTPClient, m5stack-avatar
// Notes   : マイニング処琁E�E mining_task.* に刁E��、E
//           画面描画は ui_mining_core2.h に雁E��E��E

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
#include "logging.h"   // ↁE他�E #include と一緒に、ファイル先頭の方へ移動推奨
#include "azure_tts.h"
#include "stackchan_behavior.h"

#include "app_test_mode.h"
#include "ui_test_core2.h"


// UI 更新用の前回時刻 [ms]
static unsigned long lastUiMs = 0;

// 画面モードと刁E��替ぁE
enum AppMode : uint8_t {
  MODE_DASH = 0,
  MODE_STACKCHAN = 1,
  MODE_TEST = 2,
};

static AppMode g_mode = MODE_DASH;
static AppMode g_modeBeforeTest = MODE_DASH;

// Test mode
static AppTestMode g_test;
static UITestCore2  g_testUi;

// Test中はマイニングを強制pauseしたぁE��ETS再生中pauseとORで統合！E
static bool g_pauseMiningForTest = false;


// "Attention" ("WHAT?") mode: short-lived focus state triggered by tap in Stackchan screen.
static bool     g_attentionActive = false;
static uint32_t g_attentionUntilMs = 0;
static MiningYieldProfile g_savedYield = MiningYieldNormal();
static bool     g_savedYieldValid = false;

// Azure TTS
static AzureTts g_tts;
static StackchanBehavior g_behavior;
static uint32_t g_ttsIdCounter = 1;
static uint32_t g_ttsInflightId = 0;
static uint32_t g_ttsInflightRid = 0;
static bool     g_ttsPrevBusy = false;
static uint32_t g_lastPopEmptyLogMs = 0;
static bool     g_lastPopEmptyBusy = false;
static AppMode  g_lastPopEmptyMode = MODE_DASH;
static bool     g_lastPopEmptyAttn = false;

// ===== 自動スリープ関連 =====

// 最後に「ユーザー入力」があった時刻 [ms]
static unsigned long lastInputMs = 0;

// 画面がスリープ（消�E�E�中かどぁE��
static bool displaySleeping = false;

// BtnA/B/C の押下に伴ぁE��タチE��開始ビープ』を次のUI更新で1回だけ抑止する
static bool g_suppressTouchBeepOnce = false;


// NTP が一度設定されたかどぁE��
static bool g_timeNtpDone = false;

// 画面関連の定数
static const uint8_t  DISPLAY_ACTIVE_BRIGHTNESS = 128;     // 通常時�E明るぁE
static const uint32_t DISPLAY_SLEEP_TIMEOUT_MS  =
    (uint32_t)MC_DISPLAY_SLEEP_SECONDS * 1000UL;           // 設定値[秒]→[ms]で画面OFF

// スリープ前の「Zzz…」表示時間 [ms]
static const uint32_t DISPLAY_SLEEP_MESSAGE_MS  = 5000UL;  // ここを変えれ�E好きな秒数に


// ---- TTS中のマイニング制御�E�安�E側�E�E----
static int s_baseThreads = -1;     // 通常時�Ethreadsを記�E
static int s_appliedThreads = -999;
static uint32_t s_zeroSince = 0;

// ---- TTS中のマイニング制御�E�捨てなぁEpause 版！E----
// ポイント：スレチE��数めE0 にしなぁE��EOBを捨てなぁE��E
// 再生中だけ「pause」して、終わったら再開する
static bool s_pausedByTts = false;

static void applyMiningPolicyForTts(bool ttsBusy) {
  (void)ttsBusy;

  const bool speaking = M5.Speaker.isPlaying();
  const bool wantPause = speaking || g_pauseMiningForTest;

  if (wantPause != s_pausedByTts) {
    mc_logf("[TTS] mining pause: %d -> %d (speaking=%d test=%d)",
            (int)s_pausedByTts, (int)wantPause, (int)speaking, (int)g_pauseMiningForTest);

    setMiningPaused(wantPause);      // ☁Emining_task 側で実裁E��後述�E�E
    s_pausedByTts = wantPause;
  }
}

//TTS用
// ---------------- WiFi / Time ----------------

// WiFi 接続を「状態�Eシン化」したノンブロチE��ング版、E
// 毎フレーム呼び出される前提で、E
//   - 初回呼び出し時に WiFi.begin() をキチE��
//   - 接続が完亁E��るかタイムアウトしたら true を返す
//   - それまでは false を返す
// ※接続に成功したかどぁE��は WiFi.status() == WL_CONNECTED で判定する、E
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
        // 「接続試行」としては終わった�Eで true を返す�E��E劁E失敗�E WiFi.status() で見る�E�E
        return true;
      }
      // まだ接続試行中
      return false;
    }

    case WIFI_DONE:
    default:
      // 2回目以降�E何もしなぁE
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
  // --- シリアルとログ�E�最初に開く�E�E---
  Serial.begin(115200);
  delay(50);
  mc_logf("[MAIN] setup() start");

  // --- CPUクロチE��を最大に ---
  setCpuFrequencyMhz(240);

  // --- M5Unified の設宁E---
  auto cfg_m5 = M5.config();
  cfg_m5.output_power  = true;   // 外部5VはON
  cfg_m5.clear_display = true;   // 起動時に画面クリア

  // 使ってぁE��ぁE�E蔵チE��イスはOFFにしておくと安定度アチE�Eが期征E��きる
  cfg_m5.internal_imu = false;   // 今�EIMU使ってぁE��ぁE�EでOFF
  cfg_m5.internal_mic = false;   // マイクも使ってぁE��ぁE�EでOFF
  cfg_m5.internal_spk = true;    // スピ�Eカーはビ�Eプで使ぁE�EでON
  cfg_m5.internal_rtc = true;    // RTCはNTPと併用したぁE�EでONのまま

  mc_logf("[MAIN] call M5.begin()");
  M5.begin(cfg_m5);
  mc_logf("[MAIN] M5.begin() done");

  // config�E�Efg を使ぁE�E琁E��こ�E後にあるので、ここで取っておく�E�E
  const auto& cfg = appConfig();

  // Azure TTS 初期匁E
  g_tts.begin();

  //チE��ト枠を�E期化
  g_test.begin(&g_tts);
  g_testUi.begin();

  // --- 画面の初期状慁E---
  M5.Display.setBrightness(DISPLAY_ACTIVE_BRIGHTNESS);
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextColor(WHITE, BLACK);


  // ☁EUI起勁E& スプラチE��ュ表示
  UIMining::instance().begin(cfg.app_name, cfg.app_version);

  // スタチE��チャン「喋めE黙る」時間設定（単佁E ms�E�E
  // 喋る時間 = talkMin + 0〜talkVar の乱数加箁E
  // 黙る時間 = silentMin + 0〜silentVar の乱数加箁E
  UIMining::instance().setStackchanSpeechTiming(
    2200, 1200,   // 喋る: 最短2200ms + (0、E200ms) ↁE2.2、E.4私E
    900,  1400    // 黙る: 最短 900ms + (0、E400ms) ↁE0.9、E.3私E
  );


  // タイマ�E類�E初期匁E
  lastUiMs        = 0;
  lastInputMs     = millis();
  displaySleeping = false;

  // 起動ログ
  mc_logf("%s %s booting...", cfg.app_name, cfg.app_version);

  // ☁EWiFi/NTPは loop() 側でノンブロチE��ングに進めるのでここでは呼ばなぁE
  // wifi_connect();
  // setupTimeNTP();

  // FreeRTOS タスクでマイニング開姁E
  startMiner();
}




void loop() {
  M5.update();

  unsigned long now = millis();

  // TTSの再生開姁E終亁E�E琁E��スリープ中でも呼びたいので早めに�E�E
  g_tts.poll();
  bool ttsBusyNow = g_tts.isBusy();
  if (g_ttsPrevBusy && !ttsBusyNow && g_ttsInflightId != 0) {
    LOG_EVT_INFO("EVT_TTS_DONE", "rid=%lu tts_id=%lu",
                 (unsigned long)g_ttsInflightRid, (unsigned long)g_ttsInflightId);
    g_ttsInflightId = 0;
    g_ttsInflightRid = 0;
  }
  g_ttsPrevBusy = ttsBusyNow;
  g_behavior.setTtsSpeaking(ttsBusyNow);
  applyMiningPolicyForTts(ttsBusyNow);

  // Wi-Fi刁E��検知�E�keep-alive中のTLSセチE��ョンを次回に備えて破棁E��紁E
  static wl_status_t s_prevWifi = WL_IDLE_STATUS;
  wl_status_t wifiNow = WiFi.status();
  if (s_prevWifi == WL_CONNECTED && wifiNow != WL_CONNECTED) {
    mc_logf("[WIFI] disconnected (status=%d) -> reset TTS session", (int)wifiNow);
    g_tts.requestSessionReset();
  }
  s_prevWifi = wifiNow;

  // --- 入力検�E�E��Eタン + タチE���E�E---
  bool anyInput = false;


  // ☁EwasPressed() は「読んだ瞬間に消費」されるので、忁E��一度だけ読む
  const bool btnA = M5.BtnA.wasPressed();
  const bool btnB = M5.BtnB.wasPressed();
  const bool btnC = M5.BtnC.wasPressed();
  if (btnA || btnB || btnC) {
    anyInput = true;
    g_suppressTouchBeepOnce = true;   // ☁E次のUI更新でタチE��開始ビープを1回だけ抑止
  }


  // タチE��入力（短タチE�Eも拾えるように「押された瞬間」を検�E�E�E
  static bool prevTouchPressed = false;
  bool touchPressed = false;
  bool touchDown    = false;

  int touchX = 0;
  int touchY = 0;

  auto& tp = M5.Touch;
  // Touch poll can occasionally hang if hit too frequently on some Core2 units.
  // Poll at a modest rate (e.g. 25ms) and reuse the cached values.
  static uint32_t s_lastTouchPollMs = 0;
  static int s_touchX = 0;
  static int s_touchY = 0;
  static bool s_touchPressed = false;

  if (tp.isEnabled()) {
    if ((uint32_t)(now - s_lastTouchPollMs) >= 25) {
      s_lastTouchPollMs = now;
      auto det = tp.getDetail();
      s_touchPressed = det.isPressed();
      if (s_touchPressed) {
        s_touchX = det.x;
        s_touchY = det.y;
      }
    }

    touchPressed = s_touchPressed;
    touchX = s_touchX;
    touchY = s_touchY;

    touchDown = touchPressed && !prevTouchPressed;
    prevTouchPressed = touchPressed;

    if (touchPressed) {
      anyInput = true;
    }
  }

  // Cache touch state for UI (avoid extra I2C touch reads inside UI drawing)
  {
    UIMining::TouchSnapshot ts;
    ts.enabled = tp.isEnabled();
    ts.pressed = touchPressed;
    ts.down    = touchDown;
    ts.x       = touchX;
    ts.y       = touchY;
    UIMining::instance().setTouchSnapshot(ts);
  }




  // --- スリープ中の復帰処琁E---
  if (displaySleeping) {
    if (anyInput) {
      // 何か入力があったら画面ONにして、このフレームは「起きるだけ、E
      mc_logf("[MAIN] display wake (sleep off)");
      M5.Display.setBrightness(DISPLAY_ACTIVE_BRIGHTNESS);
      displaySleeping = false;
      lastInputMs     = now;
    }

    delay(2);
    return;
  }


  // ここから「画面がON」�E時�E処琁E
  UIMining& ui = UIMining::instance();

  // --- Cボタン�E�テスト画面へ�E�トグル�E�E---
  if (btnC) {
    M5.Speaker.tone(1500, 40);

    if (g_mode != MODE_TEST) {
      g_modeBeforeTest = g_mode;

      // Stackchanから離脱するなら後始末�E�Ettention解除も含む�E�E
      if (g_mode == MODE_STACKCHAN) {
        ui.onLeaveStackchanMode();

        if (g_attentionActive) {
          g_attentionActive = false;
          if (g_savedYieldValid) setMiningYieldProfile(g_savedYield);
          ui.triggerAttention(0);
        }
      }

      g_mode = MODE_TEST;
      g_pauseMiningForTest = true;
      g_test.enter(now);
      g_testUi.markDirty();
      mc_logf("[MAIN] BtnC pressed, enter TEST mode");

    } else {
      g_pauseMiningForTest = false;
      g_test.exit(now);
      g_mode = g_modeBeforeTest;
      g_testUi.markDirty();
      mc_logf("[MAIN] BtnC pressed, exit TEST mode -> mode=%d", (int)g_mode);

      if (g_mode == MODE_STACKCHAN) {
        ui.onEnterStackchanMode();
      }
    }
  }

  // --- Test mode�E�テスト画面だけ回して return ---
  if (g_mode == MODE_TEST) {
    TestInput tin;
    tin.btnA = btnA; tin.btnB = btnB; tin.btnC = btnC;
    tin.touchDown = touchDown;
    tin.touchX = touchX;
    tin.touchY = touchY;

    g_test.update(now, tin);
    g_testUi.draw(now, g_test.state());

    delay(2);
    return;
  }


  // Bボタン�E�固定文を喋る（まず�E動作確認用�E�E
  if (btnB) {
    const char* text = "Hello from Mining Stackchan.";
    if (!g_tts.speakAsync(text)) {
      mc_logf("[TTS] speakAsync failed (busy / wifi / config?)");
    }
  }



  if (anyInput) {
    lastInputMs = now;
  }


  // --- Aボタン�E�ダチE��ュボ�EチE<-> スタチE��チャン + ビ�EチE---
  if (btnA) {
    M5.Speaker.tone(1500, 50);

    if (g_mode == MODE_DASH) {
      g_mode = MODE_STACKCHAN;
      ui.onEnterStackchanMode();
    } else if (g_mode == MODE_STACKCHAN) {
      g_mode = MODE_DASH;
      ui.onLeaveStackchanMode();

      // Leaving stackchan -> clear attention + restore mining yield
      if (g_attentionActive) {
        g_attentionActive = false;
        if (g_savedYieldValid) setMiningYieldProfile(g_savedYield);
        ui.triggerAttention(0);
      }
    }

    mc_logf("[MAIN] BtnA pressed, mode=%d", (int)g_mode);
  }


  // --- Attention mode: tap in Stackchan screen to go "WHAT?" and throttle mining ---
  // NOTE: Right now we only apply "strong yield" (so mining continues but UI becomes very responsive).
  // Future hooks:
  //   - STOP: setMiningActiveThreads(0)
  //   - HALF: setMiningActiveThreads(1)


  // --- 4)C: pending があれ�E Azure TTS を開始（取りこぼしOK�E�最新優先！E---
  

  if ((g_mode == MODE_STACKCHAN) && touchDown && !displaySleeping) {
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



  // --- 起動時の WiFi 接綁E& NTP 同期�E�ノンブロチE��ング�E�E---
  bool wifiDone = wifi_connect();
  if (wifiDone && !g_timeNtpDone && WiFi.status() == WL_CONNECTED) {
    setupTimeNTP();
    g_timeNtpDone = true;
  }


  // --- UI 更新�E�E00ms ごとに1回！E---
  if (now - lastUiMs >= 100) {
    lastUiMs = now;

    MiningSummary summary;
    updateMiningSummary(summary);

    // (ui is already referenced above)
    UIMining::PanelData data;
    buildPanelData(summary, ui, data);

    g_behavior.update(data, now);

    StackchanReaction reaction;
    if (g_behavior.popReaction(&reaction)) {
      LOG_EVT_INFO("EVT_PRESENT_POP",
                   "rid=%lu type=%d prio=%d speak=%d busy=%d mode=%d attn=%d",
                   (unsigned long)reaction.rid, (int)reaction.evType, (int)reaction.priority,
                   reaction.speak ? 1 : 0, ttsBusyNow ? 1 : 0, (int)g_mode, g_attentionActive ? 1 : 0);

      const bool suppressedByAttention = (g_mode == MODE_STACKCHAN) && g_attentionActive;

      // IdleTick は「何も起きてない」扱いにして、Avatarには触らない（フリーズ回避）
      const bool isIdleTick = (reaction.evType == StackchanEventType::IdleTick);

      bool appliedToUi = false;

      if (g_mode == MODE_STACKCHAN) {
        if (!isIdleTick) {
          // expression は変化がある時だけ反映（無駄打ち & ハング率を下げる）
          static bool s_hasLastExp = false;
          static m5avatar::Expression s_lastExp = m5avatar::Expression::Neutral;

          if (!s_hasLastExp || reaction.expression != s_lastExp) {
            ui.setStackchanExpression(reaction.expression);
            s_lastExp = reaction.expression;
            s_hasLastExp = true;
          }

          // speak=0 のイベントでは speech を触らない（安全策）
          if (reaction.speak && reaction.speechText.length()) {
            ui.setStackchanSpeech(reaction.speechText);
          }

          appliedToUi = true;
        } else {
          // IdleTick: UI側に任せる
          appliedToUi = false;
        }
      } else {
        // Dashboard中はStackchan avatar を触らない
        appliedToUi = false;
      }

      LOG_EVT_INFO("EVT_PRESENT_UI_APPLY",
                   "rid=%lu type=%d prio=%d speak=%d suppressed_by_attention=%d applied=%d",
                   (unsigned long)reaction.rid, (int)reaction.evType, (int)reaction.priority,
                   reaction.speak ? 1 : 0, suppressedByAttention ? 1 : 0, appliedToUi ? 1 : 0);

      // TTS は speak=1 かつ textあり の時だけ
      if (reaction.speak && reaction.speechText.length()) {
        if (!ttsBusyNow) {
          uint32_t ttsId = g_ttsIdCounter++;
          if (g_ttsIdCounter == 0) g_ttsIdCounter = 1;
          g_ttsInflightId  = ttsId;
          g_ttsInflightRid = reaction.rid;
          g_tts.speakAsync(reaction.speechText);
          LOG_EVT_INFO("EVT_PRESENT_TTS_START",
                       "rid=%lu tts_id=%lu type=%d prio=%d busy=%d mode=%d attn=%d",
                       (unsigned long)reaction.rid, (unsigned long)ttsId,
                       (int)reaction.evType, (int)reaction.priority,
                       ttsBusyNow ? 1 : 0, (int)g_mode, g_attentionActive ? 1 : 0);
        } else {
          LOG_EVT_INFO("EVT_PRESENT_TTS_SKIP_BUSY",
                       "rid=%lu type=%d prio=%d busy=%d mode=%d attn=%d",
                       (unsigned long)reaction.rid, (int)reaction.evType, (int)reaction.priority,
                       ttsBusyNow ? 1 : 0, (int)g_mode, g_attentionActive ? 1 : 0);
        }
      }

    } else {
      // No reaction popped: treat as "presenter heartbeat" (not an error / not an event)
      // Log only on state-change and with a low-rate heartbeat.
      static uint32_t s_lastHbMs = 0;
      static uint32_t s_emptyStreak = 0;
      s_emptyStreak++;

      const uint32_t PRESENTER_HEARTBEAT_MS = 10000;  // 10sに1回だけ心拍
      bool stateChanged = (ttsBusyNow != g_lastPopEmptyBusy) ||
                          (g_mode != g_lastPopEmptyMode) ||
                          (g_attentionActive != g_lastPopEmptyAttn);

      if (stateChanged || (now - s_lastHbMs) >= PRESENTER_HEARTBEAT_MS) {
        LOG_EVT_DEBUG("EVT_PRESENT_HEARTBEAT",
                      "busy=%d mode=%d attn=%d empty_streak=%lu",
                      ttsBusyNow ? 1 : 0, (int)g_mode, g_attentionActive ? 1 : 0,
                      (unsigned long)s_emptyStreak);

        s_lastHbMs = now;
        s_emptyStreak = 0;

        // keep last state for change detection
        g_lastPopEmptyBusy = ttsBusyNow;
        g_lastPopEmptyMode = g_mode;
        g_lastPopEmptyAttn = g_attentionActive;
      }
    }


    // ticker は Step1 の buildTicker(summary) のまま
    String ticker = buildTicker(summary);

    // ☁EBtnA/B/C が反応してぁE��ら、次の drawAll 1回だけタチE��開始ビープを抑止
    const bool suppressTouchBeep = g_suppressTouchBeepOnce;
    g_suppressTouchBeepOnce = false;

    // ☁Eここで画面を�Eり替ぁE
    if ((g_mode == MODE_STACKCHAN)) {
      ui.drawStackchanScreen(data);   // スタチE��チャン画面      // (Phase2) legacy pending path disabled

    } else {
      ui.drawAll(data, ticker);       // ダチE��ュボ�Eド画面
    }
  }

  // --- 一定時間無操作なら画面OFF�E��Eイニングは継続！E---
  if (!displaySleeping && (now - lastInputMs >= DISPLAY_SLEEP_TIMEOUT_MS)) {
    mc_logf("[MAIN] display sleep (screen off)");

    // 右パネルだけ「Zzz…」メチE��ージめE秒だけ表示
    UIMining::instance().drawSleepMessage();
    delay(DISPLAY_SLEEP_MESSAGE_MS);  // ☁Eここを定数に

    M5.Display.setBrightness(0);
    displaySleeping = true;
  }

  // ---- TTS中のマイニング負荷制御�E�捨てなぁE���E�E----
  // ・再生中: applyMiningPolicyForTts() ぁEpause する�E�EOB維持E��E
  // ・取得中/準備中: STOP(0)はJOBを捨てめE��ぁE�Eで、ここでは yield 強化に留めめE
  // ・Attention(WHAT?) ぁEyield を管琁E��てぁE��時�E、そちらを優先すめE
  static bool s_ttsYieldApplied = false;
  static MiningYieldProfile s_ttsSavedYield = MiningYieldNormal();
  static bool s_ttsSavedYieldValid = false;

  if (g_tts.isBusy()) {
    if (!s_ttsYieldApplied && !g_attentionActive) {
      s_ttsSavedYield = getMiningYieldProfile();
      s_ttsSavedYieldValid = true;
      setMiningYieldProfile(MiningYieldStrong());
      s_ttsYieldApplied = true;
      mc_logf("[TTS] mining yield: Strong");
    }
  } else {
    if (s_ttsYieldApplied && !g_attentionActive) {
      if (s_ttsSavedYieldValid) {
        setMiningYieldProfile(s_ttsSavedYield);
      } else {
        setMiningYieldProfile(MiningYieldNormal());
      }
      s_ttsYieldApplied = false;
      mc_logf("[TTS] mining yield: restore");
    }
  }



  delay(2);
}









