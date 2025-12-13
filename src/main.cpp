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

// UI 更新用の前回時刻 [ms]
static unsigned long lastUiMs = 0;

// Aボタンで切り替える画面モード
static bool g_stackchanMode = false;

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

  // --- 画面の初期状態 ---
  M5.Display.setBrightness(DISPLAY_ACTIVE_BRIGHTNESS);
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextColor(WHITE, BLACK);

  const auto& cfg = appConfig();

  // ★ UI起動 & スプラッシュ表示
  UIMining::instance().begin(cfg.app_name, cfg.app_version);

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

  // --- 入力検出（ボタン + タッチ） ---
  bool anyInput = false;

  // 物理ボタン
  if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed()) {
    anyInput = true;
  }

  // タッチ入力（押されている間は true）
  auto& tp = M5.Touch;
  if (tp.isEnabled()) {
    auto det = tp.getDetail();
    if (det.isPressed()) {
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

  if (anyInput) {
    lastInputMs = now;
  }

  // --- Aボタン：画面モード切り替え + ビープ ---
  // （スタックチャン画面に入る時も必ずピッと鳴る）
  if (M5.BtnA.wasPressed()) {
    M5.Speaker.tone(1500, 50);  // 他の場所と同じ 1500Hz / 50ms

    g_stackchanMode = !g_stackchanMode;
    mc_logf("[MAIN] BtnA pressed, stackchanMode=%d", (int)g_stackchanMode);

    UIMining& ui = UIMining::instance();
    if (g_stackchanMode) {
      ui.onEnterStackchanMode();
    } else {
      ui.onLeaveStackchanMode();
    }
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

    UIMining& ui = UIMining::instance();
    UIMining::PanelData data;
    buildPanelData(summary, ui, data);

    // ticker は Step1 の buildTicker(summary) のまま
    String ticker = buildTicker(summary);


        // ★ WiFi 診断メッセージ
    {
      wl_status_t st = WiFi.status();
      switch (st) {
        case WL_CONNECTED:
          data.wifiDiag = "WiFi connection is OK";
          break;
        case WL_NO_SSID_AVAIL:
          data.wifiDiag = "SSID not found. Check the AP name and power.";
          break;
        case WL_CONNECT_FAILED:
          data.wifiDiag = "Check the WiFi password and encryption settings.";
          break;
        default:
          data.wifiDiag = "Check your router and signal strength.";
          break;
      }
    }

    // ★ Pool 診断メッセージ（mining_task から）
    data.poolDiag = summary.poolDiag;
    
    //String ticker = buildTicker(summary);


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



  delay(2);
}
