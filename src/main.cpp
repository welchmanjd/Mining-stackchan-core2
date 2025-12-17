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

#include "app_test_mode.h"
#include "ui_test_core2.h"


// UI 更新用の前回時刻 [ms]
static unsigned long lastUiMs = 0;

// 画面モードと切り替え
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

// Test中はマイニングを強制pauseしたい（TTS再生中pauseとORで統合）
static bool g_pauseMiningForTest = false;


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

// BtnA/B/C の押下に伴う『タッチ開始ビープ』を次のUI更新で1回だけ抑止する
static bool g_suppressTouchBeepOnce = false;


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

// ---- TTS中のマイニング制御（捨てない pause 版） ----
// ポイント：スレッド数を 0 にしない（JOBを捨てない）
// 再生中だけ「pause」して、終わったら再開する
static bool s_pausedByTts = false;

static void applyMiningPolicyForTts(bool ttsBusy) {
  (void)ttsBusy;

  const bool speaking = M5.Speaker.isPlaying();
  const bool wantPause = speaking || g_pauseMiningForTest;

  if (wantPause != s_pausedByTts) {
    mc_logf("[TTS] mining pause: %d -> %d (speaking=%d test=%d)",
            (int)s_pausedByTts, (int)wantPause, (int)speaking, (int)g_pauseMiningForTest);

    setMiningPaused(wantPause);      // ★ mining_task 側で実装（後述）
    s_pausedByTts = wantPause;
  }
}

//TTS用
static uint32_t g_lastSpeechSeq = 0;
static String   g_ttsPending;
static uint32_t g_ttsLastTryMs = 0;




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

  //テスト枠を初期化
  g_test.begin(&g_tts);
  g_testUi.begin();

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


  // ★ wasPressed() は「読んだ瞬間に消費」されるので、必ず一度だけ読む
  const bool btnA = M5.BtnA.wasPressed();
  const bool btnB = M5.BtnB.wasPressed();
  const bool btnC = M5.BtnC.wasPressed();
  if (btnA || btnB || btnC) {
    anyInput = true;
    g_suppressTouchBeepOnce = true;   // ★ 次のUI更新でタッチ開始ビープを1回だけ抑止
  }


  // タッチ入力（短タップも拾えるように「押された瞬間」を検出）
  static bool prevTouchPressed = false;
  bool touchPressed = false;
  bool touchDown    = false;

  int touchX = 0;
  int touchY = 0;

  auto& tp = M5.Touch;
  if (tp.isEnabled()) {
    auto det = tp.getDetail();
    touchPressed = det.isPressed();
    touchDown    = touchPressed && !prevTouchPressed;
    prevTouchPressed = touchPressed;
    if (touchPressed) {
      touchX = det.x;
      touchY = det.y;
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
  UIMining& ui = UIMining::instance();

  // --- Cボタン：テスト画面へ（トグル） ---
  if (btnC) {
    M5.Speaker.tone(1500, 40);

    if (g_mode != MODE_TEST) {
      g_modeBeforeTest = g_mode;

      // Stackchanから離脱するなら後始末（Attention解除も含む）
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

  // --- Test mode：テスト画面だけ回して return ---
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


  // Bボタン：固定文を喋る（まずは動作確認用）
  if (btnB) {
    const char* text = "こんにちはマイニングスタックチャンです。";
    if (!g_tts.speakAsync(text)) {
      mc_logf("[TTS] speakAsync failed (busy / wifi / config?)");
    }
  }



  if (anyInput) {
    lastInputMs = now;
  }


  // --- Aボタン：ダッシュボード <-> スタックチャン + ビープ ---
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


  // --- 4)C: pending があれば Azure TTS を開始（取りこぼしOK：最新優先） ---
  if ((g_mode == MODE_STACKCHAN) && g_ttsPending.length() > 0 && !g_tts.isBusy()) {
    const uint32_t nowMs = millis();

    // 失敗で詰まらないように、最低 500ms 間隔で再試行
    if (g_ttsLastTryMs == 0 || (uint32_t)(nowMs - g_ttsLastTryMs) >= 500) {
      g_ttsLastTryMs = nowMs;

      if (g_tts.speakAsync(g_ttsPending)) {
        mc_logf("[TTS] speak bubble (seq=%u)", (unsigned)g_lastSpeechSeq);
        g_ttsPending = "";  // ★方針通り：喋り始めたら捨てる
      } else {
        mc_logf("[TTS] speakAsync failed (busy / wifi / config?)");
        // pending は残るので、次のチャンスで再試行される
      }
    }
  }


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

    // ★ BtnA/B/C が反応していたら、次の drawAll 1回だけタッチ開始ビープを抑止
    const bool suppressTouchBeep = g_suppressTouchBeepOnce;
    g_suppressTouchBeepOnce = false;

    // ★ ここで画面を切り替え
    if ((g_mode == MODE_STACKCHAN)) {
      ui.drawStackchanScreen(data);   // スタックチャン画面

      // --- 4)B: 吹き出し更新を検知して pending に積む ---
      uint32_t seq = ui.stackchanSpeechSeq();
      if (seq != 0 && seq != g_lastSpeechSeq) {
        g_lastSpeechSeq = seq;
        g_ttsPending    = ui.stackchanSpeechText();
        g_ttsLastTryMs  = 0;  // すぐ喋りに行きたいのでリトライ待ちリセット
        mc_logf("[TTS] bubble pending (seq=%u) %s", (unsigned)seq, g_ttsPending.c_str());
      }

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

  // ---- TTS中のマイニング負荷制御（捨てない版） ----
  // ・再生中: applyMiningPolicyForTts() が pause する（JOB維持）
  // ・取得中/準備中: STOP(0)はJOBを捨てやすいので、ここでは yield 強化に留める
  // ・Attention(WHAT?) が yield を管理している時は、そちらを優先する
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
